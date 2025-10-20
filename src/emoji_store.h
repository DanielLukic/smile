#pragma once

#include <glib-object.h>

typedef struct {
  char *emoji;
  char *hexcode;
  char *group;
  char *search_text;
  char *primary_name;
  guint order;
} SmileEmoji;

typedef struct {
  GPtrArray *all;
} EmojiStore;

EmojiStore *emoji_store_new(const char *json_path, GError **error);
EmojiStore *emoji_store_new_from_resource(const char *resource_path, GError **error);
void        emoji_store_free(EmojiStore *store);

GPtrArray *emoji_store_get_all(const EmojiStore *store);
GPtrArray *emoji_store_filter(const EmojiStore *store, const char *query);
