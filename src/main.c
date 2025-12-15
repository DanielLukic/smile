#include <gio/gio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include "emoji_store.h"

extern GResource *emoji_res_get_resource(void);

#define DEFAULT_EMOJI_DATA_PATH "data/emoji.json"
#define EMBEDDED_EMOJI_RESOURCE "/it/mijorus/smile/emoji.json"
#define INITIAL_EMOJI_CHUNK 200
#define EMOJI_CHUNK_SIZE 150
#define SCROLL_THRESHOLD_PX 300.0
#define RECENTS_LIMIT 20

typedef struct {
  GtkApplication *app;
  EmojiStore *store;
  GPtrArray  *current_results;
  GPtrArray  *recents; /* array of RecentItem* */
  GtkWidget  *flowbox;
  GtkWidget  *status_label;
  GtkWidget  *search_entry;
  GtkWidget  *window;
  guint       loaded_count;
  gint        selected_index;
  guint64     use_counter;
} SmileAppState;

typedef struct {
  SmileEmoji *emoji;
  char       *hexcode;
  guint       count;
  guint64     last_used;
} RecentItem;

static void populate_flowbox(SmileAppState *state, GPtrArray *items);
static void emoji_button_clicked(GtkButton *button, gpointer user_data);
static GPtrArray *build_results_for_query(SmileAppState *state, const char *text);
static void recents_dump(SmileAppState *state, const char *reason);

static void
reset_search_and_results(SmileAppState *state)
{
  if (!state || !state->store || !state->search_entry) {
    return;
  }

  gtk_entry_set_text(GTK_ENTRY(state->search_entry), "");
  GPtrArray *all = build_results_for_query(state, "");
  populate_flowbox(state, all);
}

static void
ensure_selection_css(void)
{
  static gboolean css_loaded = FALSE;
  if (css_loaded) {
    return;
  }

  GtkCssProvider *provider = gtk_css_provider_new();
  const gchar *css =
    ".emoji-selected {"
    "  background-color: @theme_selected_bg_color;"
    "  color: @theme_selected_fg_color;"
    "}";
  gtk_css_provider_load_from_data(provider, css, -1, NULL);

  GdkScreen *screen = gdk_screen_get_default();
  if (screen) {
    gtk_style_context_add_provider_for_screen(
      screen,
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  g_object_unref(provider);
  css_loaded = TRUE;
}

static GtkWidget *
get_button_for_index(SmileAppState *state, gint index)
{
  GtkFlowBoxChild *child = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(state->flowbox), index);
  if (!child) {
    return NULL;
  }
  return gtk_bin_get_child(GTK_BIN(child));
}

static void
set_button_selected(GtkWidget *button, gboolean selected)
{
  if (!button) {
    return;
  }
  GtkStyleContext *ctx = gtk_widget_get_style_context(button);
  if (selected) {
    gtk_style_context_add_class(ctx, "emoji-selected");
  } else {
    gtk_style_context_remove_class(ctx, "emoji-selected");
  }
}

static GtkWidget *
create_emoji_button(SmileAppState *state, SmileEmoji *entry)
{
  GtkWidget *button = gtk_button_new_with_label(entry->emoji);
  gtk_widget_set_hexpand(button, FALSE);
  gtk_widget_set_vexpand(button, FALSE);
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(button, 40, 40);
  gtk_widget_set_margin_start(button, 1);
  gtk_widget_set_margin_end(button, 1);
  gtk_widget_set_margin_top(button, 1);
  gtk_widget_set_margin_bottom(button, 1);

  g_object_set_data(G_OBJECT(button), "emoji-entry", entry);
  g_signal_connect(button, "clicked", G_CALLBACK(emoji_button_clicked), state);
  return button;
}

static void
recent_item_free(RecentItem *item)
{
  if (!item) {
    return;
  }
  g_clear_pointer(&item->hexcode, g_free);
  g_free(item);
}

static SmileEmoji *
find_emoji_by_hexcode(const EmojiStore *store, const char *hexcode)
{
  if (!store || !hexcode) {
    return NULL;
  }
  for (guint i = 0; i < store->all->len; i++) {
    SmileEmoji *e = g_ptr_array_index(store->all, i);
    if (g_strcmp0(e->hexcode, hexcode) == 0) {
      return e;
    }
  }
  return NULL;
}

static gint
calculate_columns(SmileAppState *state)
{
  if (!state->current_results || state->current_results->len == 0) {
    return 1;
  }

  GtkFlowBox *box = GTK_FLOW_BOX(state->flowbox);
  gint columns = 0;
  gint base_y = -1;

  guint max_check = MIN(state->loaded_count, state->current_results->len);
  for (guint i = 0; i < max_check; i++) {
    GtkFlowBoxChild *child = gtk_flow_box_get_child_at_index(box, i);
    if (!child) {
      break;
    }

    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(child), &alloc);
    if (base_y == -1) {
      base_y = alloc.y;
    }

    if (alloc.y > base_y + alloc.height / 2) {
      break;
    }

    columns += 1;
  }

  if (columns <= 0) {
    columns = 1;
  }

  return columns;
}

