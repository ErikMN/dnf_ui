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

#include <unistd.h>
#include <cstdio>

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Internal UI handles used only during application setup
// Keeps widget construction readable without extending SearchWidgets
// -----------------------------------------------------------------------------
struct AppWidgets {
  GtkWidget *window = NULL;

  GtkWidget *vbox_root = NULL;
  GtkWidget *vbox_main = NULL;
  GtkWidget *vbox_history = NULL;

  GtkWidget *history_list = NULL;

  GtkWidget *entry = NULL;
  GtkWidget *search_button = NULL;
  GtkWidget *desc_checkbox = NULL;
  GtkWidget *exact_checkbox = NULL;
  GtkWidget *spinner = NULL;

  GtkWidget *list_button = NULL;
  GtkWidget *clear_button = NULL;
  GtkWidget *clear_cache_button = NULL;
  GtkWidget *toggle_history_button = NULL;
  GtkWidget *toggle_info_button = NULL;
  GtkWidget *refresh_button = NULL;

  GtkWidget *install_button = NULL;
  GtkWidget *reinstall_button = NULL;
  GtkWidget *remove_button = NULL;
  GtkWidget *apply_button = NULL;
  GtkWidget *clear_pending_button = NULL;

  GtkWidget *status_label = NULL;
  GtkWidget *inner_paned = NULL;

  GtkWidget *scrolled_list = NULL;
  GtkWidget *listbox = NULL;
  GtkWidget *notebook = NULL;

  GtkWidget *details_label = NULL;
  GtkWidget *files_label = NULL;
  GtkWidget *deps_label = NULL;
  GtkWidget *changelog_label = NULL;
  GtkWidget *pending_list = NULL;

  GtkWidget *count_label = NULL;
};

// -----------------------------------------------------------------------------
// Function forward declarations
// -----------------------------------------------------------------------------
static void activate(GtkApplication *app, gpointer user_data);
static GtkWidget *create_window(GtkApplication *app);
static GtkWidget *create_thin_separator(void);
static GtkWidget *create_scrolled_text_label(const char *text);
static void setup_shortcuts(GtkWidget *window, GtkWidget *entry);
static void build_main_ui(AppWidgets *ui);
static SearchWidgets *create_search_widgets(const AppWidgets *ui);
static void setup_css(SearchWidgets *widgets);
static void initialize_ui_state(SearchWidgets *widgets);
static void connect_signals(const AppWidgets *ui, SearchWidgets *widgets);
static void connect_cleanup(GtkWidget *window, SearchWidgets *widgets);
static void setup_periodic_tasks(void);
static void apply_root_state(const AppWidgets *ui, SearchWidgets *widgets);
static void show_pending_quit_dialog(SearchWidgets *widgets);
static gboolean on_main_window_close_request(GtkWindow *window, gpointer user_data);

// -----------------------------------------------------------------------------
// Run GTK application and return process exit status
// -----------------------------------------------------------------------------
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
// Create main application window
// -----------------------------------------------------------------------------
static GtkWidget *
create_window(GtkApplication *app)
{
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "DNF UI");
  load_window_geometry(GTK_WINDOW(window));

  return window;
}

// -----------------------------------------------------------------------------
// Create a reusable flat line separator
// -----------------------------------------------------------------------------
static GtkWidget *
create_thin_separator(void)
{
  GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(line, -1, 1);
  gtk_widget_add_css_class(line, "thin-line");

  return line;
}

// -----------------------------------------------------------------------------
// Create selectable top-aligned text label inside a scrolled window
// -----------------------------------------------------------------------------
static GtkWidget *
create_scrolled_text_label(const char *text)
{
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);

  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_widget_set_focusable(label, FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), label);
  gtk_widget_set_valign(label, GTK_ALIGN_START);
  gtk_widget_set_margin_start(label, 10);
  gtk_widget_set_margin_end(label, 10);
  gtk_widget_set_margin_top(label, 10);
  gtk_widget_set_margin_bottom(label, 10);

  return scrolled;
}

