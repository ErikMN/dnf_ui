// -----------------------------------------------------------------------------
// src/package_query_controller.cpp
// Signal callbacks and async package query controller
// Handles search, list, clear, and history actions plus the shared Stop-button
// state for background package queries.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend.hpp"
#include "package_info_controller.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

#include <map>
#include <mutex>

// Cache one visible result set per search term and flag combination.
// Entries are tied to the BaseManager generation that produced them so a Base
// rebuild cannot serve stale package metadata back into the UI.
struct CachedSearchResults {
  uint64_t generation;
  std::vector<PackageRow> packages;
};

static std::map<std::string, CachedSearchResults> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache

// -----------------------------------------------------------------------------
// Clear cached search results.
// Used both by the Clear Cache button and after successful Base rebuilds.
// -----------------------------------------------------------------------------
void
package_query_clear_search_cache()
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
// Task payload helpers for background package queries
// -----------------------------------------------------------------------------
struct SearchTaskData {
  char *term;
  char *cache_key;
  uint64_t request_id;
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

// Task data for package-list operations started from the main action buttons.
// We snapshot the BaseManager generation at dispatch time so the UI can ignore
// results produced against an older Base after a rebuild or transaction.
// request_id keeps the active Stop button state matched to the task that
// currently owns it.
struct PackageListTaskData {
  uint64_t request_id;
  uint64_t generation;
};

// Return true when the shared package-list request state currently owns a running task.
static bool
has_active_package_list_request(const SearchWidgets *widgets)
{
  return widgets && widgets->query_state.package_list_cancellable &&
      !g_cancellable_is_cancelled(widgets->query_state.package_list_cancellable);
}

// Return the button that owns the Stop state for the active package list request.
static GtkButton *
package_list_stop_button(SearchWidgets *widgets, PackageListRequestKind kind)
{
  if (!widgets) {
    return nullptr;
  }

  switch (kind) {
  case PackageListRequestKind::LIST_INSTALLED:
    return widgets->query.list_button;
  case PackageListRequestKind::LIST_AVAILABLE:
    return widgets->query.list_available_button;
  case PackageListRequestKind::SEARCH:
  case PackageListRequestKind::NONE:
  default:
    return widgets->query.search_button;
  }
}

// Human-readable cancel status for the current background package list request.
static const char *
package_list_cancelled_status(PackageListRequestKind kind)
{
  switch (kind) {
  case PackageListRequestKind::SEARCH:
    return "Search cancelled.";
  case PackageListRequestKind::LIST_INSTALLED:
    return "Listing installed packages cancelled.";
  case PackageListRequestKind::LIST_AVAILABLE:
    return "Listing available packages cancelled.";
  case PackageListRequestKind::NONE:
  default:
    return "Operation cancelled.";
  }
}

// Track the active background package list request and switch the owning button to Stop.
static void
begin_package_list_request(SearchWidgets *widgets, GCancellable *c, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || !c) {
    return;
  }

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
  }

  widgets->query_state.package_list_cancellable = G_CANCELLABLE(g_object_ref(c));
  widgets->query_state.current_package_list_request_id = request_id;
  widgets->query_state.current_package_list_request_kind = kind;
  GtkButton *stop_button = package_list_stop_button(widgets, kind);

  gtk_button_set_label(widgets->query.search_button, "Search");
  gtk_button_set_label(widgets->query.list_button, "List Installed");
  gtk_button_set_label(widgets->query.list_available_button, "List Available");
  gtk_button_set_label(stop_button, "Stop");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(stop_button), TRUE);
}

// Restore the normal search and list controls after a package query stops or finishes.
static void
restore_package_list_controls(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  gtk_button_set_label(widgets->query.search_button, "Search");
  gtk_button_set_label(widgets->query.list_button, "List Installed");
  gtk_button_set_label(widgets->query.list_available_button, "List Available");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), TRUE);
}

// Restore the shared package list UI when the active background request is done.
static void
end_package_list_request(SearchWidgets *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->query_state.current_package_list_request_id != request_id ||
      widgets->query_state.current_package_list_request_kind != kind) {
    return;
  }

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
    widgets->query_state.package_list_cancellable = nullptr;
  }
  widgets->query_state.current_package_list_request_id = 0;
  widgets->query_state.current_package_list_request_kind = PackageListRequestKind::NONE;
  restore_package_list_controls(widgets);
}

