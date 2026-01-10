// -----------------------------------------------------------------------------
// src/widgets.cpp
// Signal callbacks and search logic
// Handles user-triggered actions (search, clear, history, etc.)
// and asynchronous DNF queries for package information.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"
#include "ui_helpers.hpp"
#include "dnf_backend.hpp"
#include "config.hpp"
#include "base_manager.hpp"

// Forward declarations
static void add_to_history(SearchWidgets *widgets, const std::string &term);
static void perform_search(SearchWidgets *widgets, const std::string &term);

// Global cache for previous search results
static std::map<std::string, std::vector<std::string>> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache

// -----------------------------------------------------------------------------
// Task payload & cancellable helpers (snapshot cache key; cancel on widget destroy)
// -----------------------------------------------------------------------------
struct SearchTaskData {
  char *term;
  char *cache_key;
  // Snapshot of BaseManager generation at dispatch time.
  // Used to drop stale results if the backend Base is rebuilt while this task runs.
  uint64_t generation;
};

static void
search_task_data_free(gpointer p)
{
  SearchTaskData *d = static_cast<SearchTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->term);
  g_free(d->cache_key);
  g_free(d);
}

static GCancellable *
make_task_cancellable_for(GtkWidget *w)
{
  GCancellable *c = g_cancellable_new();
  if (w) {
    g_signal_connect_object(w, "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);
  }
  return c;
}

struct ApplyTaskData {
  std::vector<std::string> install;
  std::vector<std::string> remove;
};

static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  delete d;
}

// -----------------------------------------------------------------------------
// Update Apply button enabled state based on pending actions
// -----------------------------------------------------------------------------
static void
update_apply_button(SearchWidgets *widgets)
{
  if (!widgets || !widgets->apply_button) {
    return;
  }

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->apply_button), !widgets->pending.empty());
}

// -----------------------------------------------------------------------------
// Refresh Pending Actions tab
// -----------------------------------------------------------------------------
static void
refresh_pending_tab(SearchWidgets *widgets)
{
  // Clear existing rows
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->pending_list, 0)) {
    gtk_list_box_remove(widgets->pending_list, GTK_WIDGET(row));
  }

  // Re-add actions
  for (const auto &a : widgets->pending) {
    std::string line = (a.type == PendingAction::INSTALL ? "Install: " : "Remove: ") + a.nevra;
    GtkWidget *row = gtk_label_new(line.c_str());
    gtk_label_set_xalign(GTK_LABEL(row), 0.0);
    gtk_list_box_append(widgets->pending_list, row);
  }
  update_apply_button(widgets);
}

