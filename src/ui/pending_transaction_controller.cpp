// -----------------------------------------------------------------------------
// src/ui/pending_transaction_controller.cpp
// Pending transaction and apply controller
// Handles mark/review/apply actions and the async follow-up refresh after
// transactions complete.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "package_info_controller.hpp"
#include "package_query_controller.hpp"
#include "package_table_view.hpp"
#include "pending_transaction_controller.hpp"
#include "pending_transaction_request.hpp"
#include "transaction_progress.hpp"
#include "transaction_service_client.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

// Task payload for the async apply transaction worker.
struct ApplyTaskData {
  std::string transaction_path;
  TransactionProgressWindow *progress_window;
};

// Task payload for the async transaction-preview worker.
struct PreviewTaskData {
  TransactionRequest request;
  TransactionPreview preview;
  std::string transaction_path;
};

// Button payload used to jump from one pending action back to its package row.
struct PendingJumpButtonData {
  SearchWidgets *widgets;
  PendingAction action;
};

static void update_apply_button(SearchWidgets *widgets);

static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  delete d;
}

static void
preview_task_data_free(gpointer p)
{
  PreviewTaskData *d = static_cast<PreviewTaskData *>(p);
  delete d;
}

static void
pending_jump_button_data_free(gpointer p)
{
  PendingJumpButtonData *d = static_cast<PendingJumpButtonData *>(p);
  delete d;
}

static void
invalidate_service_preview(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (!widgets->transaction.preview_transaction_path.empty()) {
    // Drop the prepared service request when the pending transaction changes or is dismissed.
    transaction_service_client_release_request(widgets->transaction.preview_transaction_path);
  }

  widgets->transaction.preview_transaction_path.clear();
}

// Explain why the running application package can be viewed but not modified
// from inside the same process.
static std::string
self_protected_transaction_message(const PackageRow &pkg)
{
  return "Cannot modify " + pkg.name + " while DNF UI is running. Close the application and use another tool.";
}

static const char *
pending_transaction_preview_busy_message()
{
  return "Wait for the current transaction preview to finish.";
}

static bool
pending_transaction_preview_is_busy(SearchWidgets *widgets)
{
  return widgets && widgets->transaction.preview_request_in_progress;
}

static void
set_preview_request_busy_state(SearchWidgets *widgets, bool busy)
{
  if (!widgets) {
    return;
  }

  widgets->transaction.preview_request_in_progress = busy;
  if (widgets->transaction.pending_list) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.pending_list), !busy);
  }

  if (busy) {
    ui_helpers_set_icon_button(widgets->transaction.apply_button, "emblem-ok-symbolic", "Preparing Preview...");
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), FALSE);
    return;
  }

  update_apply_button(widgets);
}

// Show the selected pending action in the main package list so it can be reviewed or changed.
static void
show_pending_action_package(SearchWidgets *widgets, const PendingAction &action)
{
  if (!widgets) {
    return;
  }

  std::vector<PackageRow> rows;
  switch (action.type) {
  case PendingAction::INSTALL:
    rows = dnf_backend_get_available_package_rows_by_nevra(action.nevra);
    break;
  case PendingAction::REMOVE:
  case PendingAction::REINSTALL:
    rows = dnf_backend_get_installed_package_rows_by_nevra(action.nevra);
    break;
  }

  if (rows.empty()) {
    ui_helpers_set_status(
        widgets->query.status_label, "Pending package could not be found in current package data.", "red");
    return;
  }

  // This temporary one-package review replaces the main query-backed table, so
  // post-transaction refreshes should rebuild it from the selected NEVRA
  // instead of replaying an older search or list view.
  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->results.selected_nevra = action.nevra;
  package_table_fill_package_view(widgets, rows);
}

