#include <gio/gio.h>
#include <gtk/gtk.h>

#include "emoji_store.h"

extern GResource *emoji_res_get_resource(void);

#define DEFAULT_EMOJI_DATA_PATH "data/emoji.json"
#define EMBEDDED_EMOJI_RESOURCE "/it/mijorus/smile/emoji.json"

typedef struct {
  GtkApplication *app;
  EmojiStore *store;
  GPtrArray  *current_results;
  GtkWidget  *flowbox;
  GtkWidget  *status_label;
  GtkWidget  *search_entry;
  GtkWidget  *window;
  gint        selected_index;
} SmileAppState;

static void populate_flowbox(SmileAppState *state, GPtrArray *items);

static void
reset_search_and_results(SmileAppState *state)
{
  if (!state || !state->store || !state->search_entry) {
    return;
  }

  gtk_entry_set_text(GTK_ENTRY(state->search_entry), "");
  if (state->current_results) {
    g_ptr_array_unref(state->current_results);
  }
  state->current_results = emoji_store_get_all(state->store);
  populate_flowbox(state, g_ptr_array_ref(state->current_results));
  state->selected_index = -1;
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

static gint
calculate_columns(SmileAppState *state)
{
  if (!state->current_results || state->current_results->len == 0) {
    return 1;
  }

  GtkFlowBox *box = GTK_FLOW_BOX(state->flowbox);
  gint columns = 0;
  gint base_y = -1;

  for (guint i = 0; i < state->current_results->len; i++) {
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
  state->selected_index = -1;

  for (guint i = 0; i < items->len; i++) {
    SmileEmoji *entry = g_ptr_array_index(items, i);
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

    gtk_container_add(container, button);
  }

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

  GPtrArray *results = emoji_store_filter(state->store, text);
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

  GtkWidget *status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(status), 0.5);
  gtk_widget_set_no_show_all(status, TRUE);
  gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);

  state->flowbox = flowbox;
  state->status_label = status;

  GPtrArray *all_entries = emoji_store_get_all(state->store);
  populate_flowbox(state, all_entries);

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