static gboolean
ensure_loaded_up_to(SmileAppState *state, guint min_count)
{
  if (!state->current_results) {
    return FALSE;
  }

  GtkContainer *container = GTK_CONTAINER(state->flowbox);
  gboolean added = FALSE;

  guint target = MIN(min_count, state->current_results->len);
  for (guint i = state->loaded_count; i < target; i++) {
    SmileEmoji *entry = g_ptr_array_index(state->current_results, i);
    GtkWidget *button = create_emoji_button(state, entry);
    gtk_container_add(container, button);
    gtk_widget_show(button);
    state->loaded_count += 1;
    added = TRUE;
  }

  return added;
}

static char *
recents_path(void)
{
  const char *data_dir = g_get_user_data_dir();
  return g_build_filename(data_dir, "smile", "recents.json", NULL);
}

static void
recents_save(SmileAppState *state)
{
  if (!state || !state->recents) {
    return;
  }

  g_autofree char *path = recents_path();
  g_autofree char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);

   g_print("Smile: saving recents to %s (items: %u)\n", path, state->recents->len);
   recents_dump(state, "save");

  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  for (guint i = 0; i < state->recents->len; i++) {
    RecentItem *item = g_ptr_array_index(state->recents, i);
    if (!item || !item->hexcode) {
      continue;
    }
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "hexcode");
    json_builder_add_string_value(b, item->hexcode);
    json_builder_set_member_name(b, "count");
    json_builder_add_int_value(b, item->count);
    json_builder_set_member_name(b, "last_used");
    json_builder_add_int_value(b, (gint64) item->last_used);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_to_file(gen, path, NULL);
  json_node_free(root);
  g_object_unref(b);
}

static void
recents_load(SmileAppState *state)
{
  g_return_if_fail(state);

  if (state->recents) {
    g_ptr_array_free(state->recents, TRUE);
  }
  state->recents = g_ptr_array_new_with_free_func((GDestroyNotify) recent_item_free);

  g_autofree char *path = recents_path();
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_print("Smile: recents file not found (%s), starting fresh\n", path);
    return;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_file(parser, path, NULL)) {
    g_print("Smile: failed to load recents file %s\n", path);
    return;
  }
  g_print("Smile: loaded recents from %s\n", path);

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    return;
  }

  JsonArray *arr = json_node_get_array(root);
  guint len = json_array_get_length(arr);
  for (guint i = 0; i < len; i++) {
    JsonObject *obj = json_array_get_object_element(arr, i);
    if (!obj) {
      continue;
    }
    if (!json_object_has_member(obj, "hexcode")) {
      continue;
    }
    const char *hex = json_object_get_string_member(obj, "hexcode");
    guint count = 0;
    guint64 last_used = 0;
    if (json_object_has_member(obj, "count")) {
      count = (guint) json_object_get_int_member(obj, "count");
    }
    if (json_object_has_member(obj, "last_used")) {
      last_used = (guint64) json_object_get_int_member(obj, "last_used");
    }
    RecentItem *item = g_new0(RecentItem, 1);
    item->hexcode = g_strdup(hex);
    item->emoji = find_emoji_by_hexcode(state->store, hex);
    item->count = count;
    item->last_used = last_used;
    g_ptr_array_add(state->recents, item);
    if (item->last_used > state->use_counter) {
      state->use_counter = item->last_used;
    }
  }
  /* Trim to RECENTS_LIMIT; keep file order (already most recent first). */
  while (state->recents->len > RECENTS_LIMIT) {
    g_ptr_array_remove_index(state->recents, state->recents->len - 1);
  }
  recents_dump(state, "after load");
}

