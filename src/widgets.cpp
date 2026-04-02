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
#include "transaction_progress.hpp"

// Forward declarations
static void add_to_history(SearchWidgets *widgets, const std::string &term);
static void perform_search(SearchWidgets *widgets, const std::string &term);
static void refresh_current_package_view(SearchWidgets *widgets);
static void cancel_active_package_list_request(SearchWidgets *widgets);
static void start_apply_transaction(SearchWidgets *widgets);

// Global cache for previous search results
static std::map<std::string, std::vector<PackageRow>> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache

// -----------------------------------------------------------------------------
// Task payload & cancellable helpers (snapshot cache key; cancel on widget destroy)
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
  std::vector<std::string> reinstall;
  struct TransactionProgressWindow *progress_window;
};

static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  delete d;
}

// Split the pending queue into install, remove, and reinstall transaction specs.
static void
build_pending_transaction_specs(const SearchWidgets *widgets,
                                std::vector<std::string> &install,
                                std::vector<std::string> &remove,
                                std::vector<std::string> &reinstall)
{
  install.clear();
  remove.clear();
  reinstall.clear();

  if (!widgets) {
    return;
  }

  install.reserve(widgets->pending.size());
  remove.reserve(widgets->pending.size());
  reinstall.reserve(widgets->pending.size());

  for (const auto &action : widgets->pending) {
    if (action.type == PendingAction::INSTALL) {
      install.push_back(action.nevra);
    } else if (action.type == PendingAction::REINSTALL) {
      reinstall.push_back(action.nevra);
    } else {
      remove.push_back(action.nevra);
    }
  }
}

// -----------------------------------------------------------------------------
// FIXME: HACK: Scroll position helpers
// -----------------------------------------------------------------------------

// Saved scroll position used to restore the package list viewport after a refresh.
struct ScrollRestoreData {
  GtkAdjustment *hadj;
  GtkAdjustment *vadj;
  double hvalue;
  double vvalue;
};

// Release the adjustment references kept in the saved scroll-position snapshot.
static void
scroll_restore_data_free(gpointer p)
{
  ScrollRestoreData *d = static_cast<ScrollRestoreData *>(p);
  if (!d) {
    return;
  }

  if (d->hadj) {
    g_object_unref(d->hadj);
  }
  if (d->vadj) {
    g_object_unref(d->vadj);
  }

  delete d;
}

// Restore the saved scroll position once the refreshed view is back in place.
static gboolean
restore_scroll_position_idle(gpointer user_data)
{
  ScrollRestoreData *d = static_cast<ScrollRestoreData *>(user_data);
  if (!d) {
    return G_SOURCE_REMOVE;
  }

  if (d->hadj) {
    gtk_adjustment_set_value(d->hadj, d->hvalue);
  }
  if (d->vadj) {
    gtk_adjustment_set_value(d->vadj, d->vvalue);
  }

  return G_SOURCE_REMOVE;
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

  size_t pending_count = widgets->pending.size();
  bool has_pending = pending_count > 0;
  std::string apply_label = "Apply Transactions";
  if (has_pending) {
    apply_label += " (" + std::to_string(pending_count) + ")";
  }

  gtk_button_set_label(widgets->apply_button, apply_label.c_str());
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->apply_button), has_pending);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->clear_pending_button), has_pending);
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
    std::string prefix;
    switch (a.type) {
    case PendingAction::INSTALL:
      prefix = "Install: ";
      break;
    case PendingAction::REINSTALL:
      prefix = "Reinstall: ";
      break;
    case PendingAction::REMOVE:
      prefix = "Remove: ";
      break;
    }
    std::string line = prefix + a.nevra;
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

// Return true when the shared package-list request state currently owns a running task.
static bool
has_active_package_list_request(const SearchWidgets *widgets)
{
  return widgets && widgets->package_list_cancellable && !g_cancellable_is_cancelled(widgets->package_list_cancellable);
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
    return widgets->list_button;
  case PackageListRequestKind::LIST_AVAILABLE:
    return widgets->list_available_button;
  case PackageListRequestKind::SEARCH:
  case PackageListRequestKind::NONE:
  default:
    return widgets->search_button;
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

  if (widgets->package_list_cancellable) {
    g_object_unref(widgets->package_list_cancellable);
  }

  widgets->package_list_cancellable = G_CANCELLABLE(g_object_ref(c));
  widgets->current_package_list_request_id = request_id;
  widgets->current_package_list_request_kind = kind;
  GtkButton *stop_button = package_list_stop_button(widgets, kind);

  gtk_button_set_label(widgets->search_button, "Search");
  gtk_button_set_label(widgets->list_button, "List Installed");
  gtk_button_set_label(widgets->list_available_button, "List Available");
  gtk_button_set_label(stop_button, "Stop");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->desc_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->exact_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->history_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->list_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->list_available_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(stop_button), TRUE);
}

// Restore the normal search and list controls after a package query stops or finishes.
static void
restore_package_list_controls(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  gtk_button_set_label(widgets->search_button, "Search");
  gtk_button_set_label(widgets->list_button, "List Installed");
  gtk_button_set_label(widgets->list_available_button, "List Available");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->desc_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->exact_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->history_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->list_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->list_available_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
}

