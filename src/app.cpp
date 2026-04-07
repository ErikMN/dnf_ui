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
  GtkWidget *list_available_button = NULL;
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

  GtkTextBuffer *details_buffer = NULL;
  GtkTextBuffer *files_buffer = NULL;
  GtkTextBuffer *deps_buffer = NULL;
  GtkTextBuffer *changelog_buffer = NULL;
  GtkWidget *pending_list = NULL;

  GtkWidget *count_label = NULL;
  GtkWidget *warmup_label = NULL;
};

// -----------------------------------------------------------------------------
// Function forward declarations
// -----------------------------------------------------------------------------
static void activate(GtkApplication *app, gpointer user_data);
static GtkWidget *create_window(GtkApplication *app);
static GtkWidget *create_thin_separator(void);
static GtkWidget *create_scrolled_text_view(const char *text, GtkWrapMode wrap_mode, GtkTextBuffer **out_buffer);
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
static gboolean start_backend_warmup_idle(gpointer user_data);
static void start_backend_warmup_task(SearchWidgets *widgets);
static void on_backend_warmup_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_backend_warmup_task_finished(GObject *source_object, GAsyncResult *result, gpointer user_data);

// -----------------------------------------------------------------------------
// Run GTK application and return process exit status
// -----------------------------------------------------------------------------
int
app_run_dnf_ui(int argc, char **argv)
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
  config_load_window_geometry(GTK_WINDOW(window));

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
// Create selectable read-only text view inside a scrolled window
// -----------------------------------------------------------------------------
static GtkWidget *
create_scrolled_text_view(const char *text, GtkWrapMode wrap_mode, GtkTextBuffer **out_buffer)
{
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap_mode);
  gtk_widget_set_focusable(view, TRUE);
  gtk_widget_set_margin_start(view, 10);
  gtk_widget_set_margin_end(view, 10);
  gtk_widget_set_margin_top(view, 10);
  gtk_widget_set_margin_bottom(view, 10);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buffer, text, -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);

  if (out_buffer) {
    *out_buffer = buffer;
  }

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

  GtkWidget *list_available_button = gtk_button_new_with_label("List Available");
  gtk_box_append(GTK_BOX(hbox_buttons), list_available_button);
  ui->list_available_button = list_available_button;

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
  gtk_label_set_selectable(GTK_LABEL(status_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_box_append(GTK_BOX(vbox_main), status_label);
  ui->status_label = status_label;

  gtk_box_append(GTK_BOX(vbox_main), create_thin_separator());

  // --- Inner paned (packages | details/files tabs) ---
  GtkWidget *inner_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_main), inner_paned);
  gtk_widget_set_vexpand(inner_paned, TRUE);
  gtk_widget_set_hexpand(inner_paned, TRUE);
  int pos = config_load_paned_position();
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
  GtkTextBuffer *details_buffer = NULL;
  GtkWidget *scrolled_details =
      create_scrolled_text_view("Select a package for details.", GTK_WRAP_WORD, &details_buffer);
  ui->details_buffer = details_buffer;

  GtkWidget *tab_label_info = gtk_label_new("Info");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_details, tab_label_info);

  // --- Tab 2: File List ---
  GtkTextBuffer *files_buffer = NULL;
  GtkWidget *scrolled_files =
      create_scrolled_text_view("Select an installed package to view its file list.", GTK_WRAP_NONE, &files_buffer);
  ui->files_buffer = files_buffer;

  GtkWidget *tab_label_files = gtk_label_new("Files");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_files, tab_label_files);

  // --- Tab 3: Dependencies ---
  GtkTextBuffer *deps_buffer = NULL;
  GtkWidget *scrolled_deps =
      create_scrolled_text_view("Select a package to view dependencies.", GTK_WRAP_WORD, &deps_buffer);
  ui->deps_buffer = deps_buffer;

  GtkWidget *tab_label_deps = gtk_label_new("Dependencies");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_deps, tab_label_deps);

  // --- Tab 4: Changelog ---
  GtkTextBuffer *changelog_buffer = NULL;
  GtkWidget *scrolled_changelog =
      create_scrolled_text_view("Select a package to view its changelog.", GTK_WRAP_WORD, &changelog_buffer);
  ui->changelog_buffer = changelog_buffer;

  GtkWidget *tab_label_changelog = gtk_label_new("Changelog");
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_changelog, tab_label_changelog);

  // --- Tab 5: Pending actions ---
  GtkWidget *pending_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(pending_scrolled, TRUE);
  gtk_widget_set_vexpand(pending_scrolled, TRUE);

  GtkWidget *pending_list = gtk_list_box_new();
  // Pending rows act as direct action buttons and should not keep listbox selection state.
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(pending_list), GTK_SELECTION_NONE);
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
  gtk_widget_set_hexpand(count_label, TRUE);
  gtk_box_append(GTK_BOX(bottom_bar), count_label);
  ui->count_label = count_label;

  // Passive startup note shown only while the backend is warming up.
  GtkWidget *warmup_label = gtk_label_new("Loading package data...");
  gtk_label_set_xalign(GTK_LABEL(warmup_label), 1.0);
  gtk_widget_set_visible(warmup_label, FALSE);
  gtk_box_append(GTK_BOX(bottom_bar), warmup_label);
  ui->warmup_label = warmup_label;
}