// -----------------------------------------------------------------------------
// Remove a pending action
// -----------------------------------------------------------------------------
static bool
remove_pending_action(SearchWidgets *widgets, const std::string &nevra)
{
  for (size_t i = 0; i < widgets->pending.size(); ++i) {
    if (widgets->pending[i].nevra == nevra) {
      widgets->pending.erase(widgets->pending.begin() + i);
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Find pending action type for a package
// -----------------------------------------------------------------------------
static bool
get_pending_action_type(SearchWidgets *widgets, const std::string &nevra, PendingAction::Type &out_type)
{
  for (const auto &a : widgets->pending) {
    if (a.nevra == nevra) {
      out_type = a.type;
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Spinner ref-count helpers (prevents one task from hiding spinner used by another)
// -----------------------------------------------------------------------------
static GQuark
spinner_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("dnfui-spinner-count");
  }

  return q;
}

static void
spinner_acquire(GtkSpinner *spinner)
{
  if (!spinner) {
    return;
  }

  GQuark q = spinner_quark();
  int count = GPOINTER_TO_INT(g_object_get_qdata(G_OBJECT(spinner), q));
  count++;
  g_object_set_qdata(G_OBJECT(spinner), q, GINT_TO_POINTER(count));

  if (count == 1) {
    gtk_widget_set_visible(GTK_WIDGET(spinner), TRUE);
    gtk_spinner_start(spinner);
  }
}

static void
spinner_release(GtkSpinner *spinner)
{
  if (!spinner) {
    return;
  }

  GQuark q = spinner_quark();
  int count = GPOINTER_TO_INT(g_object_get_qdata(G_OBJECT(spinner), q));
  if (count > 0) {
    count--;
    g_object_set_qdata(G_OBJECT(spinner), q, GINT_TO_POINTER(count));
  }

  if (count == 0) {
    gtk_spinner_stop(spinner);
    gtk_widget_set_visible(GTK_WIDGET(spinner), FALSE);
    g_object_set_qdata(G_OBJECT(spinner), q, nullptr);
  }
}

// -----------------------------------------------------------------------------
// Helpers: selection handling (supports both GtkListBox and GtkListView)
// -----------------------------------------------------------------------------
static bool
get_selected_package_from_listbox(GtkListBox *box, std::string &out_pkg)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row(box);
  if (!row) {
    return false;
  }

  GtkWidget *child = gtk_list_box_row_get_child(row);
  if (!GTK_IS_LABEL(child)) {
    return false;
  }

  const char *text = gtk_label_get_text(GTK_LABEL(child));
  if (!text || !*text) {
    return false;
  }
  out_pkg.assign(text);

  return true;
}

static bool
get_selected_package(SearchWidgets *widgets, std::string &out_pkg)
{
  if (!widgets || !widgets->list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->list_scroller);
  if (!child) {
    return false;
  }

  if (GTK_IS_LIST_VIEW(child)) {
    GtkListView *lv = GTK_LIST_VIEW(child);
    GtkSelectionModel *model = gtk_list_view_get_model(lv);
    if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
      return false;
    }

    GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
    guint index = gtk_single_selection_get_selected(sel);
    if (index == GTK_INVALID_LIST_POSITION) {
      return false;
    }

    GObject *obj = (GObject *)g_list_model_get_item(gtk_single_selection_get_model(sel), index);
    if (!obj) {
      return false;
    }

    const char *pkg_name = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
    bool ok = (pkg_name && *pkg_name);
    if (ok) {
      out_pkg.assign(pkg_name);
    }
    g_object_unref(obj);

    return ok;
  } else if (GTK_IS_LIST_BOX(child)) {
    return get_selected_package_from_listbox(GTK_LIST_BOX(child), out_pkg);
  }

  return false;
}

// -----------------------------------------------------------------------------
// Clear cached search results (called from Clear Cache button)
// -----------------------------------------------------------------------------
void
clear_search_cache()
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_search_cache.clear();
}

// -----------------------------------------------------------------------------
// Helper: Build a unique cache key based on search flags and term
// -----------------------------------------------------------------------------
static std::string
cache_key_for(const std::string &term)
{
  std::string key = (g_search_in_description.load() ? "desc:" : "name:");
  key += (g_exact_match.load() ? "exact:" : "contains:");
  key += term;

  return key;
}

// -----------------------------------------------------------------------------
// Async Operations
// GTK4 uses GTask for running expensive libdnf5 operations in worker threads
// without freezing the UI. These functions handle installed and available
// package queries off the main loop.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Async: Installed packages (non-blocking)
// Executes a background query using libdnf5 to fetch the list of installed
// packages. Runs in a worker thread via GTask to avoid blocking the GTK UI.
// Returns a std::vector<std::string> containing package names.
// -----------------------------------------------------------------------------

// Task data for list-installed operation.
// We snapshot the BaseManager generation at dispatch time so the UI can ignore
// results produced against an older Base after a rebuild/transaction.
struct ListTaskData {
  uint64_t generation;
};

static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    // Query all installed packages
    auto *results = new std::vector<std::string>(get_installed_packages());
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<std::string> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Async completion handler: Installed package listing
// Runs on the GTK main thread after on_list_task() finishes. Retrieves the
// result vector from the GTask, repopulates the package list UI, and updates
// the status message accordingly.
// -----------------------------------------------------------------------------
static void
on_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = (SearchWidgets *)user_data;

  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      spinner_release(widgets->spinner);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
      return;
    }
  }

  // Drop stale results if the backend Base changed while the worker was running.
  // This prevents rendering a list that no longer matches the current repo/system state.
  const ListTaskData *td = static_cast<const ListTaskData *>(g_task_get_task_data(task));
  if (td && td->generation != BaseManager::instance().current_generation()) {
    spinner_release(widgets->spinner);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
    return;
  }

  GError *error = nullptr;
  std::vector<std::string> *packages = (std::vector<std::string> *)g_task_propagate_pointer(task, &error);

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  // Re-enable UI after async list finishes
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    // Populate list asynchronously and update status
    fill_listbox_async(widgets, *packages, true); // NOTE: mark all installed packages
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    gtk_label_set_text(widgets->details_label, "Select a package for details.");
    delete packages;
  } else {
    set_status(widgets->status_label, error ? error->message : "Error listing packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Async: Search available packages (non-blocking)
// Executes a background libdnf5 query to find packages matching the search term.
// Runs off the GTK main thread via GTask to keep the UI responsive.
// Returns a std::vector<std::string> of matching package names.
// -----------------------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *)
{
  const SearchTaskData *td = static_cast<const SearchTaskData *>(task_data);
  const char *pattern = td ? td->term : "";
  try {
    auto *results = new std::vector<std::string>(search_available_packages(pattern));
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<std::string> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Called when background search finishes
// Updates UI, caches results, and repopulates the listbox
// -----------------------------------------------------------------------------
static void
on_search_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = (SearchWidgets *)user_data;

  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      spinner_release(widgets->spinner);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
      return;
    }
  }

  const SearchTaskData *td = static_cast<const SearchTaskData *>(g_task_get_task_data(task));
  if (td && td->generation != BaseManager::instance().current_generation()) {
    spinner_release(widgets->spinner);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
    return;
  }

  GError *error = nullptr;
  std::vector<std::string> *packages = (std::vector<std::string> *)g_task_propagate_pointer(task, &error);

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  // Re-enable UI
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    // Cache results for faster re-display next time (use dispatch-time key)
    if (td && td->cache_key) {
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      g_search_cache[td->cache_key] = *packages;
    }

    refresh_installed_nevras();

    // Fill UI list and display result count
    fill_listbox_async(widgets, *packages, true);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    delete packages;
  } else {
    set_status(widgets->status_label, error ? error->message : "Error or no results.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// UI callback: List Installed button
// Starts async listing of all installed packages
// -----------------------------------------------------------------------------
void
on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  set_status(widgets->status_label, "Listing installed packages...", "blue");

  // Show spinner (ref-counted)
  spinner_acquire(widgets->spinner);

  // Disable search controls while loading
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

  // Run query asynchronously
  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  // Store generation snapshot so completion can reject stale results.
  ListTaskData *td = new ListTaskData;
  td->generation = BaseManager::instance().current_generation();

  GTask *task = g_task_new(nullptr, c, on_list_task_finished, widgets);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<ListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// UI callback: Search button (or pressing Enter in entry field)
// Reads options, caches query, and triggers background search
// -----------------------------------------------------------------------------
void
on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->exact_checkbox));

  const char *txt = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  std::string pattern = txt ? txt : "";

  if (pattern.empty()) {
    return;
  }

  // Save search term and start lookup
  add_to_history(widgets, pattern);
  perform_search(widgets, pattern);
}