static void
recents_record_use(SmileAppState *state, SmileEmoji *emoji)
{
  if (!state || !emoji || !emoji->hexcode) {
    return;
  }
  if (!state->recents) {
    state->recents = g_ptr_array_new_with_free_func((GDestroyNotify) recent_item_free);
  }

  /* Remove existing entry if present. */
  for (guint i = 0; i < state->recents->len; i++) {
    RecentItem *item = g_ptr_array_index(state->recents, i);
    if (g_strcmp0(item->hexcode, emoji->hexcode) == 0) {
      g_ptr_array_remove_index(state->recents, i);
      break;
    }
  }

  /* Evict oldest if at limit. */
  if (state->recents->len >= RECENTS_LIMIT) {
    g_ptr_array_remove_index(state->recents, state->recents->len - 1);
  }

  RecentItem *item = g_new0(RecentItem, 1);
  item->hexcode = g_strdup(emoji->hexcode);
  item->emoji = emoji;
  item->count = 1;
  item->last_used = ++state->use_counter;
  /* Prepend to keep most recent first without needing to sort. */
  g_ptr_array_insert(state->recents, 0, item);

  recents_dump(state, "after record_use");
  recents_save(state);
}

static void
recents_dump(SmileAppState *state, const char *reason)
{
  if (!state || !state->recents) {
    return;
  }
  g_print("Smile: recents (%s)\n", reason ? reason : "");
  for (guint i = 0; i < state->recents->len; i++) {
    RecentItem *item = g_ptr_array_index(state->recents, i);
    const char *hex = item && item->hexcode ? item->hexcode : "(null)";
    const char *emo = (item && item->emoji && item->emoji->emoji) ? item->emoji->emoji : "?";
    g_print("  %u %s (%s) count=%u last=%" G_GUINT64_FORMAT "\n",
            i, emo, hex,
            item ? item->count : 0,
            item ? item->last_used : 0);
  }
}

static void
on_adjustment_value_changed(GtkAdjustment *adj, gpointer user_data)
{
  SmileAppState *state = user_data;
  gdouble value = gtk_adjustment_get_value(adj);
  gdouble upper = gtk_adjustment_get_upper(adj);
  gdouble page = gtk_adjustment_get_page_size(adj);

  if (value + page + SCROLL_THRESHOLD_PX >= upper) {
    ensure_loaded_up_to(state, state->loaded_count + EMOJI_CHUNK_SIZE);
  }
}

static GPtrArray *
build_results_for_query(SmileAppState *state, const char *text)
{
  if (!text || !*text) {
    GPtrArray *combined = g_ptr_array_new();
    if (state->recents) {
      for (guint i = 0; i < state->recents->len; i++) {
        RecentItem *item = g_ptr_array_index(state->recents, i);
        if (item->emoji) {
          g_ptr_array_add(combined, item->emoji);
        }
      }
    }
    GPtrArray *all = emoji_store_get_all(state->store);
    for (guint i = 0; i < all->len; i++) {
      g_ptr_array_add(combined, g_ptr_array_index(all, i));
    }
    g_ptr_array_unref(all);
    return combined;
  }

  GPtrArray *results = emoji_store_filter(state->store, text);
  if (!state->recents || state->recents->len == 0) {
    return results;
  }

  /* Promote matching recents to the front, keep filter order for the rest. */
  GPtrArray *reordered = g_ptr_array_new();
  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);

  for (guint i = 0; i < state->recents->len; i++) {
    RecentItem *item = g_ptr_array_index(state->recents, i);
    if (!item || !item->emoji) {
      continue;
    }
    for (guint j = 0; j < results->len; j++) {
      SmileEmoji *res = g_ptr_array_index(results, j);
      if (res == item->emoji) {
        g_ptr_array_add(reordered, res);
        g_hash_table_insert(seen, res, res);
        break;
      }
    }
  }

  for (guint j = 0; j < results->len; j++) {
    SmileEmoji *res = g_ptr_array_index(results, j);
    if (!g_hash_table_contains(seen, res)) {
      g_ptr_array_add(reordered, res);
    }
  }

  g_hash_table_unref(seen);
  g_ptr_array_unref(results);
  return reordered;
}