// -----------------------------------------------------------------------------
// Update Apply button enabled state based on pending actions
// -----------------------------------------------------------------------------
static void
update_apply_button(SearchWidgets *widgets)
{
  if (!widgets || !widgets->transaction.apply_button || !widgets->transaction.clear_pending_button) {
    return;
  }

  size_t pending_count = widgets->transaction.actions.size();
  bool has_pending = pending_count > 0;
  std::string apply_label = "Apply Transactions";
  if (has_pending) {
    apply_label += " (" + std::to_string(pending_count) + ")";
  }

  ui_helpers_set_icon_button(widgets->transaction.apply_button, "emblem-ok-symbolic", apply_label.c_str());
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.apply_button), has_pending);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.clear_pending_button), has_pending);
}

// -----------------------------------------------------------------------------
// Refresh Pending Actions tab
// -----------------------------------------------------------------------------
static void
refresh_pending_tab(SearchWidgets *widgets)
{
  // Clear existing rows
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->transaction.pending_list, 0)) {
    gtk_list_box_remove(widgets->transaction.pending_list, GTK_WIDGET(row));
  }

  // Re-add actions
  for (const auto &a : widgets->transaction.actions) {
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

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_hexpand(button, TRUE);

    GtkWidget *label = gtk_label_new(line.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    gtk_button_set_child(GTK_BUTTON(button), label);

    PendingJumpButtonData *data = new PendingJumpButtonData { widgets, a };
    g_signal_connect_data(
        button,
        "clicked",
        G_CALLBACK(+[](GtkButton *, gpointer user_data) {
          PendingJumpButtonData *data = static_cast<PendingJumpButtonData *>(user_data);
          if (!data) {
            return;
          }

          show_pending_action_package(data->widgets, data->action);
        }),
        data,
        +[](gpointer data, GClosure *) { pending_jump_button_data_free(data); },
        GConnectFlags(0));

    gtk_list_box_append(widgets->transaction.pending_list, button);
  }
  update_apply_button(widgets);
}

// -----------------------------------------------------------------------------
// Remove a pending action
// -----------------------------------------------------------------------------
static bool
remove_pending_action(SearchWidgets *widgets, const std::string &nevra)
{
  for (size_t i = 0; i < widgets->transaction.actions.size(); ++i) {
    if (widgets->transaction.actions[i].nevra == nevra) {
      widgets->transaction.actions.erase(widgets->transaction.actions.begin() + i);
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
  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == nevra) {
      out_type = a.type;
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Helper: Rebuild base asynchronously and refresh installed highlights afterwards
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  gboolean ok = g_task_propagate_boolean(task, &error);

  if (!ok && error) {
    ui_helpers_set_status(widgets->query.status_label, error->message, "red");
    g_error_free(error);
    return;
  }

  // Transaction follow-up rebuilds produce a new Base generation, so any
  // cached search result rows must be discarded before the next search.
  package_query_clear_search_cache();

  // Refresh installed state, then repopulate the currently visible package
  // view so rows removed by the transaction disappear without a manual reload.
  dnf_backend_refresh_installed_nevras();
  package_query_reload_current_view(widgets);
}

static void
rebuild_after_tx_async(SearchWidgets *widgets)
{
  // Once the post-transaction rebuild begins, stop serving cached search
  // results from the pre-transaction Base generation.
  package_query_clear_search_cache();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, rebuild_after_tx_finished);
  g_task_run_in_thread(task, widgets_on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start the async apply flow after the user confirms the transaction summary.
// -----------------------------------------------------------------------------
static void
start_apply_transaction(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->transaction.preview_transaction_path.empty()) {
    ui_helpers_set_status(widgets->query.status_label, "No prepared transaction request is available.", "red");
    return;
  }

  ApplyTaskData *td = new ApplyTaskData;
  td->transaction_path = widgets->transaction.preview_transaction_path;
  td->progress_window = transaction_progress_create_window(widgets, widgets->transaction.actions.size());

  transaction_progress_append(td->progress_window, "Queued transaction request.");
  ui_helpers_set_status(
      widgets->query.status_label, "Applying pending changes. See transaction window for details.", "blue");
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        ApplyTaskData *td = static_cast<ApplyTaskData *>(g_task_get_task_data(task));
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Stop spinner (ref-counted)
        widgets_spinner_release(widgets->query.spinner);

        transaction_progress_finish(td ? td->progress_window : nullptr, success, "");

        if (success) {
          invalidate_service_preview(widgets);
          // Clear pending queue and refresh tab
          widgets->transaction.actions.clear();
          refresh_pending_tab(widgets);

          ui_helpers_set_status(widgets->query.status_label, "Transaction successful.", "green");

          // Rebuild base and refresh installed highlighting asynchronously
          rebuild_after_tx_async(widgets);
        } else {
          invalidate_service_preview(widgets);
          std::string details = error ? error->message : "Transaction failed.";
          ui_helpers_set_status(widgets->query.status_label, details.c_str(), "red");
          // Show the full backend error in a copyable dialog instead of only in the status bar.
          transaction_progress_show_error_dialog(widgets,
                                                 "Transaction Failed",
                                                 "The transaction could not be completed. Review the details below.",
                                                 details);
          if (error) {
            g_error_free(error);
          }
        }
      });

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        bool ok = transaction_service_client_apply_started_request(
            td->transaction_path,
            [td](const std::string &message) { transaction_progress_append(td->progress_window, message); },
            err);
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
// Async: Install selected package
// -----------------------------------------------------------------------------
void
pending_transaction_on_install_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!package_table_get_selected_package_row(widgets, pkg)) {
    ui_helpers_set_status(widgets->query.status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending install
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::INSTALL) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    // If it was pending REMOVE (or anything else), replace it with INSTALL
    remove_pending_action(widgets, pkg.nevra);
    widgets->transaction.actions.push_back({ PendingAction::INSTALL, pkg.nevra });
    refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, ("Marked for install: " + pkg.name).c_str(), "blue");
  }
  ui_helpers_update_action_button_labels(widgets, pkg.nevra);
  invalidate_service_preview(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
}

