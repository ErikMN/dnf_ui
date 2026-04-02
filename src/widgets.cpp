// -----------------------------------------------------------------------------
// src/widgets.cpp
// Pending transaction, repository refresh, and shared widget helpers
// Handles mark/review/apply actions, refresh callbacks, and helper code
// shared by the split widget controller modules.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"
#include "base_manager.hpp"
#include "dnf_backend.hpp"
#include "transaction_progress.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

// Forward declarations
static void refresh_current_package_view(SearchWidgets *widgets);
static void start_apply_transaction(SearchWidgets *widgets);

// Shared cancellable helper used by background widget tasks.
GCancellable *
make_task_cancellable_for(GtkWidget *w)
{
  GCancellable *c = g_cancellable_new();
  if (w) {
    g_signal_connect_object(w, "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);
  }
  return c;
}

// Task payload for the async apply transaction worker.
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

void
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

void
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