static void
update_selection(SmileAppState *state, gint new_index)
{
  guint len = state->current_results ? state->current_results->len : 0;
  if (len == 0) {
    state->selected_index = -1;
    return;
  }

  if (new_index < 0) {
    new_index = 0;
  }
  if ((guint) new_index >= len) {
    new_index = (gint) len - 1;
  }

  if (state->selected_index >= 0 && (guint) state->selected_index < len) {
    GtkWidget *prev = get_button_for_index(state, state->selected_index);
    set_button_selected(prev, FALSE);
  }

  if ((guint) new_index >= state->loaded_count) {
    ensure_loaded_up_to(state, (guint) new_index + 1);
  }

  GtkWidget *button = get_button_for_index(state, new_index);
  set_button_selected(button, TRUE);
  state->selected_index = new_index;
}

static void
smile_app_state_free(SmileAppState *state)
{
  if (!state) {
    return;
  }

  if (state->current_results) {
    g_ptr_array_unref(state->current_results);
  }

  if (state->recents) {
    g_ptr_array_free(state->recents, TRUE);
  }

  emoji_store_free(state->store);
  g_free(state);
}

static EmojiStore *
load_emoji_store(GError **error)
{
  GError *local_error = NULL;
  EmojiStore *store = emoji_store_new_from_resource(EMBEDDED_EMOJI_RESOURCE, &local_error);
  if (store) {
    return store;
  }
  g_clear_error(&local_error);

  const char *override = g_getenv("SMILE_EMOJI_PATH");
  if (override && *override) {
    store = emoji_store_new(override, &local_error);
    if (store) {
      return store;
    }
    g_clear_error(&local_error);
  }

  return emoji_store_new(DEFAULT_EMOJI_DATA_PATH, error);
}

static SmileAppState *
smile_app_state_get(GtkApplication *app, GError **error)
{
  SmileAppState *state = g_object_get_data(G_OBJECT(app), "smile-app-state");
  if (state) {
    return state;
  }

  state = g_new0(SmileAppState, 1);
  state->app = app;
  state->selected_index = -1;

  state->store = load_emoji_store(error);
  if (!state->store) {
    g_free(state);
    return NULL;
  }
  state->use_counter = 0;
  recents_load(state);

  g_object_set_data_full(G_OBJECT(app), "smile-app-state", state, (GDestroyNotify) smile_app_state_free);
  return state;
}

static void
copy_and_close(SmileAppState *state, SmileEmoji *entry)
{
  if (!entry) {
    return;
  }

  GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

  /* Tell clipboard managers exactly what to persist. */
  const GtkTargetEntry targets[] = {
    { (gchar *) "UTF8_STRING", 0, 0 },
    { (gchar *) "TEXT", 0, 0 },
    { (gchar *) "STRING", 0, 0 },
  };
  gtk_clipboard_set_can_store(clipboard, targets, G_N_ELEMENTS(targets));

  gtk_clipboard_set_text(clipboard, entry->emoji, -1);
  gtk_clipboard_store(clipboard);
  g_print("Smile: copied emoji '%s' (hex: %s) to CLIPBOARD\n",
          entry->emoji ? entry->emoji : "(null)",
          entry->hexcode ? entry->hexcode : "(null)");

  gchar *verify = gtk_clipboard_wait_for_text(clipboard);
  if (verify) {
    g_print("Smile: CLIPBOARD now reads '%s'\n", verify);
    g_free(verify);
  } else {
    g_print("Smile: CLIPBOARD readback is NULL\n");
  }

  /* Also set PRIMARY for apps that paste from the primary selection. */
  GtkClipboard *primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
  gtk_clipboard_set_text(primary, entry->emoji, -1);
  g_print("Smile: copied emoji '%s' to PRIMARY\n",
          entry->emoji ? entry->emoji : "(null)");

  recents_record_use(state, entry);

  /* Keep the app running; just hide the window now. */
  GtkWindow *window = gtk_application_get_active_window(state->app);
  if (window) {
    gtk_widget_hide(GTK_WIDGET(window));
  }
}