// -----------------------------------------------------------------------------
// UI callback: Selecting a search term from the history list
// -----------------------------------------------------------------------------
void
on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row) {
    return;
  }

  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GtkWidget *child = gtk_list_box_row_get_child(row);
  const char *term = gtk_label_get_text(GTK_LABEL(child));
  perform_search(widgets, term);
}

// -----------------------------------------------------------------------------
// UI callback: Clear List button
// Clears all displayed results, details and file info
// -----------------------------------------------------------------------------
void
on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;

  // Remove all listbox rows
  if (widgets->listbox) {
    while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->listbox, 0)) {
      gtk_list_box_remove(widgets->listbox, GTK_WIDGET(row));
    }
  }
  // Fallback: recreate empty scrolled window content if listbox unavailable
  else if (widgets->list_scroller) {
    GtkStringList *empty = gtk_string_list_new(nullptr);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(empty));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
    gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(lv));
  }

  // Reset UI labels
  gtk_label_set_text(widgets->count_label, "Items: 0");
  set_status(widgets->status_label, "Ready.", "gray");
  gtk_label_set_text(widgets->details_label, "");
  gtk_label_set_text(widgets->files_label, "");
}

// -----------------------------------------------------------------------------
// Add new search term to search history if not already present
// -----------------------------------------------------------------------------
static void
add_to_history(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Prevent duplicates in history
  for (const auto &s : widgets->history) {
    if (s == term) {
      return;
    }
  }

  // Append new term to internal list and UI widget
  widgets->history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->history_list, row);
}