// Cancel the active package list request and immediately unlock the shared controls.
static void
cancel_active_package_list_request(SearchWidgets *widgets)
{
  if (!widgets || !widgets->query_state.package_list_cancellable) {
    return;
  }

  uint64_t request_id = widgets->query_state.current_package_list_request_id;
  PackageListRequestKind kind = widgets->query_state.current_package_list_request_kind;
  GCancellable *c = widgets->query_state.package_list_cancellable;
  if (!g_cancellable_is_cancelled(c)) {
    g_cancellable_cancel(c);
  }

  // Release only the spinner slot owned by this request so other running tasks
  // can keep their progress indication visible.
  widgets_spinner_release(widgets->query.spinner);

  // Fully release request ownership so the UI and internal request state stay aligned.
  end_package_list_request(widgets, request_id, kind);

  ui_helpers_set_status(widgets->query.status_label, package_list_cancelled_status(kind), "gray");
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
// Returns a std::vector<PackageRow> containing structured package metadata.
// -----------------------------------------------------------------------------

static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    // Query all installed packages
    auto *results = new std::vector<PackageRow>(dnf_backend_get_installed_package_rows_interruptible(cancellable));
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
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
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      if (td) {
        end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
      }
      return;
    }
  }

  // Drop stale results if the backend Base changed while the worker was running.
  // This prevents rendering a list that no longer matches the current repo/system state.
  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Stop spinner (ref-counted)
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  }

  if (packages) {
    // Populate the package table and update status
    widgets->results.selected_nevra.clear();
    package_table_fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    package_info_reset_details_view(widgets);
    delete packages;
  } else {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error listing packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Async: Latest available packages (non-blocking)
// Executes a background query using libdnf5 to fetch the latest available
// package candidate rows. Runs in a worker thread via GTask to avoid blocking
// the GTK UI.
// -----------------------------------------------------------------------------
static void
on_list_available_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    auto *results = new std::vector<PackageRow>(dnf_backend_get_available_package_rows_interruptible(cancellable));
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Async completion handler: Latest available package listing
// Runs on the GTK main thread after on_list_available_task() finishes.
// Retrieves the result vector from the GTask, refreshes installed highlighting,
// and repopulates the package list UI.
// -----------------------------------------------------------------------------
static void
on_list_available_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      if (td) {
        end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
      }
      return;
    }
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Stop spinner (ref-counted)
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  }

  if (packages) {
    dnf_backend_refresh_installed_nevras();

    widgets->results.selected_nevra.clear();
    package_table_fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu available packages.", packages->size());
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    package_info_reset_details_view(widgets);
    delete packages;
  } else {
    ui_helpers_set_status(
        widgets->query.status_label, error ? error->message : "Error listing available packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Async: Search available packages (non-blocking)
// Executes a background libdnf5 query to find packages matching the search term.
// Runs off the GTK main thread via GTask to keep the UI responsive.
// Returns a std::vector<PackageRow> of matching package metadata.
// -----------------------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  const SearchTaskData *td = static_cast<const SearchTaskData *>(task_data);
  const char *pattern = td ? td->term : "";
  try {
    DNF_UI_TRACE(
        "Search task start request=%llu pattern=%s", td ? static_cast<unsigned long long>(td->request_id) : 0, pattern);
    auto *results =
        new std::vector<PackageRow>(dnf_backend_search_available_package_rows_interruptible(pattern, cancellable));
    DNF_UI_TRACE("Search task done request=%llu results=%zu",
                 td ? static_cast<unsigned long long>(td->request_id) : 0,
                 results->size());
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    DNF_UI_TRACE(
        "Search task failed request=%llu error=%s", td ? static_cast<unsigned long long>(td->request_id) : 0, e.what());
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
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GCancellable *c = g_task_get_cancellable(task);
  const SearchTaskData *td = static_cast<const SearchTaskData *>(g_task_get_task_data(task));

  if (c && g_cancellable_is_cancelled(c)) {
    if (td) {
      end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Stop spinner (ref-counted)
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
  }

  if (packages) {
    // Cache results for faster re-display next time.
    // Search results are only reusable while the backend Base generation stays
    // the same, otherwise repo state may have changed underneath the cache.
    if (td && td->cache_key) {
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      g_search_cache[td->cache_key] = CachedSearchResults { td->generation, *packages };
    }

    dnf_backend_refresh_installed_nevras();

    // Fill the package table and display result count
    widgets->results.selected_nevra.clear();
    package_table_fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    package_info_reset_details_view(widgets);
    delete packages;
  } else {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error or no results.", "red");
    if (error) {
      g_error_free(error);
    }
  }
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
  for (const auto &s : widgets->query_state.history) {
    if (s == term) {
      return;
    }
  }

  // Append new term to internal list and UI widget
  widgets->query_state.history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->query.history_list, row);
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
  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.exact_checkbox));

  gtk_editable_set_text(GTK_EDITABLE(widgets->query.entry), term.c_str());
  ui_helpers_set_status(widgets->query.status_label, ("Searching for '" + term + "'...").c_str(), "blue");
  widgets->results.selected_nevra.clear();

  // Check cache first.
  // Reuse only results produced from the current Base generation so refreshes
  // and transaction rebuilds cannot surface outdated package metadata.
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    uint64_t generation = BaseManager::instance().current_generation();
    auto it = g_search_cache.find(cache_key_for(term));
    if (it != g_search_cache.end()) {
      if (it->second.generation != generation) {
        g_search_cache.erase(it);
      } else {
        // Use cached results and skip background thread.
        package_table_fill_package_view(widgets, it->second.packages);

        char msg[256];
        snprintf(msg, sizeof(msg), "Loaded %zu cached results.", it->second.packages.size());
        ui_helpers_set_status(widgets->query.status_label, msg, "gray");
        package_info_reset_details_view(widgets);

        return;
      }
    }
  }

  // Otherwise perform real background search
  widgets_spinner_acquire(widgets->query.spinner);

  const std::string key = cache_key_for(term);
  SearchTaskData *td = static_cast<SearchTaskData *>(g_malloc0(sizeof *td));
  td->term = g_strdup(term.c_str());
  td->cache_key = g_strdup(key.c_str());
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  // The shared request helper owns disabling the search controls and flipping
  // the initiating Search button to Stop.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::SEARCH);
  GTask *task = g_task_new(nullptr, c, on_search_task_finished, widgets);
  g_task_set_task_data(task, td, search_task_data_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// UI callback: List Installed button
// Starts async listing of all installed packages
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_INSTALLED) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, "Listing installed packages...", "blue");

  // Show spinner (ref-counted)
  widgets_spinner_acquire(widgets->query.spinner);

  // Run query asynchronously
  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  // Store generation snapshot so completion can reject stale results.
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  // The shared request helper owns disabling the entry and flipping the
  // initiating button from List Installed to Stop.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  GTask *task = g_task_new(nullptr, c, on_list_task_finished, widgets);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// UI callback: List Available button
