// -----------------------------------------------------------------------------
// GTK Application setup and activation
// -----------------------------------------------------------------------------
#include "app.hpp"
#include "widgets.hpp"
#include "config.hpp"
#include "dnf_backend.hpp"
#include "ui_helpers.hpp"
#include "base_manager.hpp"

#include <gtk/gtk.h>
#include <libdnf5/rpm/package_query.hpp>

// Forward declarations
static void activate(GtkApplication *app, gpointer user_data);

int
run_dnf_ui(int argc, char **argv)
{
  GtkApplication *app = gtk_application_new("com.fedora.dnfui", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}

// -----------------------------------------------------------------------------
// GTK app setup
// -----------------------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer)
{
  GtkWidget *window = gtk_application_window_new(app);
  // Preload installed package names for highlighting
  {
    auto &base = BaseManager::instance().get_base();
    libdnf5::rpm::PackageQuery query(base);
    query.filter_installed();
    for (auto pkg : query) {
      g_installed_names.insert(pkg.get_name());
    }
  }
  gtk_window_set_title(GTK_WINDOW(window), "DNF Package Viewer");
  load_window_geometry(GTK_WINDOW(window));

  // Keyboard shortcuts: Ctrl+Q and Ctrl+W to close window
  GtkEventController *shortcuts = GTK_EVENT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_widget_add_controller(window, shortcuts);

  auto shortcut_callback = +[](GtkWidget *widget, GVariant *args, gpointer) -> gboolean {
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
  };

  // Ctrl+Q
  GtkShortcut *close_shortcut_q = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_q, GDK_CONTROL_MASK),
                                                   gtk_callback_action_new(shortcut_callback, NULL, NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), close_shortcut_q);

  // Ctrl+W
  GtkShortcut *close_shortcut_w = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_w, GDK_CONTROL_MASK),
                                                   gtk_callback_action_new(shortcut_callback, NULL, NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), close_shortcut_w);

  GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox_root);

  GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_root), outer_paned);
  gtk_paned_set_position(GTK_PANED(outer_paned), 200);

  GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_end_child(GTK_PANED(outer_paned), vbox_main);

  GtkWidget *vbox_history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_vexpand(vbox_history, TRUE);
  gtk_widget_set_hexpand(vbox_history, TRUE);
  gtk_paned_set_start_child(GTK_PANED(outer_paned), vbox_history);

  GtkWidget *history_label = gtk_label_new("Search History");
  gtk_label_set_xalign(GTK_LABEL(history_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_history), history_label);

  GtkWidget *scrolled_history = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled_history, TRUE);
  gtk_widget_set_hexpand(scrolled_history, TRUE);
  gtk_box_append(GTK_BOX(vbox_history), scrolled_history);

  GtkWidget *history_list = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_history), history_list);

  // --- Search bar row ---
  GtkWidget *hbox_search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_search);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search available packages...");
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox_search), entry);

  GtkWidget *search_button = gtk_button_new_with_label("Search");
  gtk_box_append(GTK_BOX(hbox_search), search_button);

  GtkWidget *desc_checkbox = gtk_check_button_new_with_label("Search in description");
  gtk_box_append(GTK_BOX(hbox_search), desc_checkbox);

  GtkWidget *exact_checkbox = gtk_check_button_new_with_label("Exact match");
  gtk_box_append(GTK_BOX(hbox_search), exact_checkbox);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox_search), spinner);

  // --- Buttons row ---
  GtkWidget *hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_buttons);

  GtkWidget *list_button = gtk_button_new_with_label("List Installed");
  gtk_box_append(GTK_BOX(hbox_buttons), list_button);

  GtkWidget *clear_button = gtk_button_new_with_label("Clear List");
  gtk_box_append(GTK_BOX(hbox_buttons), clear_button);

  GtkWidget *clear_cache_button = gtk_button_new_with_label("Clear Cache");
  gtk_box_append(GTK_BOX(hbox_buttons), clear_cache_button);
  g_signal_connect(
      clear_cache_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { clear_search_cache(); }), NULL);

  GtkWidget *status_label = gtk_label_new("Ready.");
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_main), status_label);

  // --- Inner paned (packages | details) ---
  GtkWidget *inner_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_main), inner_paned);
  gtk_widget_set_vexpand(inner_paned, TRUE);
  gtk_widget_set_hexpand(inner_paned, TRUE);
  int pos = load_paned_position();
  if (pos < 100) {
    pos = 300;
  }
  gtk_paned_set_position(GTK_PANED(inner_paned), pos);

  // --- Left: package list ---
  GtkWidget *scrolled_list = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_list, TRUE);
  gtk_widget_set_vexpand(scrolled_list, TRUE);
  gtk_paned_set_start_child(GTK_PANED(inner_paned), scrolled_list);

  GtkWidget *listbox = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_list), listbox);

  // --- Right: details + files split ---
  GtkWidget *right_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_hexpand(right_paned, TRUE);
  gtk_widget_set_vexpand(right_paned, TRUE);
  gtk_paned_set_end_child(GTK_PANED(inner_paned), right_paned);

  // --- Top: package details ---
  GtkWidget *scrolled_details = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_details, TRUE);
  gtk_widget_set_vexpand(scrolled_details, TRUE);
  gtk_paned_set_start_child(GTK_PANED(right_paned), scrolled_details);

  // container to keep label top-aligned
  GtkWidget *details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(details_box, GTK_ALIGN_START);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_details), details_box);

  GtkWidget *details_label = gtk_label_new("Select a package for details.");
  gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(details_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(details_label), PANGO_WRAP_WORD);
  gtk_label_set_selectable(GTK_LABEL(details_label), TRUE);
  gtk_box_append(GTK_BOX(details_box), details_label);

  // --- Bottom: file list ---
  GtkWidget *scrolled_files = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_files, TRUE);
  gtk_widget_set_vexpand(scrolled_files, TRUE);
  gtk_paned_set_end_child(GTK_PANED(right_paned), scrolled_files);

  // --- File info ---
  GtkWidget *files_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(files_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(files_label), TRUE);
  gtk_label_set_selectable(GTK_LABEL(files_label), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_files), files_label);
  gtk_widget_set_valign(files_label, GTK_ALIGN_START);

  // --- Bottom bar with item count ---
  GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_hexpand(bottom_bar, TRUE);
  gtk_widget_add_css_class(bottom_bar, "bottom-bar");
  gtk_box_append(GTK_BOX(vbox_root), bottom_bar);

  GtkWidget *count_label = gtk_label_new("Items: 0");
  gtk_label_set_xalign(GTK_LABEL(count_label), 0.0);
  gtk_box_append(GTK_BOX(bottom_bar), count_label);

  // --- Struct setup ---
  SearchWidgets *widgets = new SearchWidgets();
  widgets->entry = GTK_ENTRY(entry);
  widgets->listbox = GTK_LIST_BOX(listbox);
  widgets->list_scroller = GTK_SCROLLED_WINDOW(scrolled_list);
  widgets->history_list = GTK_LIST_BOX(history_list);
  widgets->spinner = GTK_SPINNER(spinner);
  widgets->search_button = GTK_BUTTON(search_button);
  widgets->status_label = GTK_LABEL(status_label);
  widgets->details_label = GTK_LABEL(details_label);
  widgets->files_label = GTK_LABEL(files_label);
  widgets->desc_checkbox = GTK_CHECK_BUTTON(desc_checkbox);
  widgets->exact_checkbox = GTK_CHECK_BUTTON(exact_checkbox);
  widgets->count_label = GTK_LABEL(count_label);

  // --- Modern GTK4 CSS for status bar background ---
  {
    GtkCssProvider *css = gtk_css_provider_new();
    // TODO: Use external CSS?
    gtk_css_provider_load_from_string(css,
                                      "label.status-bar { padding: 4px; border-radius: 4px; } "
                                      ".bottom-bar { padding: 5px; border-top: 1px solid #666; } "
                                      ".installed { "
                                      "  background-color: #b3f0b3; " /* softer green */
                                      "  color: black; "              /* readable text */
                                      "  padding: 2px 4px; "
                                      "  border-radius: 2px; "
                                      "}");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_widget_add_css_class(GTK_WIDGET(widgets->status_label), "status-bar");
    g_object_unref(css);
  }
  set_status(widgets->status_label, "Ready.", "gray");

  // --- Connect signals ---
  g_signal_connect(list_button, "clicked", G_CALLBACK(on_list_button_clicked), widgets);

  g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), widgets);

  g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), widgets);

  g_signal_connect(entry, "activate", G_CALLBACK(on_search_button_clicked), widgets);
  g_signal_connect(history_list, "row-selected", G_CALLBACK(on_history_row_selected), widgets);

  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) { delete static_cast<SearchWidgets *>(data); }),
                   widgets);

  g_signal_connect(window,
                   "close-request",
                   G_CALLBACK(+[](GtkWindow *w, gpointer user_data) -> gboolean {
                     save_window_geometry(w);
                     save_paned_position(GTK_PANED(user_data));
                     return FALSE;
                   }),
                   inner_paned);

  gtk_window_present(GTK_WINDOW(window));
}