// -----------------------------------------------------------------------------
// Setup window keyboard shortcuts
// -----------------------------------------------------------------------------
static void
setup_shortcuts(GtkWidget *window, GtkWidget *entry)
{
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

  // Ctrl+F: focus search bar
  GtkShortcut *focus_search = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK),
                                               gtk_callback_action_new(
                                                   +[](GtkWidget *, GVariant *, gpointer user_data) -> gboolean {
                                                     GtkEntry *entry = static_cast<GtkEntry *>(user_data);
                                                     gtk_widget_grab_focus(GTK_WIDGET(entry));
                                                     return TRUE;
                                                   },
                                                   entry,
                                                   NULL));

  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), focus_search);
}

// -----------------------------------------------------------------------------
// Build all GTK widgets and main application layout
// -----------------------------------------------------------------------------
static void
build_main_ui(AppWidgets *ui)
{
  GtkWidget *window = ui->window;

  GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox_root);
  ui->vbox_root = vbox_root;

  GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_root), outer_paned);
  gtk_paned_set_position(GTK_PANED(outer_paned), 200);

  GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_end_child(GTK_PANED(outer_paned), vbox_main);
  ui->vbox_main = vbox_main;

  GtkWidget *vbox_history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_vexpand(vbox_history, TRUE);
  gtk_widget_set_hexpand(vbox_history, TRUE);
  gtk_paned_set_start_child(GTK_PANED(outer_paned), vbox_history);
  ui->vbox_history = vbox_history;

  GtkWidget *history_label = gtk_label_new("Search History");
  gtk_label_set_xalign(GTK_LABEL(history_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_history), history_label);

  // --- Flat line separator below Search History label ---
  gtk_box_append(GTK_BOX(vbox_history), create_thin_separator());

  GtkWidget *scrolled_history = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled_history, TRUE);
  gtk_widget_set_hexpand(scrolled_history, TRUE);
  gtk_box_append(GTK_BOX(vbox_history), scrolled_history);

  GtkWidget *history_list = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_history), history_list);
  ui->history_list = history_list;

  // --- Search bar row ---
  GtkWidget *hbox_search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_search);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search available packages...");
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox_search), entry);
  ui->entry = entry;

  GtkWidget *search_button = gtk_button_new_with_label("Search");
  gtk_box_append(GTK_BOX(hbox_search), search_button);
  ui->search_button = search_button;

  GtkWidget *desc_checkbox = gtk_check_button_new_with_label("Search in description");
  gtk_box_append(GTK_BOX(hbox_search), desc_checkbox);
  ui->desc_checkbox = desc_checkbox;

  GtkWidget *exact_checkbox = gtk_check_button_new_with_label("Exact match");
  gtk_box_append(GTK_BOX(hbox_search), exact_checkbox);
  ui->exact_checkbox = exact_checkbox;

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox_search), spinner);
  ui->spinner = spinner;

  // --- Flat line separator below Search bar ---
  gtk_box_append(GTK_BOX(vbox_main), create_thin_separator());

  // --- Buttons row ---
  GtkWidget *hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_buttons);

  GtkWidget *list_button = gtk_button_new_with_label("List Installed");
  gtk_box_append(GTK_BOX(hbox_buttons), list_button);
  ui->list_button = list_button;

  GtkWidget *clear_button = gtk_button_new_with_label("Clear List");
  gtk_box_append(GTK_BOX(hbox_buttons), clear_button);
  ui->clear_button = clear_button;

  GtkWidget *clear_cache_button = gtk_button_new_with_label("Clear Cache");
  gtk_box_append(GTK_BOX(hbox_buttons), clear_cache_button);
  ui->clear_cache_button = clear_cache_button;

  GtkWidget *toggle_history_button = gtk_button_new_with_label("Hide History");
  gtk_box_append(GTK_BOX(hbox_buttons), toggle_history_button);
  ui->toggle_history_button = toggle_history_button;

  GtkWidget *toggle_info_button = gtk_button_new_with_label("Hide Info");
  gtk_box_append(GTK_BOX(hbox_buttons), toggle_info_button);
  ui->toggle_info_button = toggle_info_button;

  // --- Refresh Repositories button ---
  // Triggers an asynchronous repository rebuild using BaseManager::rebuild()
  // Runs in a background thread to keep the GTK UI responsive
  GtkWidget *refresh_button = gtk_button_new_with_label("Refresh Repositories");
  gtk_box_append(GTK_BOX(hbox_buttons), refresh_button);
  ui->refresh_button = refresh_button;

  // --- Transaction buttons row (Install / Reinstall / Remove / Apply) ---
  GtkWidget *hbox_tx_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_tx_buttons);

  GtkWidget *install_button = gtk_button_new_with_label("Mark for Install");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), install_button);
  ui->install_button = install_button;

  GtkWidget *reinstall_button = gtk_button_new_with_label("Mark for Reinstall");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), reinstall_button);
  ui->reinstall_button = reinstall_button;

  GtkWidget *remove_button = gtk_button_new_with_label("Mark for Removal");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), remove_button);
  ui->remove_button = remove_button;

  GtkWidget *apply_button = gtk_button_new_with_label("Apply Transactions");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), apply_button);
  ui->apply_button = apply_button;

  GtkWidget *clear_pending_button = gtk_button_new_with_label("Clear Transactions");
  gtk_box_append(GTK_BOX(hbox_tx_buttons), clear_pending_button);
  ui->clear_pending_button = clear_pending_button;

  // --- Flat line separator ---
  gtk_box_append(GTK_BOX(vbox_main), create_thin_separator());

  GtkWidget *status_label = gtk_label_new("Ready.");
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_main), status_label);
  ui->status_label = status_label;

  gtk_box_append(GTK_BOX(vbox_main), create_thin_separator());

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
  ui->inner_paned = inner_paned;

  // --- Left: package list ---
  GtkWidget *scrolled_list = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_list, TRUE);
  gtk_widget_set_vexpand(scrolled_list, TRUE);
  gtk_paned_set_start_child(GTK_PANED(inner_paned), scrolled_list);
  ui->scrolled_list = scrolled_list;

  GtkWidget *listbox = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_list), listbox);
  ui->listbox = listbox;

  // --- Right: notebook with tabs ---
  GtkWidget *notebook = gtk_notebook_new();
  gtk_widget_set_hexpand(notebook, TRUE);
  gtk_widget_set_vexpand(notebook, TRUE);
  gtk_paned_set_end_child(GTK_PANED(inner_paned), notebook);
  ui->notebook = notebook;

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
  // Keep package text copyable without stealing focus when switching tabs.
  gtk_widget_set_focusable(details_label, FALSE);
  gtk_box_append(GTK_BOX(details_box), details_label);
  ui->details_label = details_label;

  GtkWidget *tab_label_info = gtk_label_new("Info");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_details, tab_label_info);

  // --- Tab 2: File List ---
  GtkWidget *scrolled_files = create_scrolled_text_label("Select an installed package to view its file list.");
  GtkWidget *files_label = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled_files));
  ui->files_label = files_label;

  GtkWidget *tab_label_files = gtk_label_new("Files");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_files, tab_label_files);

  // --- Tab 3: Dependencies ---
  GtkWidget *scrolled_deps = create_scrolled_text_label("Select a package to view dependencies.");
  GtkWidget *deps_label = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled_deps));
  ui->deps_label = deps_label;

  GtkWidget *tab_label_deps = gtk_label_new("Dependencies");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_deps, tab_label_deps);

  // --- Tab 4: Changelog ---
  GtkWidget *scrolled_changelog = create_scrolled_text_label("Select a package to view its changelog.");
  GtkWidget *changelog_label = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled_changelog));
  ui->changelog_label = changelog_label;

  GtkWidget *tab_label_changelog = gtk_label_new("Changelog");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_changelog, tab_label_changelog);

  // --- Tab 5: Pending actions ---
  GtkWidget *pending_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(pending_scrolled, TRUE);
  gtk_widget_set_vexpand(pending_scrolled, TRUE);

  GtkWidget *pending_list = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pending_scrolled), pending_list);
  ui->pending_list = pending_list;

  GtkWidget *tab_label_pending = gtk_label_new("Pending");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pending_scrolled, tab_label_pending);

  // --- Bottom bar with item count ---
  GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_hexpand(bottom_bar, TRUE);
  gtk_widget_add_css_class(bottom_bar, "bottom-bar");
  gtk_box_append(GTK_BOX(vbox_root), bottom_bar);

  GtkWidget *count_label = gtk_label_new("Items: 0");
  gtk_label_set_xalign(GTK_LABEL(count_label), 0.0);
  gtk_box_append(GTK_BOX(bottom_bar), count_label);
  ui->count_label = count_label;
}