// Restore the shared package list UI when the active background request is done.
static void
end_package_list_request(SearchWidgets *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->current_package_list_request_id != request_id ||
      widgets->current_package_list_request_kind != kind) {
    return;
  }

  if (widgets->package_list_cancellable) {
    g_object_unref(widgets->package_list_cancellable);
    widgets->package_list_cancellable = nullptr;
  }
  widgets->current_package_list_request_id = 0;
  widgets->current_package_list_request_kind = PackageListRequestKind::NONE;
  restore_package_list_controls(widgets);
}

// Cancel the active package list request and immediately unlock the shared controls.
static void
cancel_active_package_list_request(SearchWidgets *widgets)
{
  if (!widgets || !widgets->package_list_cancellable) {
    return;
  }

  PackageListRequestKind kind = widgets->current_package_list_request_kind;
  GCancellable *c = widgets->package_list_cancellable;
  if (!g_cancellable_is_cancelled(c)) {
    g_cancellable_cancel(c);
  }

  // Release only the spinner slot owned by this request so other running tasks
  // can keep their progress indication visible.
  spinner_release(widgets->spinner);
  restore_package_list_controls(widgets);
  set_status(widgets->status_label, package_list_cancelled_status(kind), "gray");
}

// -----------------------------------------------------------------------------
// FIXME: HACK: Refresh the visible package rows after pending-action state changes
// Rebuilds the current package list presentation so status badges stay in sync
// with the pending transaction state.
// -----------------------------------------------------------------------------
static void
refresh_current_package_view(SearchWidgets *widgets)
{
  ScrollRestoreData *scroll = new ScrollRestoreData { nullptr, nullptr, 0.0, 0.0 };

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(widgets->list_scroller);
  if (hadj) {
    scroll->hadj = GTK_ADJUSTMENT(g_object_ref(hadj));
    scroll->hvalue = gtk_adjustment_get_value(hadj);
  }

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(widgets->list_scroller);
  if (vadj) {
    scroll->vadj = GTK_ADJUSTMENT(g_object_ref(vadj));
    scroll->vvalue = gtk_adjustment_get_value(vadj);
  }

  fill_package_view(widgets, widgets->current_packages);

  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, restore_scroll_position_idle, scroll, scroll_restore_data_free);
}

