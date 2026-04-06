// -----------------------------------------------------------------------------
// src/pending_transaction_controller.cpp
// Pending transaction and apply controller
// Handles mark/review/apply actions and the async follow-up refresh after
// transactions complete.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"

#include "dnf_backend.hpp"
#include "transaction_progress.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

// Forward declarations
static void start_apply_transaction(SearchWidgets *widgets);

// Task payload for the async apply transaction worker.
struct ApplyTaskData {
  std::vector<std::string> install;
  std::vector<std::string> remove;
  std::vector<std::string> reinstall;
  TransactionProgressWindow *progress_window;
};

// Button payload used to jump from one pending action back to its package row.
struct PendingJumpButtonData {
  SearchWidgets *widgets;
  PendingAction action;
};

static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  delete d;
}

static void
pending_jump_button_data_free(gpointer p)
{
  PendingJumpButtonData *d = static_cast<PendingJumpButtonData *>(p);
  delete d;
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

  widgets->results.selected_nevra = action.nevra;
  package_table_fill_package_view(widgets, rows);
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

  install.reserve(widgets->transaction.actions.size());
  remove.reserve(widgets->transaction.actions.size());
  reinstall.reserve(widgets->transaction.actions.size());

  for (const auto &action : widgets->transaction.actions) {
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
// Update Apply button enabled state based on pending actions
// -----------------------------------------------------------------------------
static void
update_apply_button(SearchWidgets *widgets)
{
  if (!widgets || !widgets->transaction.apply_button) {
    return;
  }

  size_t pending_count = widgets->transaction.actions.size();
  bool has_pending = pending_count > 0;
  std::string apply_label = "Apply Transactions";
  if (has_pending) {
    apply_label += " (" + std::to_string(pending_count) + ")";
  }

  gtk_button_set_label(widgets->transaction.apply_button, apply_label.c_str());
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
  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      return;
    }
  }
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
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

  // Refresh installed state and rebind the current package rows.
  dnf_backend_refresh_installed_nevras();

  if (!widgets->results.current_packages.empty()) {
    widgets_refresh_current_package_view(widgets);
  }
}

static void
rebuild_after_tx_async(SearchWidgets *widgets)
{
  // Once the post-transaction rebuild begins, stop serving cached search
  // results from the pre-transaction Base generation.
  package_query_clear_search_cache();

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = g_task_new(nullptr, c, rebuild_after_tx_finished, widgets);
  g_task_run_in_thread(task, widgets_on_rebuild_task);
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

  // Refresh the package table to apply pending-state badges.
  widgets_refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Async: Remove selected package
// -----------------------------------------------------------------------------
void
pending_transaction_on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!package_table_get_selected_package_row(widgets, pkg)) {
    ui_helpers_set_status(widgets->query.status_label, "No package selected.", "gray");
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

  // Refresh the package table to apply pending-state badges.
  widgets_refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Async: Reinstall selected package
// -----------------------------------------------------------------------------
void
pending_transaction_on_reinstall_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  PackageRow pkg;
  if (!package_table_get_selected_package_row(widgets, pkg)) {
    ui_helpers_set_status(widgets->query.status_label, "No package selected.", "gray");
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

  widgets_refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Clears all pending install, remove, and reinstall actions without applying them
// -----------------------------------------------------------------------------
void
pending_transaction_on_clear_pending_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, "No pending actions to clear.", "blue");
    return;
  }

  size_t count = widgets->transaction.actions.size();
  widgets->transaction.actions.clear();
  refresh_pending_tab(widgets);

  // Refresh the package table to remove pending-state badges.
  widgets_refresh_current_package_view(widgets);

  char msg[256];
  snprintf(msg, sizeof(msg), "Cleared %zu pending action%s.", count, count == 1 ? "" : "s");
  ui_helpers_set_status(widgets->query.status_label, msg, "green");
}

// -----------------------------------------------------------------------------
// Start the async apply flow after the user confirms the transaction summary.
// -----------------------------------------------------------------------------
static void
start_apply_transaction(SearchWidgets *widgets)
{
  ApplyTaskData *td = new ApplyTaskData;
  build_pending_transaction_specs(widgets, td->install, td->remove, td->reinstall);
  td->progress_window = transaction_progress_create_window(widgets, widgets->transaction.actions.size());

  transaction_progress_append(td->progress_window, "Queued transaction request.");
  ui_helpers_set_status(
      widgets->query.status_label, "Applying pending changes. See transaction window for details.", "blue");
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
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
        widgets_spinner_release(widgets->query.spinner);

        transaction_progress_finish(td ? td->progress_window : nullptr, success, "");

        if (success) {
          // Clear pending queue and refresh tab
          widgets->transaction.actions.clear();
          refresh_pending_tab(widgets);

          ui_helpers_set_status(widgets->query.status_label, "Transaction successful.", "green");

          // Rebuild base and refresh installed highlighting asynchronously
          rebuild_after_tx_async(widgets);
        } else {
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
      },
      widgets);

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        bool ok = dnf_backend_apply_transaction(
            td->install, td->remove, td->reinstall, err, [td](const std::string &message) {
              transaction_progress_append(td->progress_window, message);
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
pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets->transaction.actions.empty()) {
    ui_helpers_set_status(widgets->query.status_label, "No pending changes.", "gray");
    return;
  }

  std::vector<std::string> install;
  std::vector<std::string> remove;
  std::vector<std::string> reinstall;
  build_pending_transaction_specs(widgets, install, remove, reinstall);

  // Resolve the full transaction first so the summary includes dependency-driven changes too.
  TransactionPreview preview;
  std::string error;
  if (!dnf_backend_preview_transaction(install, remove, reinstall, preview, error)) {
    const char *status_message = error.empty() ? "Unable to prepare transaction preview." : error.c_str();
    ui_helpers_set_status(widgets->query.status_label, status_message, "red");
    // Show the solver error in a copyable dialog before the transaction starts.
    transaction_progress_show_error_dialog(widgets,
                                           "Transaction Preview Failed",
                                           "The transaction could not be prepared. Review the details below.",
                                           error.empty() ? std::string(status_message) : error);
    return;
  }

  transaction_progress_show_summary_dialog(widgets, preview, start_apply_transaction);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