// -----------------------------------------------------------------------------
// Create shared SearchWidgets state from UI handles
// Destroyed when the main window is destroyed
// -----------------------------------------------------------------------------
static SearchWidgets *
create_search_widgets(const AppWidgets *ui)
{
  SearchWidgets *widgets = new SearchWidgets();
  widgets->entry = GTK_ENTRY(ui->entry);
  widgets->listbox = GTK_LIST_BOX(ui->listbox);
  widgets->list_scroller = GTK_SCROLLED_WINDOW(ui->scrolled_list);
  widgets->inner_paned = GTK_PANED(ui->inner_paned);
  widgets->history_list = GTK_LIST_BOX(ui->history_list);
  widgets->spinner = GTK_SPINNER(ui->spinner);
  widgets->search_button = GTK_BUTTON(ui->search_button);
  widgets->install_button = GTK_BUTTON(ui->install_button);
  widgets->remove_button = GTK_BUTTON(ui->remove_button);
  widgets->reinstall_button = GTK_BUTTON(ui->reinstall_button);
  widgets->apply_button = GTK_BUTTON(ui->apply_button);
  widgets->clear_pending_button = GTK_BUTTON(ui->clear_pending_button);
  widgets->status_label = GTK_LABEL(ui->status_label);
  widgets->details_label = GTK_LABEL(ui->details_label);
  widgets->files_label = GTK_LABEL(ui->files_label);
  widgets->deps_label = GTK_LABEL(ui->deps_label);
  widgets->desc_checkbox = GTK_CHECK_BUTTON(ui->desc_checkbox);
  widgets->exact_checkbox = GTK_CHECK_BUTTON(ui->exact_checkbox);
  widgets->count_label = GTK_LABEL(ui->count_label);
  widgets->changelog_label = GTK_LABEL(ui->changelog_label);
  widgets->pending_list = GTK_LIST_BOX(ui->pending_list);
  widgets->search_cancellable = nullptr;
  widgets->next_search_request_id = 1;
  widgets->current_search_request_id = 0;
  widgets->allow_close_with_pending = false;
  widgets->pending_quit_dialog_open = false;

  return widgets;
}