// -----------------------------------------------------------------------------
// Perform search operation (cached or live)
// -----------------------------------------------------------------------------
static void
perform_search(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Ensure cache key reflects current checkboxes even when triggered from history
  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->exact_checkbox));

  gtk_editable_set_text(GTK_EDITABLE(widgets->entry), term.c_str());
  set_status(widgets->status_label, ("Searching for '" + term + "'...").c_str(), "blue");

  // Show spinner (ref-counted)
  spinner_acquire(widgets->spinner);

  // Disable search input and button during search
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

  // Check cache first
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_search_cache.find(cache_key_for(term));
    if (it != g_search_cache.end()) {
      // Use cached results and skip background thread
      spinner_release(widgets->spinner);
      fill_listbox_async(widgets, it->second, true);

      char msg[256];
      snprintf(msg, sizeof(msg), "Loaded %zu cached results.", it->second.size());
      set_status(widgets->status_label, msg, "gray");

      // Re-enable search controls
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

      return;
    }
  }

  // Otherwise perform real background search
  const std::string key = cache_key_for(term);
  SearchTaskData *td = static_cast<SearchTaskData *>(g_malloc0(sizeof *td));
  td->term = g_strdup(term.c_str());
  td->cache_key = g_strdup(key.c_str());
  td->generation = BaseManager::instance().current_generation();

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  GTask *task = g_task_new(nullptr, c, on_search_task_finished, widgets);
  g_task_set_task_data(task, td, search_task_data_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Async: Refresh repositories (non-blocking)
// Runs BaseManager::rebuild() in a worker thread so GTK stays responsive
// -----------------------------------------------------------------------------
void
on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    BaseManager::instance().rebuild();
    g_task_return_boolean(task, TRUE);
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Async completion handler: Refresh repositories
// -----------------------------------------------------------------------------
void
on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      return;
    }
  }
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GError *error = nullptr;
  gboolean success = g_task_propagate_boolean(task, &error);

  if (success) {
    set_status(widgets->status_label, "Repositories refreshed.", "green");
  } else {
    set_status(widgets->status_label, error ? error->message : "Repo refresh failed.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Helper: Rebuild base asynchronously and refresh installed highlights afterwards
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      return;
    }
  }
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GError *error = nullptr;
  gboolean ok = g_task_propagate_boolean(task, &error);

  if (!ok && error) {
    set_status(widgets->status_label, error->message, "red");
    g_error_free(error);
    return;
  }

  // Refresh installed set and rebind current list items to update "installed" highlight
  refresh_installed_nevras();

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->list_scroller);
  if (GTK_IS_LIST_VIEW(child)) {
    GtkListView *lv = GTK_LIST_VIEW(child);
    GtkSelectionModel *model = gtk_list_view_get_model(lv);
    if (GTK_IS_SINGLE_SELECTION(model)) {
      GtkStringList *store = GTK_STRING_LIST(gtk_single_selection_get_model(GTK_SINGLE_SELECTION(model)));
      std::vector<std::string> current_items;

      guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
      for (guint i = 0; i < n; ++i) {
        GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(store), i));
        const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
        if (text && *text) {
          current_items.emplace_back(text);
        }
        g_object_unref(obj);
      }
      fill_listbox_async(widgets, current_items, true);
    }
  } else if (GTK_IS_LIST_BOX(child)) {
    // Collect all current ListBox row labels and rebind
    std::vector<std::string> current_items;
    for (int i = 0;; ++i) {
      GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(child), i);
      if (!row) {
        break;
      }

      GtkWidget *c = gtk_list_box_row_get_child(row);
      if (GTK_IS_LABEL(c)) {
        const char *t = gtk_label_get_text(GTK_LABEL(c));
        if (t && *t) {
          current_items.emplace_back(t);
        }
      }
    }
    if (!current_items.empty()) {
      fill_listbox_async(widgets, current_items, true);
    }
  }
}

