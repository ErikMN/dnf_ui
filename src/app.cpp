// -----------------------------------------------------------------------------
// src/app.cpp
// GTK Application setup and activation
// -----------------------------------------------------------------------------
#include "app.hpp"
#include "widgets.hpp"
#include "config.hpp"
#include "dnf_backend.hpp"
#include "ui_helpers.hpp"
#include "base_manager.hpp"

#include <gtk/gtk.h>

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
  gtk_window_set_title(GTK_WINDOW(window), "DNF UI");
  load_window_geometry(GTK_WINDOW(window));

  // Keyboard shortcuts: Ctrl+Q and Ctrl+W to close window
  GtkEventController *shortcuts = GTK_EVENT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_widget_add_controller(window, shortcuts);

  auto shortcut_callback = +[](GtkWidget *widget, GVariant *, gpointer) -> gboolean {
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

  // --- Flat line separator below Search History label ---
  GtkWidget *line_history = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(line_history, -1, 1);
  gtk_widget_add_css_class(line_history, "thin-line");
  gtk_box_append(GTK_BOX(vbox_history), line_history);

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

  // Ctrl+F: focus search bar
  GtkShortcut *focus_search = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK),
                                               gtk_callback_action_new(
                                                   +[](GtkWidget *widget, GVariant *, gpointer user_data) -> gboolean {
                                                     GtkEntry *entry = static_cast<GtkEntry *>(user_data);
                                                     gtk_widget_grab_focus(GTK_WIDGET(entry));
                                                     return TRUE;
                                                   },
                                                   entry,
                                                   NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), focus_search);

  GtkWidget *search_button = gtk_button_new_with_label("Search");
  gtk_box_append(GTK_BOX(hbox_search), search_button);

  GtkWidget *desc_checkbox = gtk_check_button_new_with_label("Search in description");
  gtk_box_append(GTK_BOX(hbox_search), desc_checkbox);

  GtkWidget *exact_checkbox = gtk_check_button_new_with_label("Exact match");
  gtk_box_append(GTK_BOX(hbox_search), exact_checkbox);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox_search), spinner);

  // --- Flat line separator below Search bar ---
  GtkWidget *line_search_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(line_search_buttons, -1, 1);
  gtk_widget_add_css_class(line_search_buttons, "thin-line");
  gtk_box_append(GTK_BOX(vbox_main), line_search_buttons);

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

  // --- Flat line separator ---
  GtkWidget *line_buttons_status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(line_buttons_status, -1, 1);
  gtk_widget_add_css_class(line_buttons_status, "thin-line");
  gtk_box_append(GTK_BOX(vbox_main), line_buttons_status);

  GtkWidget *status_label = gtk_label_new("Ready.");
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_main), status_label);

  GtkWidget *line_status_paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(line_status_paned, -1, 1);
  gtk_widget_add_css_class(line_status_paned, "thin-line");
  gtk_box_append(GTK_BOX(vbox_main), line_status_paned);

  // --- Inner paned (packages | details/files tabs) ---
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

  // --- Right: notebook with tabs ---
  GtkWidget *notebook = gtk_notebook_new();
  gtk_widget_set_hexpand(notebook, TRUE);
  gtk_widget_set_vexpand(notebook, TRUE);
  gtk_paned_set_end_child(GTK_PANED(inner_paned), notebook);

  // --- Tab 1: Package Info ---
  GtkWidget *scrolled_details = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_details, TRUE);
  gtk_widget_set_vexpand(scrolled_details, TRUE);

  // container to keep label top-aligned
  GtkWidget *details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(details_box, GTK_ALIGN_START);
  gtk_widget_set_margin_start(details_box, 10);
  gtk_widget_set_margin_end(details_box, 10);
  gtk_widget_set_margin_top(details_box, 10);
  gtk_widget_set_margin_bottom(details_box, 10);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_details), details_box);

  GtkWidget *details_label = gtk_label_new("Select a package for details.");
  gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(details_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(details_label), PANGO_WRAP_WORD);
  gtk_label_set_selectable(GTK_LABEL(details_label), TRUE);
  gtk_widget_set_focusable(details_label, TRUE);
  gtk_box_append(GTK_BOX(details_box), details_label);

  GtkWidget *tab_label_info = gtk_label_new("Info");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_details, tab_label_info);

  // --- Tab 2: File List ---
  GtkWidget *scrolled_files = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_files, TRUE);
  gtk_widget_set_vexpand(scrolled_files, TRUE);

  GtkWidget *files_label = gtk_label_new("Select an installed package to view its file list.");
  gtk_label_set_xalign(GTK_LABEL(files_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(files_label), TRUE);
  gtk_label_set_selectable(GTK_LABEL(files_label), TRUE);
  gtk_widget_set_focusable(files_label, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_files), files_label);
  gtk_widget_set_valign(files_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start(files_label, 10);
  gtk_widget_set_margin_end(files_label, 10);
  gtk_widget_set_margin_top(files_label, 10);
  gtk_widget_set_margin_bottom(files_label, 10);

  GtkWidget *tab_label_files = gtk_label_new("Files");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_files, tab_label_files);

  // --- Tab 3: Dependencies ---
  GtkWidget *scrolled_deps = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_deps, TRUE);
  gtk_widget_set_vexpand(scrolled_deps, TRUE);

  GtkWidget *deps_label = gtk_label_new("Select a package to view dependencies.");
  gtk_label_set_xalign(GTK_LABEL(deps_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(deps_label), TRUE);
  gtk_label_set_selectable(GTK_LABEL(deps_label), TRUE);
  gtk_widget_set_focusable(deps_label, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_deps), deps_label);
  gtk_widget_set_valign(deps_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start(deps_label, 10);
  gtk_widget_set_margin_end(deps_label, 10);
  gtk_widget_set_margin_top(deps_label, 10);
  gtk_widget_set_margin_bottom(deps_label, 10);

  GtkWidget *tab_label_deps = gtk_label_new("Dependencies");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_deps, tab_label_deps);

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
  widgets->deps_label = GTK_LABEL(deps_label);
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
                                      "} "
                                      ".thin-line { "
                                      "  background-color: @borders; "
                                      "  margin: 0; "
                                      "  padding: 0; "
                                      "  min-height: 1px; "
                                      "} ");
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

  // --- Refresh Repositories button ---
  // Triggers an asynchronous repository rebuild using BaseManager::rebuild()
  // Runs in a background thread to keep the GTK UI responsive
  GtkWidget *refresh_button = gtk_button_new_with_label("Refresh Repositories");
  gtk_box_append(GTK_BOX(hbox_buttons), refresh_button);
  g_signal_connect(refresh_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     set_status(widgets->status_label, "Refreshing repositories...", "blue");
                     gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);
                     GTask *task = g_task_new(NULL, NULL, on_rebuild_task_finished, widgets);
                     g_task_run_in_thread(task, on_rebuild_task);
                     g_object_unref(task);
                   }),
                   widgets);

  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) { delete static_cast<SearchWidgets *>(data); }),
                   widgets);

  // TODO: FIXME: This is broken
  // Save paned position on close
  // g_signal_connect(window,
  //                  "close-request",
  //                  G_CALLBACK(+[](GtkWindow *w, gpointer user_data) -> gboolean {
  //                    save_window_geometry(w);
  //                    save_paned_position(GTK_PANED(user_data));
  //                    return FALSE;
  //                  }),
  //                  inner_paned);

  // --- Periodic refresh of installed package names every 5 minutes ---
  g_timeout_add_seconds(
      300, // 5 minutes
      [](gpointer) -> gboolean {
        refresh_installed_nevras();
        return TRUE; // keep repeating
      },
      nullptr);

  gtk_window_present(GTK_WINDOW(window));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