// -----------------------------------------------------------------------------
// Create shared SearchWidgets state from UI handles
// Destroyed when the main window is destroyed
// -----------------------------------------------------------------------------
static SearchWidgets *
create_search_widgets(const AppWidgets *ui)
{
  SearchWidgets *widgets = new SearchWidgets();
  widgets->query.entry = GTK_ENTRY(ui->entry);
  widgets->query.history_list = GTK_LIST_BOX(ui->history_list);
  widgets->query.spinner = GTK_SPINNER(ui->spinner);
  widgets->query.search_button = GTK_BUTTON(ui->search_button);
  widgets->query.list_button = GTK_BUTTON(ui->list_button);
  widgets->query.list_available_button = GTK_BUTTON(ui->list_available_button);
  widgets->query.status_label = GTK_LABEL(ui->status_label);
  widgets->query.desc_checkbox = GTK_CHECK_BUTTON(ui->desc_checkbox);
  widgets->query.exact_checkbox = GTK_CHECK_BUTTON(ui->exact_checkbox);

  widgets->results.listbox = GTK_LIST_BOX(ui->listbox);
  widgets->results.list_scroller = GTK_SCROLLED_WINDOW(ui->scrolled_list);
  widgets->results.inner_paned = GTK_PANED(ui->inner_paned);
  widgets->results.details_buffer = ui->details_buffer;
  widgets->results.files_buffer = ui->files_buffer;
  widgets->results.deps_buffer = ui->deps_buffer;
  widgets->results.changelog_buffer = ui->changelog_buffer;
  widgets->results.count_label = GTK_LABEL(ui->count_label);

  widgets->transaction.install_button = GTK_BUTTON(ui->install_button);
  widgets->transaction.remove_button = GTK_BUTTON(ui->remove_button);
  widgets->transaction.reinstall_button = GTK_BUTTON(ui->reinstall_button);
  widgets->transaction.apply_button = GTK_BUTTON(ui->apply_button);
  widgets->transaction.clear_pending_button = GTK_BUTTON(ui->clear_pending_button);
  widgets->transaction.pending_list = GTK_LIST_BOX(ui->pending_list);

  widgets->query_state.package_list_cancellable = nullptr;
  widgets->query_state.next_package_list_request_id = 1;
  widgets->query_state.current_package_list_request_id = 0;
  widgets->query_state.current_package_list_request_kind = PackageListRequestKind::NONE;

  widgets->window_state.allow_close_with_pending = false;
  widgets->window_state.pending_quit_dialog_open = false;
  widgets->window_state.backend_warmup_label = GTK_LABEL(ui->warmup_label);
  widgets->window_state.backend_warmup_cancellable = nullptr;

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
                                    ".package-status-upgradeable { "
                                    "  background-color: #f0ddb0; "
                                    "  border-color: #9f7a24; "
                                    "  color: #4a3200; "
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
  gtk_widget_add_css_class(GTK_WIDGET(widgets->query.status_label), "status-bar");
  g_object_unref(css);
}

// -----------------------------------------------------------------------------
// Initialize widget state after construction
// -----------------------------------------------------------------------------
static void
initialize_ui_state(SearchWidgets *widgets)
{
  // Apply and Clear Transactions are meaningful only when there are pending actions
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), FALSE);

  ui_helpers_set_status(widgets->query.status_label, "Ready.", "gray");
  package_table_fill_package_view(widgets, {});
}