// Reset the details notebook after repopulating the main package view.
static void
reset_package_details_view(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  gtk_label_set_text(widgets->details_label, "Select a package for details.");
  gtk_label_set_text(widgets->files_label, "Select an installed package to view its file list.");
  gtk_label_set_text(widgets->deps_label, "Select a package to view dependencies.");
  gtk_label_set_text(widgets->changelog_label, "Select a package to view its changelog.");
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
// Returns a std::vector<PackageRow> containing structured package metadata.
// -----------------------------------------------------------------------------

// Task data for package-list operations started from the main action buttons.
// We snapshot the BaseManager generation at dispatch time so the UI can ignore
// results produced against an older Base after a rebuild or transaction.
// request_id keeps the active Stop button state matched to the task that
// currently owns it.
struct PackageListTaskData {
  uint64_t request_id;
  uint64_t generation;
};

static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    // Query all installed packages
    auto *results = new std::vector<PackageRow>(get_installed_package_rows_interruptible(cancellable));
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
    spinner_release(widgets->spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  }

  if (packages) {
    // Populate the package table and update status
    widgets->selected_nevra.clear();
    fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    reset_package_details_view(widgets);
    delete packages;
  } else {
    set_status(widgets->status_label, error ? error->message : "Error listing packages.", "red");
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
    auto *results = new std::vector<PackageRow>(get_available_package_rows_interruptible(cancellable));
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
    spinner_release(widgets->spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  }

  if (packages) {
    refresh_installed_nevras();

    widgets->selected_nevra.clear();
    fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu available packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    reset_package_details_view(widgets);
    delete packages;
  } else {
    set_status(widgets->status_label, error ? error->message : "Error listing available packages.", "red");
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
    auto *results = new std::vector<PackageRow>(search_available_package_rows_interruptible(pattern, cancellable));
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
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
    spinner_release(widgets->spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
  }

  if (packages) {
    // Cache results for faster re-display next time (use dispatch-time key)
    if (td && td->cache_key) {
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      g_search_cache[td->cache_key] = *packages;
    }

    refresh_installed_nevras();

    // Fill the package table and display result count
    widgets->selected_nevra.clear();
    fill_package_view(widgets, *packages);
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
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->current_package_list_request_kind == PackageListRequestKind::LIST_INSTALLED) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  set_status(widgets->status_label, "Listing installed packages...", "blue");

  // Show spinner (ref-counted)
  spinner_acquire(widgets->spinner);

  // Run query asynchronously
  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  // Store generation snapshot so completion can reject stale results.
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->next_package_list_request_id++;
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
on_list_available_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->current_package_list_request_kind == PackageListRequestKind::LIST_AVAILABLE) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  set_status(widgets->status_label, "Listing available packages...", "blue");

  // Show spinner (ref-counted)
  spinner_acquire(widgets->spinner);

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->next_package_list_request_id++;
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
on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->current_package_list_request_kind == PackageListRequestKind::SEARCH) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

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
on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  widgets->current_packages.clear();
  widgets->selected_nevra.clear();
  fill_package_view(widgets, {});

  // Reset UI labels and actions
  set_status(widgets->status_label, "Ready.", "gray");
  reset_package_details_view(widgets);
  update_action_button_labels(widgets, "");
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
  widgets->selected_nevra.clear();

  // Check cache first
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_search_cache.find(cache_key_for(term));
    if (it != g_search_cache.end()) {
      // Use cached results and skip background thread
      fill_package_view(widgets, it->second);

      char msg[256];
      snprintf(msg, sizeof(msg), "Loaded %zu cached results.", it->second.size());
      set_status(widgets->status_label, msg, "gray");

      return;
    }
  }

  // Otherwise perform real background search
  spinner_acquire(widgets->spinner);

  const std::string key = cache_key_for(term);
  SearchTaskData *td = static_cast<SearchTaskData *>(g_malloc0(sizeof *td));
  td->term = g_strdup(term.c_str());
  td->cache_key = g_strdup(key.c_str());
  td->request_id = widgets->next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
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

  // Refresh installed state and rebind the current package rows.
  refresh_installed_nevras();

  if (!widgets->current_packages.empty()) {
    refresh_current_package_view(widgets);
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

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!get_selected_package_row(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending install
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::INSTALL) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    // If it was pending REMOVE (or anything else), replace it with INSTALL
    remove_pending_action(widgets, pkg.nevra);
    widgets->pending.push_back({ PendingAction::INSTALL, pkg.nevra });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for install: " + pkg.name).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg.nevra);

  // Refresh the package table to apply pending-state badges.
  refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Async: Remove selected package
// -----------------------------------------------------------------------------
void
on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!get_selected_package_row(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending remove
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::REMOVE) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    // If it was pending INSTALL (or anything else), replace it with REMOVE
    remove_pending_action(widgets, pkg.nevra);
    widgets->pending.push_back({ PendingAction::REMOVE, pkg.nevra });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for removal: " + pkg.name).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg.nevra);

  // Refresh the package table to apply pending-state badges.
  refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Async: Reinstall selected package
// -----------------------------------------------------------------------------
void
on_reinstall_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  PackageRow pkg;
  if (!get_selected_package_row(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::REINSTALL) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    remove_pending_action(widgets, pkg.nevra);
    widgets->pending.push_back({ PendingAction::REINSTALL, pkg.nevra });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for reinstall: " + pkg.name).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg.nevra);

  refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Clears all pending install, remove, and reinstall actions without applying them
// -----------------------------------------------------------------------------
void
on_clear_pending_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets->pending.empty()) {
    set_status(widgets->status_label, "No pending actions to clear.", "blue");
    return;
  }

  size_t count = widgets->pending.size();
  widgets->pending.clear();
  refresh_pending_tab(widgets);

  // Refresh the package table to remove pending-state badges.
  refresh_current_package_view(widgets);

  char msg[256];
  snprintf(msg, sizeof(msg), "Cleared %zu pending action%s.", count, count == 1 ? "" : "s");
  set_status(widgets->status_label, msg, "green");
}

// -----------------------------------------------------------------------------
// Start the async apply flow after the user confirms the transaction summary.
static void
start_apply_transaction(SearchWidgets *widgets)
{
  ApplyTaskData *td = new ApplyTaskData;
  build_pending_transaction_specs(widgets, td->install, td->remove, td->reinstall);
  td->progress_window = create_transaction_progress_window(widgets, widgets->pending.size());

  append_transaction_progress(td->progress_window, "Queued transaction request.");
  set_status(widgets->status_label, "Applying pending changes. See transaction window for details.", "blue");
  spinner_acquire(widgets->spinner);

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  GTask *task = g_task_new(
      nullptr,
      c,
      +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        ApplyTaskData *td = static_cast<ApplyTaskData *>(g_task_get_task_data(task));
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

        finish_transaction_progress(td ? td->progress_window : nullptr, success, "");

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
        bool ok = apply_transaction(td->install, td->remove, td->reinstall, err, [td](const std::string &message) {
          append_transaction_progress(td->progress_window, message);
        });
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

  std::vector<std::string> install;
  std::vector<std::string> remove;
  std::vector<std::string> reinstall;
  build_pending_transaction_specs(widgets, install, remove, reinstall);

  // Resolve the full transaction first so the summary includes dependency-driven changes too.
  TransactionPreview preview;
  std::string error;
  if (!preview_transaction(install, remove, reinstall, preview, error)) {
    set_status(widgets->status_label, error.empty() ? "Unable to prepare transaction preview." : error, "red");
    return;
  }

  show_transaction_summary_dialog(widgets, preview, start_apply_transaction);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