// -----------------------------------------------------------------------------
// Async: Remove selected package
// -----------------------------------------------------------------------------
void
pending_transaction_on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!package_table_get_selected_package_row(widgets, pkg)) {
    ui_helpers_set_status(widgets->query.status_label, "No package selected.", "gray");
    return;
  }

  // Only block self-removal for the exact installed row browse/search may also
  // contain related non-installed candidates for the same package name.
  if (dnf_backend_is_package_installed_exact(pkg) && dnf_backend_is_package_self_protected(pkg)) {
    ui_helpers_set_status(widgets->query.status_label, self_protected_transaction_message(pkg).c_str(), "red");
    return;
  }

  // Toggle pending remove
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::REMOVE) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    // If it was pending INSTALL (or anything else), replace it with REMOVE
    remove_pending_action(widgets, pkg.nevra);
    widgets->transaction.actions.push_back({ PendingAction::REMOVE, pkg.nevra });
    refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, ("Marked for removal: " + pkg.name).c_str(), "blue");
  }
  ui_helpers_update_action_button_labels(widgets, pkg.nevra);
  invalidate_service_preview(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);
}

// -----------------------------------------------------------------------------
// Async: Reinstall selected package
// -----------------------------------------------------------------------------
void
pending_transaction_on_reinstall_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  PackageRow pkg;
  if (!package_table_get_selected_package_row(widgets, pkg)) {
    ui_helpers_set_status(widgets->query.status_label, "No package selected.", "gray");
    return;
  }

  // Mirror the remove path and block reinstall only when this selected row is
  // the exact installed package that owns the running GUI executable.
  if (dnf_backend_is_package_installed_exact(pkg) && dnf_backend_is_package_self_protected(pkg)) {
    ui_helpers_set_status(widgets->query.status_label, self_protected_transaction_message(pkg).c_str(), "red");
    return;
  }

  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::REINSTALL) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    remove_pending_action(widgets, pkg.nevra);
    widgets->transaction.actions.push_back({ PendingAction::REINSTALL, pkg.nevra });
    refresh_pending_tab(widgets);
    ui_helpers_set_status(widgets->query.status_label, ("Marked for reinstall: " + pkg.name).c_str(), "blue");
  }
  ui_helpers_update_action_button_labels(widgets, pkg.nevra);
  invalidate_service_preview(widgets);

  package_table_refresh_statuses(widgets);
}