// -----------------------------------------------------------------------------
// Connect all GTK signals and callbacks
// -----------------------------------------------------------------------------
static void
connect_signals(const AppWidgets *ui, SearchWidgets *widgets)
{
  g_signal_connect(ui->clear_cache_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     package_query_clear_search_cache();
                     ui_helpers_set_status(widgets->query.status_label, "Search cache cleared.", "green");
                   }),
                   widgets);

  g_signal_connect(ui->list_button, "clicked", G_CALLBACK(package_query_on_list_button_clicked), widgets);

  g_signal_connect(
      ui->list_available_button, "clicked", G_CALLBACK(package_query_on_list_available_button_clicked), widgets);

  g_signal_connect(ui->install_button, "clicked", G_CALLBACK(pending_transaction_on_install_button_clicked), widgets);

  g_signal_connect(
      ui->reinstall_button, "clicked", G_CALLBACK(pending_transaction_on_reinstall_button_clicked), widgets);

  g_signal_connect(ui->remove_button, "clicked", G_CALLBACK(pending_transaction_on_remove_button_clicked), widgets);

  g_signal_connect(ui->clear_button, "clicked", G_CALLBACK(package_query_on_clear_button_clicked), widgets);

  g_signal_connect(ui->search_button, "clicked", G_CALLBACK(package_query_on_search_button_clicked), widgets);

  g_signal_connect(ui->entry, "activate", G_CALLBACK(package_query_on_search_button_clicked), widgets);
  g_signal_connect(ui->history_list, "row-selected", G_CALLBACK(package_query_on_history_row_selected), widgets);

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

  g_signal_connect(ui->apply_button, "clicked", G_CALLBACK(pending_transaction_on_apply_button_clicked), widgets);
  g_signal_connect(
      ui->clear_pending_button, "clicked", G_CALLBACK(pending_transaction_on_clear_pending_button_clicked), widgets);

  g_signal_connect(ui->refresh_button, "clicked", G_CALLBACK(widgets_on_refresh_button_clicked), widgets);

  // Intercept window close so unapplied marked changes can be confirmed first.
  g_signal_connect(ui->window, "close-request", G_CALLBACK(on_main_window_close_request), widgets);

  // Save window geometry and paned position when window is unrealized (destroyed)
  g_signal_connect(ui->window,
                   "unrealize",
                   G_CALLBACK(+[](GtkWidget *widget, gpointer user_data) {
                     GtkWindow *w = GTK_WINDOW(widget);
                     config_save_window_geometry(w);
                     config_save_paned_position(GTK_PANED(user_data));
                   }),
                   ui->inner_paned);

  // Live-update: save pane position whenever the user moves the divider
  g_signal_connect(ui->inner_paned,
                   "notify::position",
                   G_CALLBACK(+[](GtkPaned *paned, GParamSpec *, gpointer) { config_save_paned_position(paned); }),
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

  widgets->window_state.pending_quit_dialog_open = true;

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, "Quit and discard marked changes?");
  gtk_window_set_default_size(dialog, 520, 180);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
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
                     widgets->window_state.allow_close_with_pending = true;

                     GtkRoot *dialog_root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (dialog_root && GTK_IS_WINDOW(dialog_root)) {
                       gtk_window_destroy(GTK_WINDOW(dialog_root));
                     }

                     GtkRoot *parent_root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
                     if (parent_root && GTK_IS_WINDOW(parent_root)) {
                       gtk_window_close(GTK_WINDOW(parent_root));
                     }
                   }),
                   widgets);

  g_signal_connect(dialog,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     widgets->window_state.pending_quit_dialog_open = false;
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
  config_save_window_geometry(window);
  if (widgets && widgets->results.inner_paned) {
    config_save_paned_position(widgets->results.inner_paned);
  }

  if (!widgets || widgets->window_state.allow_close_with_pending) {
    return FALSE;
  }

  if (widgets->transaction.actions.empty()) {
    return FALSE;
  }

  if (!widgets->window_state.pending_quit_dialog_open) {
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
                     if (widgets->window_state.backend_warmup_cancellable) {
                       g_cancellable_cancel(widgets->window_state.backend_warmup_cancellable);
                       g_object_unref(widgets->window_state.backend_warmup_cancellable);
                     }
                     if (widgets->query_state.package_list_cancellable) {
                       g_object_unref(widgets->query_state.package_list_cancellable);
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
        dnf_backend_refresh_installed_nevras();
        return TRUE; // keep repeating
      },
      nullptr);
}

// -----------------------------------------------------------------------------
// Start backend warm up after the first window show is out of the way.
// -----------------------------------------------------------------------------
static gboolean
start_backend_warmup_idle(gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  start_backend_warmup_task(widgets);
  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Start a quiet background task that warms up the shared DNF base.
// -----------------------------------------------------------------------------
static void
start_backend_warmup_task(SearchWidgets *widgets)
{
  if (!widgets || !widgets->window_state.backend_warmup_label) {
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(widgets->window_state.backend_warmup_label), TRUE);

  widgets->window_state.backend_warmup_cancellable = g_cancellable_new();

  GTask *task = g_task_new(G_OBJECT(widgets->window_state.backend_warmup_label),
                           widgets->window_state.backend_warmup_cancellable,
                           on_backend_warmup_task_finished,
                           nullptr);
  g_task_run_in_thread(task, on_backend_warmup_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Warm up BaseManager in the background so the first package query is faster.
// -----------------------------------------------------------------------------
static void
on_backend_warmup_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Backend warm up was cancelled.");
    return;
  }

  try {
    BaseManager::instance().acquire_read();
    if (g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Backend warm up was cancelled.");
      return;
    }
    g_task_return_boolean(task, TRUE);
  } catch (const std::exception &e) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
  } catch (...) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Backend warm up failed.");
  }
}

// -----------------------------------------------------------------------------
// Ignore warm up errors so startup stays quiet and normal queries handle them.
// -----------------------------------------------------------------------------
static void
on_backend_warmup_task_finished(GObject *source_object, GAsyncResult *result, gpointer)
{
  GtkLabel *label = GTK_LABEL(source_object);
  GError *error = nullptr;
  g_task_propagate_boolean(G_TASK(result), &error);

  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_clear_error(&error);
    return;
  }

  g_clear_error(&error);

  if (label) {
    gtk_widget_set_visible(GTK_WIDGET(label), FALSE);
  }
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
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), FALSE);
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

  // Warm up the shared backend after the window is on screen
  g_idle_add_full(G_PRIORITY_LOW, start_backend_warmup_idle, widgets, nullptr);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