static void
emoji_button_clicked(GtkButton *button, gpointer user_data)
{
  SmileAppState *state = user_data;
  (void) state;

  SmileEmoji *entry = g_object_get_data(G_OBJECT(button), "emoji-entry");
  if (!entry) {
    return;
  }

  copy_and_close(state, entry);
}

static void
populate_flowbox(SmileAppState *state, GPtrArray *items)
{
  if (state->current_results == items) {
    g_ptr_array_unref(items);
    return;
  }

  GtkContainer *container = GTK_CONTAINER(state->flowbox);

  GList *children = gtk_container_get_children(container);
  for (GList *l = children; l != NULL; l = l->next) {
    gtk_widget_destroy(GTK_WIDGET(l->data));
  }
  g_list_free(children);

  if (state->current_results) {
    g_ptr_array_unref(state->current_results);
  }
  state->current_results = items;
  state->loaded_count = 0;
  state->selected_index = -1;

  /* Show initial chunk; more are lazily added on scroll / navigation. */
  ensure_loaded_up_to(state, INITIAL_EMOJI_CHUNK);

  if (items->len == 0) {
    gtk_label_set_text(GTK_LABEL(state->status_label), "No matches found");
    gtk_widget_show(state->status_label);
  } else {
    gtk_widget_hide(state->status_label);
  }

  gtk_widget_show_all(state->flowbox);
  if (state->current_results->len > 0 && state->selected_index >= 0) {
    update_selection(state, state->selected_index);
  }
}

static void
search_entry_changed(GtkSearchEntry *entry, gpointer user_data)
{
  SmileAppState *state = user_data;
  const char *text = gtk_entry_get_text(GTK_ENTRY(entry));

  GPtrArray *results = build_results_for_query(state, text);
  populate_flowbox(state, results);
}

static void
search_entry_activate(GtkSearchEntry *entry, gpointer user_data)
{
  SmileAppState *state = user_data;
  (void) entry;

  if (!state->current_results || state->current_results->len == 0) {
    return;
  }

  gint idx = state->selected_index >= 0 ? state->selected_index : 0;
  SmileEmoji *entry_data = g_ptr_array_index(state->current_results, idx);
  copy_and_close(state, entry_data);
}

static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  SmileAppState *state = user_data;
  (void) widget;

  if (event->keyval == GDK_KEY_Left ||
      event->keyval == GDK_KEY_Right ||
      event->keyval == GDK_KEY_Up ||
      event->keyval == GDK_KEY_Down) {
    guint len = state->current_results ? state->current_results->len : 0;
    if (len == 0) {
      return TRUE;
    }

    gint columns = calculate_columns(state);

    gint index = state->selected_index;
    if (index < 0) {
      update_selection(state, 0);
      return TRUE;
    }

    switch (event->keyval) {
      case GDK_KEY_Left:
        if (index > 0) {
          index -= 1;
        }
        break;
      case GDK_KEY_Right:
        if ((guint) (index + 1) < len) {
          index += 1;
        }
        break;
      case GDK_KEY_Up:
        if (index >= columns) {
          index -= columns;
        } else {
          index = 0;
        }
        break;
      case GDK_KEY_Down:
        if ((guint) (index + columns) < len) {
          index += columns;
        } else {
          index = (gint) len - 1;
        }
        break;
    }

    update_selection(state, index);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_Escape) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(state->search_entry));
    if (text && *text) {
      gtk_entry_set_text(GTK_ENTRY(state->search_entry), "");
      gtk_widget_grab_focus(state->search_entry);
      return TRUE;
    }

    GtkWindow *window = gtk_application_get_active_window(state->app);
    if (window) {
      GtkWidget *focus = gtk_window_get_focus(window);
      if (focus && focus != state->search_entry) {
        gtk_widget_grab_focus(state->search_entry);
        return TRUE;
      }

      gtk_window_close(window);
      return TRUE;
    }
  }

  if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
    if (state->current_results && state->current_results->len > 0) {
      gint idx = state->selected_index >= 0 ? state->selected_index : 0;
      SmileEmoji *entry_data = g_ptr_array_index(state->current_results, idx);
      copy_and_close(state, entry_data);
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean
on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  (void) event;
  SmileAppState *state = user_data;
  gtk_widget_hide(widget);
  if (state && state->search_entry) {
    gtk_entry_set_text(GTK_ENTRY(state->search_entry), "");
  }
  return TRUE; /* stop default destroy */
}