// Starts async listing of the latest available package candidates
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_available_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_AVAILABLE) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, "Listing available packages...", "blue");

  // Show spinner (ref-counted)
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  // The shared request helper owns disabling the entry and flipping the
  // initiating button from List Available to Stop.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  GTask *task = g_task_new(nullptr, c, on_list_available_task_finished, widgets);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_available_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// UI callback: Search button (or pressing Enter in entry field)
// Reads options, caches query, and triggers background search
// The same button acts as Stop while a search worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::SEARCH) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.exact_checkbox));

  const char *txt = gtk_editable_get_text(GTK_EDITABLE(widgets->query.entry));
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
package_query_on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row) {
    return;
  }

  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GtkWidget *child = gtk_list_box_row_get_child(row);
  const char *term = gtk_label_get_text(GTK_LABEL(child));
  perform_search(widgets, term);
}

// -----------------------------------------------------------------------------
// UI callback: Clear List button
// Clears all displayed results, details and file info
// -----------------------------------------------------------------------------
void
package_query_on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  cancel_active_package_list_request(widgets);

  widgets->results.current_packages.clear();
  widgets->results.selected_nevra.clear();
  package_table_fill_package_view(widgets, {});

  // Reset UI labels and actions
  ui_helpers_set_status(widgets->query.status_label, "Ready.", "gray");
  package_info_reset_details_view(widgets);
  ui_helpers_update_action_button_labels(widgets, "");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