// -----------------------------------------------------------------------------
// Setup GTK CSS styling
// -----------------------------------------------------------------------------
static void
setup_css(SearchWidgets *widgets)
{
  GtkCssProvider *css = gtk_css_provider_new();
  // TODO: Use external CSS?
  gtk_css_provider_load_from_string(css,
                                    "label.status-bar { padding: 4px; border-radius: 4px; } "
                                    ".bottom-bar { padding: 5px; border-top: 1px solid #666; } "
                                    ".package-status { "
                                    "  padding: 3px 10px; "
                                    "  border-radius: 999px; "
                                    "  border: 1px solid transparent; "
                                    "  font-weight: 800; "
                                    "} "
                                    ".package-status-available { "
                                    "  background-color: #cbd8e8; "
                                    "  border-color: #6f89a8; "
                                    "  color: #10263f; "
                                    "} "
                                    ".package-status-installed { "
                                    "  background-color: #cfe6c2; "
                                    "  border-color: #668f58; "
                                    "  color: #173915; "
                                    "} "
                                    ".package-status-pending-install { "
                                    "  background-color: #2b64b5; "
                                    "  border-color: #163d74; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-status-pending-reinstall { "
                                    "  background-color: #d89a19; "
                                    "  border-color: #8c5f00; "
                                    "  color: #2d1a00; "
                                    "} "
                                    ".package-status-pending-remove { "
                                    "  background-color: #bf4a33; "
                                    "  border-color: #7c281b; "
                                    "  color: #ffffff; "
                                    "} "
                                    ".package-meta { "
                                    "  color: #555555; "
                                    "} "
                                    ".package-summary { "
                                    "  color: #333333; "
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

// -----------------------------------------------------------------------------
// Initialize widget state after construction
// -----------------------------------------------------------------------------
static void
initialize_ui_state(SearchWidgets *widgets)
{
  // Apply and Clear Transactions are meaningful only when there are pending actions
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->apply_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->clear_pending_button), FALSE);

  set_status(widgets->status_label, "Ready.", "gray");
  fill_package_view(widgets, {});
}