static GtkWidget *
build_content(SmileAppState *state)
{
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(root), 8);

  ensure_selection_css();

  GtkWidget *search = gtk_search_entry_new();
  gtk_widget_set_margin_bottom(search, 4);
  gtk_box_pack_start(GTK_BOX(root), search, FALSE, FALSE, 0);
  g_signal_connect(search, "search-changed", G_CALLBACK(search_entry_changed), state);
  g_signal_connect(search, "changed", G_CALLBACK(search_entry_changed), state);
  g_signal_connect(search, "activate", G_CALLBACK(search_entry_activate), state);
  g_signal_connect(search, "key-press-event", G_CALLBACK(on_key_press), state);
  state->search_entry = search;

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(root), scrolled, TRUE, TRUE, 0);
  g_signal_connect(scrolled, "key-press-event", G_CALLBACK(on_key_press), state);

  GtkWidget *flowbox = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flowbox), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flowbox), 10);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flowbox), 5);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flowbox), 0);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flowbox), 0);
  gtk_widget_set_hexpand(flowbox, TRUE);
  gtk_widget_set_vexpand(flowbox, FALSE);
  gtk_widget_set_valign(flowbox, GTK_ALIGN_START);
  gtk_container_add(GTK_CONTAINER(scrolled), flowbox);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  g_signal_connect(vadj, "value-changed", G_CALLBACK(on_adjustment_value_changed), state);

  GtkWidget *status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(status), 0.5);
  gtk_widget_set_no_show_all(status, TRUE);
  gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);

  state->flowbox = flowbox;
  state->status_label = status;

  GPtrArray *initial = build_results_for_query(state, "");
  populate_flowbox(state, initial);

  return root;
}

static void
on_app_activate(GtkApplication *app, gpointer user_data)
{
  (void) user_data;

  GError *error = NULL;
  SmileAppState *state = smile_app_state_get(app, &error);
  if (!state) {
    g_printerr("Failed to load emoji data: %s\n", error ? error->message : "unknown error");
    g_clear_error(&error);
    return;
  }
  g_print("Smile: recents file path %s\n", recents_path());
  recents_save(state);

  if (state->window) {
    gtk_widget_show(state->window);
    gtk_window_present(GTK_WINDOW(state->window));
    reset_search_and_results(state);
    if (state->search_entry) {
      gtk_widget_grab_focus(state->search_entry);
    }
    return;
  }

  GtkWidget *window = gtk_application_window_new(app);
  state->window = window;
  gtk_window_set_title(GTK_WINDOW(window), "Smile");
  gtk_window_set_default_size(GTK_WINDOW(window), 460, 560);
  g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), state);

  GtkWidget *content = build_content(state);
  gtk_container_add(GTK_CONTAINER(window), content);

  gtk_widget_show_all(window);
  if (state->search_entry) {
    gtk_widget_grab_focus(state->search_entry);
  }
}

int
main(int argc, char *argv[])
{
  g_resources_register(emoji_res_get_resource());

  GtkApplication *app = gtk_application_new("it.mijorus.smile", G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  g_object_unref(app);

  return status;
}