static void
rebuild_after_tx_async(SearchWidgets *widgets)
{
  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  GTask *task = g_task_new(nullptr, c, rebuild_after_tx_finished, widgets);
  g_task_run_in_thread(task, on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Async: Install selected package
// -----------------------------------------------------------------------------
void
on_install_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine selected package (supports ListBox or ListView)
  std::string pkg;
  if (!get_selected_package(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending install
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg, existing_type);
  if (has_existing && existing_type == PendingAction::INSTALL) {
    remove_pending_action(widgets, pkg);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg).c_str(), "gray");
  } else {
    // If it was pending REMOVE (or anything else), replace it with INSTALL
    remove_pending_action(widgets, pkg);
    widgets->pending.push_back({ PendingAction::INSTALL, pkg });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for install: " + pkg).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg);

  // Refresh list to apply pending highlight
  GtkWidget *child = gtk_scrolled_window_get_child(widgets->list_scroller);
  if (GTK_IS_LIST_VIEW(child)) {
    GtkListView *lv = GTK_LIST_VIEW(child);
    GtkSelectionModel *model = gtk_list_view_get_model(lv);
    GtkStringList *store = GTK_STRING_LIST(gtk_single_selection_get_model(GTK_SINGLE_SELECTION(model)));

    std::vector<std::string> current;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    for (guint i = 0; i < n; ++i) {
      GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(store), i));
      const char *t = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
      if (t && *t) {
        current.emplace_back(t);
      }
      g_object_unref(obj);
    }
    fill_listbox_async(widgets, current, true);
  }
}

// -----------------------------------------------------------------------------
// Async: Remove selected package
// -----------------------------------------------------------------------------
void
on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine selected package (supports ListBox or ListView)
  std::string pkg;
  if (!get_selected_package(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending remove
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg, existing_type);
  if (has_existing && existing_type == PendingAction::REMOVE) {
    remove_pending_action(widgets, pkg);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg).c_str(), "gray");
  } else {
    // If it was pending INSTALL (or anything else), replace it with REMOVE
    remove_pending_action(widgets, pkg);
    widgets->pending.push_back({ PendingAction::REMOVE, pkg });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for removal: " + pkg).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg);

  // Refresh list to apply pending highlight
  GtkWidget *child = gtk_scrolled_window_get_child(widgets->list_scroller);
  if (GTK_IS_LIST_VIEW(child)) {
    GtkListView *lv = GTK_LIST_VIEW(child);
    GtkSelectionModel *model = gtk_list_view_get_model(lv);
    GtkStringList *store = GTK_STRING_LIST(gtk_single_selection_get_model(GTK_SINGLE_SELECTION(model)));

    std::vector<std::string> current;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    for (guint i = 0; i < n; ++i) {
      GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(store), i));
      const char *t = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
      if (t && *t) {
        current.emplace_back(t);
      }
      g_object_unref(obj);
    }
    fill_listbox_async(widgets, current, true);
  }
}

// -----------------------------------------------------------------------------
// Apply pending actions in a single libdnf5 transaction (async via backend)
// -----------------------------------------------------------------------------
void
on_apply_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets->pending.empty()) {
    set_status(widgets->status_label, "No pending changes.", "gray");
    return;
  }

  // Build install/remove lists from pending actions
  ApplyTaskData *td = new ApplyTaskData;
  td->install.reserve(widgets->pending.size());
  td->remove.reserve(widgets->pending.size());

  for (const auto &a : widgets->pending) {
    if (a.type == PendingAction::INSTALL) {
      td->install.push_back(a.nevra);
    } else {
      td->remove.push_back(a.nevra);
    }
  }

  set_status(widgets->status_label, "Applying pending changes...", "blue");
  spinner_acquire(widgets->spinner);

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  GTask *task = g_task_new(
      nullptr,
      c,
      +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        if (GCancellable *c = g_task_get_cancellable(task)) {
          if (g_cancellable_is_cancelled(c)) {
            return;
          }
        }
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Stop spinner (ref-counted)
        spinner_release(widgets->spinner);

        if (success) {
          // Clear pending queue and refresh tab
          widgets->pending.clear();
          refresh_pending_tab(widgets);

          set_status(widgets->status_label, "Transaction successful.", "green");

          // Rebuild base and refresh installed highlighting asynchronously
          rebuild_after_tx_async(widgets);
        } else {
          set_status(widgets->status_label, error ? error->message : "Transaction failed.", "red");
          if (error) {
            g_error_free(error);
          }
        }
      },
      widgets);

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        bool ok = apply_transaction(td->install, td->remove, err);
        if (ok) {
          g_task_return_boolean(t, TRUE);
        } else {
          g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err.c_str()));
        }
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