// -----------------------------------------------------------------------------
// Connect all GTK signals and callbacks
// -----------------------------------------------------------------------------
static void
connect_signals(const AppWidgets *ui, SearchWidgets *widgets)
{
  g_signal_connect(
      ui->clear_cache_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { clear_search_cache(); }), NULL);

  g_signal_connect(ui->list_button, "clicked", G_CALLBACK(on_list_button_clicked), widgets);

  g_signal_connect(ui->install_button, "clicked", G_CALLBACK(on_install_button_clicked), widgets);

  g_signal_connect(ui->reinstall_button, "clicked", G_CALLBACK(on_reinstall_button_clicked), widgets);

  g_signal_connect(ui->remove_button, "clicked", G_CALLBACK(on_remove_button_clicked), widgets);

  g_signal_connect(ui->clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), widgets);

  g_signal_connect(ui->search_button, "clicked", G_CALLBACK(on_search_button_clicked), widgets);

  g_signal_connect(ui->entry, "activate", G_CALLBACK(on_search_button_clicked), widgets);
  g_signal_connect(ui->history_list, "row-selected", G_CALLBACK(on_history_row_selected), widgets);

  g_signal_connect(ui->toggle_history_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     GtkWidget *pane = GTK_WIDGET(user_data);
                     gboolean visible = gtk_widget_get_visible(pane);
                     gtk_widget_set_visible(pane, !visible);
                     gtk_button_set_label(button, visible ? "Show History" : "Hide History");
                   }),
                   ui->vbox_history);

  g_signal_connect(ui->toggle_info_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     GtkWidget *pane = GTK_WIDGET(user_data);
                     gboolean visible = gtk_widget_get_visible(pane);
                     gtk_widget_set_visible(pane, !visible);
                     gtk_button_set_label(button, visible ? "Show Info" : "Hide Info");
                   }),
                   ui->notebook);

  g_signal_connect(ui->apply_button, "clicked", G_CALLBACK(on_apply_button_clicked), widgets);
  g_signal_connect(ui->clear_pending_button, "clicked", G_CALLBACK(on_clear_pending_button_clicked), widgets);

  g_signal_connect(ui->refresh_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     set_status(widgets->status_label, "Refreshing repositories...", "blue");
                     gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

                     GCancellable *c = g_cancellable_new();
                     g_signal_connect_object(
                         GTK_WIDGET(widgets->entry), "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);

                     GTask *task = g_task_new(NULL, c, on_rebuild_task_finished, widgets);
                     g_task_run_in_thread(task, on_rebuild_task);
                     g_object_unref(task);
                     g_object_unref(c);
                   }),
                   widgets);

  // Intercept window close so unapplied marked changes can be confirmed first.
  g_signal_connect(ui->window, "close-request", G_CALLBACK(on_main_window_close_request), widgets);

  // Save window geometry and paned position when window is unrealized (destroyed)
  g_signal_connect(ui->window,
                   "unrealize",
                   G_CALLBACK(+[](GtkWidget *widget, gpointer user_data) {
                     GtkWindow *w = GTK_WINDOW(widget);
                     save_window_geometry(w);
                     save_paned_position(GTK_PANED(user_data));
                   }),
                   ui->inner_paned);

  // Live-update: save pane position whenever the user moves the divider
  g_signal_connect(ui->inner_paned,
                   "notify::position",
                   G_CALLBACK(+[](GtkPaned *paned, GParamSpec *, gpointer) { save_paned_position(paned); }),
                   NULL);
}