// -----------------------------------------------------------------------------
// Clears all pending install, remove, and reinstall actions without applying them
// -----------------------------------------------------------------------------
void
pending_transaction_on_clear_pending_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, "No pending actions to clear.", "blue");
    return;
  }

  size_t count = widgets->transaction.actions.size();
  widgets->transaction.actions.clear();
  invalidate_service_preview(widgets);
  refresh_pending_tab(widgets);

  // Refresh status badges without rebuilding the package table.
  package_table_refresh_statuses(widgets);

  char msg[256];
  snprintf(msg, sizeof(msg), "Cleared %zu pending action%s.", count, count == 1 ? "" : "s");
  ui_helpers_set_status(widgets->query.status_label, msg, "green");
}

// -----------------------------------------------------------------------------
// Resolve pending actions through the transaction service and confirm them
// -----------------------------------------------------------------------------
void
pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (pending_transaction_preview_is_busy(widgets)) {
    ui_helpers_set_status(widgets->query.status_label, pending_transaction_preview_busy_message(), "blue");
    return;
  }

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, "No pending changes.", "gray");
    return;
  }

  TransactionRequest request;
  std::string error;
  pending_transaction_build_request(widgets->transaction.actions, request);

  // Refuse self-protected transactions before asking the service to preview them.
  if (!pending_transaction_validate_request(request, error)) {
    ui_helpers_set_status(widgets->query.status_label, error.c_str(), "red");
    return;
  }

  invalidate_service_preview(widgets);
  ui_helpers_set_status(widgets->query.status_label, "Preparing transaction preview...", "blue");
  widgets_spinner_acquire(widgets->query.spinner);
  set_preview_request_busy_state(widgets, true);

  PreviewTaskData *td = new PreviewTaskData();
  td->request = std::move(request);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(
      widgets, c, +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        PreviewTaskData *td = static_cast<PreviewTaskData *>(g_task_get_task_data(task));

        if (widgets_task_should_skip_completion(task, widgets)) {
          return;
        }

        widgets_spinner_release(widgets->query.spinner);
        set_preview_request_busy_state(widgets, false);

        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);
        if (!success || !td) {
          const char *status_message =
              error && error->message ? error->message : "Unable to prepare transaction preview.";
          ui_helpers_set_status(widgets->query.status_label, status_message, "red");
          transaction_progress_show_error_dialog(widgets,
                                                 "Transaction Preview Failed",
                                                 "The transaction could not be prepared. Review the details below.",
                                                 status_message);
          if (error) {
            g_error_free(error);
          }
          return;
        }

        widgets->transaction.preview_transaction_path = td->transaction_path;
        transaction_progress_show_summary_dialog(
            widgets, td->preview, start_apply_transaction, invalidate_service_preview);
      });

  g_task_set_task_data(task, td, preview_task_data_free);
  g_task_run_in_thread(
      task, +[](GTask *task, gpointer, gpointer task_data, GCancellable *) {
        PreviewTaskData *td = static_cast<PreviewTaskData *>(task_data);
        std::string error;
        if (!td || !transaction_service_client_preview_request(td->request, td->preview, td->transaction_path, error)) {
          g_task_return_new_error(task,
                                  G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "%s",
                                  error.empty() ? "Unable to prepare transaction preview." : error.c_str());
          return;
        }

        g_task_return_boolean(task, TRUE);
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
