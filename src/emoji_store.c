#include "emoji_store.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

static void
emoji_entry_free(SmileEmoji *entry)
{
  if (!entry) {
    return;
  }

  g_clear_pointer(&entry->emoji, g_free);
  g_clear_pointer(&entry->hexcode, g_free);
  g_clear_pointer(&entry->group, g_free);
  g_clear_pointer(&entry->search_text, g_free);
  g_clear_pointer(&entry->primary_name, g_free);
  g_free(entry);
}

static SmileEmoji *
emoji_entry_from_json(JsonObject *object)
{
  const char *emoji = json_object_get_string_member(object, "emoji");
  const char *hexcode = json_object_get_string_member(object, "hexcode");
  const char *group = json_object_get_string_member(object, "group");
  const char *subgroups = json_object_get_string_member(object, "subgroups");
  const char *tags = json_object_get_string_member(object, "tags");
  guint order = (guint) json_object_get_int_member(object, "order");

  SmileEmoji *entry = g_new0(SmileEmoji, 1);
  entry->emoji = g_strdup(emoji);
  entry->hexcode = g_strdup(hexcode);
  entry->group = g_strdup(group);
  entry->order = order;

  GString *search = g_string_new(NULL);
  if (hexcode && *hexcode) {
    gchar *lower = g_utf8_strdown(hexcode, -1);
    g_string_append(search, lower);
    g_free(lower);
  }

  gchar *last_name = NULL;
  const char *fields[] = { group, subgroups, tags, NULL };
  for (gint i = 0; fields[i] != NULL; i++) {
    if (!fields[i] || !*fields[i]) {
      continue;
    }

    if (search->len > 0) {
      g_string_append_c(search, ' ');
    }

    gchar **parts = g_strsplit(fields[i], ",", -1);
    for (gint j = 0; parts[j] != NULL; j++) {
      gchar *trimmed = g_strstrip(parts[j]);
      if (!*trimmed) {
        continue;
      }
      gchar *lower = g_utf8_strdown(trimmed, -1);
      if (search->len > 0) {
        g_string_append_c(search, ' ');
      }
      g_string_append(search, lower);
      if (*lower) {
        g_clear_pointer(&last_name, g_free);
        last_name = g_strdup(lower);
      }
      g_free(lower);
    }
    g_strfreev(parts);
  }

  entry->search_text = g_string_free(search, FALSE);
  entry->primary_name = last_name;
  return entry;
}

static EmojiStore *
emoji_store_from_parser(JsonParser *parser, GError **error)
{
  EmojiStore *store = g_new0(EmojiStore, 1);
  store->all = g_ptr_array_new_with_free_func((GDestroyNotify) emoji_entry_free);
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Emoji data root is not an array");
    emoji_store_free(store);
    return NULL;
  }

  JsonArray *array = json_node_get_array(root);
  guint len = json_array_get_length(array);

  for (guint i = 0; i < len; i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    if (!obj) {
      continue;
    }

    SmileEmoji *entry = emoji_entry_from_json(obj);
    g_ptr_array_add(store->all, entry);
  }

  return store;
}

EmojiStore *
emoji_store_new(const char *json_path, GError **error)
{
  g_return_val_if_fail(json_path != NULL, NULL);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_file(parser, json_path, error)) {
    return NULL;
  }

  return emoji_store_from_parser(parser, error);
}

EmojiStore *
emoji_store_new_from_resource(const char *resource_path, GError **error)
{
  g_return_val_if_fail(resource_path != NULL, NULL);

  g_autoptr(GBytes) bytes = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
  if (!bytes) {
    return NULL;
  }

  gsize length = 0;
  const gchar *data = g_bytes_get_data(bytes, &length);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, length, error)) {
    return NULL;
  }

  return emoji_store_from_parser(parser, error);
}

gboolean
emoji_store_append_from_resource(EmojiStore *store, const char *resource_path, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(resource_path != NULL, FALSE);

  g_autoptr(GBytes) bytes = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
  if (!bytes) {
    return FALSE;
  }

  gsize length = 0;
  const gchar *data = g_bytes_get_data(bytes, &length);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, length, error)) {
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Symbols data root is not an array");
    return FALSE;
  }

  JsonArray *array = json_node_get_array(root);
  guint len = json_array_get_length(array);

  for (guint i = 0; i < len; i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    if (!obj) {
      continue;
    }
    SmileEmoji *entry = emoji_entry_from_json(obj);
    g_ptr_array_add(store->all, entry);
  }

  return TRUE;
}

void
emoji_store_free(EmojiStore *store)
{
  if (!store) {
    return;
  }

  if (store->all) {
    g_ptr_array_free(store->all, TRUE);
  }

  g_free(store);
}

GPtrArray *
emoji_store_get_all(const EmojiStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);

  return g_ptr_array_ref(store->all);
}

typedef struct {
  SmileEmoji *entry;
  gint score;
} ScoredEntry;

static gint
score_entry(const SmileEmoji *entry, const gchar *needle)
{
  if (!needle || !*needle) {
    return G_MAXINT;
  }

  gint best = G_MININT;

  if (entry->primary_name && g_strcmp0(entry->primary_name, needle) == 0) {
    best = 2000 - (gint) strlen(entry->primary_name);
  }

  gchar **tokens = g_strsplit(entry->search_text, " ", -1);

  for (gint i = 0; tokens && tokens[i]; i++) {
    const gchar *token = tokens[i];
    if (!token || !*token) {
      continue;
    }

    if (g_strcmp0(token, needle) == 0) {
      gint score = 1500 - (gint) strlen(token);
      if (score > best) {
        best = score;
      }
      continue;
    }

    if (g_str_has_prefix(token, needle)) {
      gint score = 1000 - (gint) strlen(token);
      if (score > best) {
        best = score;
      }
    }

    if (strstr(token, needle)) {
      gint score = 500 - (gint) strlen(token);
      if (score > best) {
        best = score;
      }
    }
  }

  g_strfreev(tokens);

  if (best == G_MININT) {
    return -1;
  }

  return (best * 10000) - (gint) entry->order;
}

static gint
scored_entry_compare(gconstpointer a, gconstpointer b)
{
  const ScoredEntry *lhs = a;
  const ScoredEntry *rhs = b;

  if (lhs->score > rhs->score) {
    return -1;
  }
  if (lhs->score < rhs->score) {
    return 1;
  }

  if (lhs->entry->order < rhs->entry->order) {
    return -1;
  }
  if (lhs->entry->order > rhs->entry->order) {
    return 1;
  }

  return 0;
}

GPtrArray *
emoji_store_filter(const EmojiStore *store, const char *query)
{
  g_return_val_if_fail(store != NULL, NULL);

  if (!query || !*query) {
    return g_ptr_array_ref(store->all);
  }

  gchar *needle = g_utf8_strdown(query, -1);

  GArray *scored = g_array_new(FALSE, FALSE, sizeof(ScoredEntry));
  const GPtrArray *all = store->all;

  for (guint i = 0; i < all->len; i++) {
    SmileEmoji *entry = g_ptr_array_index(all, i);
    gint score = score_entry(entry, needle);
    if (score >= 0) {
      ScoredEntry s = { entry, score };
      g_array_append_val(scored, s);
    }
  }

  g_free(needle);

  g_array_sort(scored, scored_entry_compare);

  GPtrArray *results = g_ptr_array_new();
  for (guint i = 0; i < scored->len; i++) {
    ScoredEntry *s = &g_array_index(scored, ScoredEntry, i);
    g_ptr_array_add(results, s->entry);
  }

  g_array_unref(scored);

  return results;
}