// -----------------------------------------------------------------------------
// Show a confirmation dialog before closing the app with unapplied changes.
// -----------------------------------------------------------------------------
static void
show_pending_quit_dialog(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  widgets->pending_quit_dialog_open = true;

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, "Quit and discard marked changes?");
  gtk_window_set_default_size(dialog, 520, 180);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(dialog, app);
    }
    gtk_window_set_transient_for(dialog, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *title = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(title), "<b>Quit and discard marked changes?</b>");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *message = gtk_label_new(
      "There are still marked changes that have not yet been applied. They will get lost if you choose to quit.");
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_box_append(GTK_BOX(outer), message);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
  gtk_box_append(GTK_BOX(button_box), cancel_button);

  GtkWidget *quit_button = gtk_button_new_with_label("Quit");
  gtk_widget_add_css_class(quit_button, "destructive-action");
  gtk_box_append(GTK_BOX(button_box), quit_button);

  g_signal_connect(cancel_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  g_signal_connect(quit_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     widgets->allow_close_with_pending = true;

                     GtkRoot *dialog_root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (dialog_root && GTK_IS_WINDOW(dialog_root)) {
                       gtk_window_destroy(GTK_WINDOW(dialog_root));
                     }

                     GtkRoot *parent_root = gtk_widget_get_root(GTK_WIDGET(widgets->entry));
                     if (parent_root && GTK_IS_WINDOW(parent_root)) {
                       gtk_window_close(GTK_WINDOW(parent_root));
                     }
                   }),
                   widgets);

  g_signal_connect(dialog,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     widgets->pending_quit_dialog_open = false;
                   }),
                   widgets);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Confirm before closing the app when there are unapplied pending changes.
// -----------------------------------------------------------------------------
static gboolean
on_main_window_close_request(GtkWindow *window, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // TODO: FIXME: This is broken
  // Save paned position on close
  save_window_geometry(window);
  if (widgets && widgets->inner_paned) {
    save_paned_position(widgets->inner_paned);
  }

  if (!widgets || widgets->allow_close_with_pending) {
    return FALSE;
  }

  if (widgets->pending.empty()) {
    return FALSE;
  }

  if (!widgets->pending_quit_dialog_open) {
    show_pending_quit_dialog(widgets);
  }

  return TRUE;
}

// -----------------------------------------------------------------------------
// Connect window destroy callback for SearchWidgets cleanup
// -----------------------------------------------------------------------------
static void
connect_cleanup(GtkWidget *window, SearchWidgets *widgets)
{
  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(data);
                     if (widgets->search_cancellable) {
                       g_object_unref(widgets->search_cancellable);
                     }
                     delete widgets;
                   }),
                   widgets);
}

// -----------------------------------------------------------------------------
// Setup periodic background tasks
// -----------------------------------------------------------------------------
static void
setup_periodic_tasks(void)
{
  // --- Periodic refresh of installed package names every 5 minutes ---
  g_timeout_add_seconds(
      300, // 5 minutes
      [](gpointer) -> gboolean {
        refresh_installed_nevras();
        return TRUE; // keep repeating
      },
      nullptr);
}

// -----------------------------------------------------------------------------
// Disable transaction buttons when not running as root
// FIXME: Replace with Polkit:
// -----------------------------------------------------------------------------
static void
apply_root_state(const AppWidgets *ui, SearchWidgets *widgets)
{
  if (geteuid() != 0) {
    std::printf("*** Not running as root ***\n");
    gtk_widget_set_sensitive(ui->install_button, FALSE);
    gtk_widget_set_sensitive(ui->reinstall_button, FALSE);
    gtk_widget_set_sensitive(ui->remove_button, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->apply_button), FALSE);
  }
}

// -----------------------------------------------------------------------------
// GTK app setup (start here)
// -----------------------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer)
{
  // Create top-level application window
  AppWidgets ui = {};
  ui.window = create_window(app);

  // Build all visible GTK widgets and layout containers
  build_main_ui(&ui);

  // Create shared widget state used by callbacks and backend actions
  SearchWidgets *widgets = create_search_widgets(&ui);

  // Setup behavior, styling, lifecycle, and background tasks
  setup_shortcuts(ui.window, ui.entry);
  setup_css(widgets);
  initialize_ui_state(widgets);
  connect_signals(&ui, widgets);
  connect_cleanup(ui.window, widgets);
  setup_periodic_tasks();
  apply_root_state(&ui, widgets);

  // Show the fully initialized window
  gtk_window_present(GTK_WINDOW(ui.window));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
