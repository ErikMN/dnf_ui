// -----------------------------------------------------------------------------
// src/ui/repository/repository_view.cpp
// Repository list window
// -----------------------------------------------------------------------------
#include "ui/repository/repository_view.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf5daemon_client/repository_service_client.hpp"
#include "i18n.hpp"
#include "ui/details/package_details_controller.hpp"
#include "ui/package_query/package_query_controller.hpp"
#include "ui/package_query/package_query_controller_internal.hpp"
#include "ui/refresh/repository_refresh_controller.hpp"
#include "ui/repository/repository_apply_model.hpp"
#include "ui/transaction/pending_transaction_apply.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/widgets.hpp"
#include "upgrade/daemon_upgrade_state.hpp"

#include <gio/gio.h>

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kRepositoryIdWidthChars = 28;
constexpr int kRepositoryStateWidthChars = 10;
constexpr int kRepositoryStateWidthPx = 90;
constexpr int kRepositoryNameWidthChars = 48;
constexpr int kRepositoryNameMaxWidthChars = 80;

struct RepositoryWindowState;

GtkWindow *g_repository_window = nullptr;
std::weak_ptr<RepositoryWindowState> g_repository_window_state;

struct RepositoryWindowState {
  GtkWindow *window = nullptr;
  GtkListBox *list_box = nullptr;
  GtkLabel *status_label = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *refresh_button = nullptr;
  GtkButton *apply_button = nullptr;
  GCancellable *cancellable = nullptr;
  std::weak_ptr<MainWindowUiState> main_widgets;
  std::map<std::string, bool> pending_enabled;
  uint64_t load_id = 0;
  bool needs_reload = false;
  bool loading = false;
  bool applying = false;
  bool destroyed = false;
};

struct RepositoryLoadTaskData {
  std::shared_ptr<RepositoryWindowState> state;
  uint64_t load_id = 0;
};

struct RepositoryToggleData {
  std::shared_ptr<RepositoryWindowState> state;
  std::string repository_id;
  bool original_enabled = false;
};

struct RepositoryApplyTaskData {
  std::shared_ptr<RepositoryWindowState> state;
  uint64_t load_id = 0;
  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
};

struct RepositoryApplyTaskResult {
  bool sync_needed = false;
  bool no_write_needed = false;
  bool clear_pending = false;
  RepositoryWriteOutcome write_outcome = RepositoryWriteOutcome::NOT_ATTEMPTED;
  RepositoryVerificationOutcome verification_outcome = RepositoryVerificationOutcome::NOT_RUN;
  RepositoryBackendSyncResult backend_sync_result = RepositoryBackendSyncResult::FAILED;
  bool repository_state_loaded = false;
  std::vector<RepositoryInfo> repositories;
  std::string error;
};

struct RepositoryReviewDialogData {
  std::shared_ptr<RepositoryWindowState> state;
  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
};

void repository_view_start_load(const std::shared_ptr<RepositoryWindowState> &state);

// -----------------------------------------------------------------------------
// Return the main application state if it is still alive.
// -----------------------------------------------------------------------------
std::shared_ptr<MainWindowUiState>
repository_view_main_widgets(const std::shared_ptr<RepositoryWindowState> &state)
{
  return state ? state->main_widgets.lock() : nullptr;
}

// -----------------------------------------------------------------------------
// Return the main window widget from the shared main UI state.
// -----------------------------------------------------------------------------
GtkWidget *
repository_view_main_window_widget(const std::shared_ptr<MainWindowUiState> &widgets)
{
  if (!widgets || !widgets->query.entry) {
    return nullptr;
  }

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  return root && GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : nullptr;
}

// -----------------------------------------------------------------------------
// Enable or disable the main window while repository changes are applying.
// -----------------------------------------------------------------------------
void
repository_view_set_main_window_sensitive(const std::shared_ptr<RepositoryWindowState> &state, bool sensitive)
{
  std::shared_ptr<MainWindowUiState> widgets = repository_view_main_widgets(state);
  GtkWidget *window = repository_view_main_window_widget(widgets);
  if (window) {
    gtk_widget_set_sensitive(window, sensitive);
  }
}

// -----------------------------------------------------------------------------
// Show why repository changes cannot be applied right now.
// -----------------------------------------------------------------------------
bool
repository_view_check_apply_preconditions(const std::shared_ptr<RepositoryWindowState> &state)
{
  std::shared_ptr<MainWindowUiState> widgets = repository_view_main_widgets(state);
  if (!state || !widgets || widgets->window_state.destroyed) {
    if (state && state->status_label) {
      gtk_label_set_text(state->status_label, _("The main window is not available."));
    }
    return false;
  }

  const char *message = nullptr;
  if (package_query_has_active_package_list_request(widgets.get())) {
    message = _("Wait for the current package query to finish.");
  } else if (repository_refresh_is_running()) {
    message = _("Wait for repository refresh to finish.");
  } else if (pending_transaction_preview_is_busy(widgets.get())) {
    message = pending_transaction_preview_busy_message();
  } else if (pending_transaction_apply_is_busy(widgets.get())) {
    message = pending_transaction_apply_busy_message();
  } else if (!widgets->transaction_state.preview_transaction_path.empty()) {
    message = _("Close the transaction preview before changing repositories.");
  }

  if (!message) {
    return true;
  }

  if (state->status_label) {
    gtk_label_set_text(state->status_label, message);
  }
  ui_helpers_set_status(widgets->query.status_label, message, "blue");
  return false;
}

// -----------------------------------------------------------------------------
// Return the main-window status text for a repository Apply backend sync.
// -----------------------------------------------------------------------------
const char *
repository_view_main_status_for_backend_sync(RepositoryBackendSyncResult sync_result, bool no_write_needed)
{
  if (no_write_needed) {
    switch (sync_result) {
    case RepositoryBackendSyncResult::LIVE_METADATA:
      return _("Repository state already matched the requested changes.");
    case RepositoryBackendSyncResult::CACHED_METADATA:
      return _("Repository state already matched the requested changes. Using cached repository metadata.");
    case RepositoryBackendSyncResult::INSTALLED_ONLY:
      return _("Repository state already matched the requested changes. Showing installed packages only.");
    case RepositoryBackendSyncResult::FAILED:
      return _("Repository changes may have changed package data. Reload packages before continuing.");
    }
  }

  switch (sync_result) {
  case RepositoryBackendSyncResult::LIVE_METADATA:
    return _("Repository changes applied. Package data refreshed.");
  case RepositoryBackendSyncResult::CACHED_METADATA:
    return _("Repository changes applied. Using cached repository metadata.");
  case RepositoryBackendSyncResult::INSTALLED_ONLY:
    return _("Repository changes applied. Showing installed packages only.");
  case RepositoryBackendSyncResult::FAILED:
    return _("Repository changes may have changed package data. Reload packages before continuing.");
  }

  return _("Repository changes may have changed package data. Reload packages before continuing.");
}

// -----------------------------------------------------------------------------
// Return the repository-window status text for a completed repository Apply.
// -----------------------------------------------------------------------------
const char *
repository_view_window_status_for_backend_sync(RepositoryBackendSyncResult sync_result, bool no_write_needed)
{
  if (no_write_needed) {
    switch (sync_result) {
    case RepositoryBackendSyncResult::LIVE_METADATA:
      return _("Repository state already matched the requested changes.");
    case RepositoryBackendSyncResult::CACHED_METADATA:
      return _("Repository state already matched the requested changes. Using cached repository metadata.");
    case RepositoryBackendSyncResult::INSTALLED_ONLY:
      return _("Repository state already matched the requested changes. Showing installed packages only.");
    case RepositoryBackendSyncResult::FAILED:
      return _("Failed to apply repository changes.");
    }
  }

  switch (sync_result) {
  case RepositoryBackendSyncResult::LIVE_METADATA:
    return _("Repository changes applied.");
  case RepositoryBackendSyncResult::CACHED_METADATA:
    return _("Repository changes applied. Using cached repository metadata.");
  case RepositoryBackendSyncResult::INSTALLED_ONLY:
    return _("Repository changes applied. Showing installed packages only.");
  case RepositoryBackendSyncResult::FAILED:
    return _("Failed to apply repository changes.");
  }

  return _("Failed to apply repository changes.");
}

// -----------------------------------------------------------------------------
// Return the main-window status text for an incomplete repository Apply.
// -----------------------------------------------------------------------------
const char *
repository_view_incomplete_main_status_for_backend_sync(RepositoryBackendSyncResult sync_result)
{
  switch (sync_result) {
  case RepositoryBackendSyncResult::LIVE_METADATA:
    return _("Repository changes may be incomplete. Package data refreshed.");
  case RepositoryBackendSyncResult::CACHED_METADATA:
    return _("Repository changes may be incomplete. Using cached repository metadata.");
  case RepositoryBackendSyncResult::INSTALLED_ONLY:
    return _("Repository changes may be incomplete. Showing installed packages only.");
  case RepositoryBackendSyncResult::FAILED:
    return _("Repository changes may have changed package data. Reload packages before continuing.");
  }

  return _("Repository changes may have changed package data. Reload packages before continuing.");
}

// -----------------------------------------------------------------------------
// Return true once a daemon write call reached dnf5daemon.
// -----------------------------------------------------------------------------
bool
repository_view_write_was_attempted(RepositoryWriteOutcome outcome)
{
  return outcome != RepositoryWriteOutcome::NOT_ATTEMPTED;
}

// -----------------------------------------------------------------------------
// Return true when the daemon write changed or may have changed repository state.
// -----------------------------------------------------------------------------
bool
repository_view_write_may_have_changed_state(RepositoryWriteOutcome outcome)
{
  return outcome == RepositoryWriteOutcome::SUCCEEDED || outcome == RepositoryWriteOutcome::PARTIAL;
}

// -----------------------------------------------------------------------------
// Return true when package data was rebuilt well enough to keep browsing.
// -----------------------------------------------------------------------------
bool
repository_view_backend_is_available(RepositoryBackendSyncResult sync_result)
{
  return sync_result != RepositoryBackendSyncResult::FAILED;
}

// -----------------------------------------------------------------------------
// Return the repository-window status for the completed Apply result.
// -----------------------------------------------------------------------------
const char *
repository_view_window_status_for_result(const RepositoryApplyTaskResult &result)
{
  if (result.no_write_needed) {
    if (repository_view_backend_is_available(result.backend_sync_result)) {
      return repository_view_window_status_for_backend_sync(result.backend_sync_result, true);
    }
    return _("Repository state already matched the requested changes, but package data could not be reloaded.");
  }

  if (result.write_outcome == RepositoryWriteOutcome::NOT_ATTEMPTED ||
      result.write_outcome == RepositoryWriteOutcome::FAILED) {
    return _("Failed to apply repository changes.");
  }

  if (result.write_outcome == RepositoryWriteOutcome::PARTIAL) {
    if (result.verification_outcome == RepositoryVerificationOutcome::UNAVAILABLE) {
      return _("Some repository changes were applied, but final state could not be verified.");
    }
    return _("Some repository changes were applied.");
  }

  if (result.verification_outcome == RepositoryVerificationOutcome::UNAVAILABLE) {
    return _("Repository changes were sent, but final state could not be verified.");
  }

  if (result.verification_outcome == RepositoryVerificationOutcome::MISMATCH) {
    return _("Repository changes were sent, but final state did not match the request.");
  }

  if (!repository_view_backend_is_available(result.backend_sync_result)) {
    return _("Repository changes applied, but package data could not be reloaded.");
  }

  return repository_view_window_status_for_backend_sync(result.backend_sync_result, false);
}

// -----------------------------------------------------------------------------
// Return the main-window status for the completed Apply result.
// -----------------------------------------------------------------------------
const char *
repository_view_main_status_for_result(const RepositoryApplyTaskResult &result)
{
  if (result.no_write_needed) {
    if (repository_view_backend_is_available(result.backend_sync_result)) {
      return repository_view_main_status_for_backend_sync(result.backend_sync_result, true);
    }
    return _("Repository state already matched the requested changes. Reload packages before continuing.");
  }

  if (!repository_view_write_may_have_changed_state(result.write_outcome)) {
    return repository_view_incomplete_main_status_for_backend_sync(result.backend_sync_result);
  }

  if (result.write_outcome == RepositoryWriteOutcome::PARTIAL) {
    return repository_view_incomplete_main_status_for_backend_sync(result.backend_sync_result);
  }

  if (result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      repository_view_backend_is_available(result.backend_sync_result)) {
    return repository_view_main_status_for_backend_sync(result.backend_sync_result, false);
  }

  if (result.write_outcome == RepositoryWriteOutcome::SUCCEEDED &&
      result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      !repository_view_backend_is_available(result.backend_sync_result)) {
    return _("Repository changes applied. Reload packages before continuing.");
  }

  return repository_view_incomplete_main_status_for_backend_sync(result.backend_sync_result);
}

// -----------------------------------------------------------------------------
// Return the main-window status color for the completed Apply result.
// -----------------------------------------------------------------------------
const char *
repository_view_main_status_color_for_result(const RepositoryApplyTaskResult &result)
{
  if (result.write_outcome == RepositoryWriteOutcome::SUCCEEDED &&
      result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      result.backend_sync_result == RepositoryBackendSyncResult::LIVE_METADATA) {
    return "green";
  }

  if (result.no_write_needed && result.backend_sync_result == RepositoryBackendSyncResult::LIVE_METADATA) {
    return "green";
  }

  if (result.no_write_needed && repository_view_backend_is_available(result.backend_sync_result)) {
    return "blue";
  }

  if (result.write_outcome == RepositoryWriteOutcome::SUCCEEDED &&
      result.verification_outcome == RepositoryVerificationOutcome::CONFIRMED &&
      repository_view_backend_is_available(result.backend_sync_result)) {
    return "blue";
  }

  return "red";
}

// -----------------------------------------------------------------------------
// Update the main package view after repository configuration may have changed.
// -----------------------------------------------------------------------------
void
repository_view_refresh_main_after_apply(const std::shared_ptr<MainWindowUiState> &widgets,
                                         const RepositoryApplyTaskResult &result)
{
  if (!widgets || widgets->window_state.destroyed) {
    return;
  }

  package_query_clear_search_cache();
  DaemonUpgradeState::instance().mark_stale();

  if (result.backend_sync_result == RepositoryBackendSyncResult::FAILED) {
    BaseManager::instance().drop_cached_base();
    package_query_on_clear_button_clicked(nullptr, widgets.get());
    ui_helpers_set_status(widgets->query.status_label, repository_view_main_status_for_result(result), "red");
    return;
  }

  bool cleared_upgradeable_table = package_query_clear_displayed_upgradeable_table(widgets.get());
  if (!cleared_upgradeable_table) {
    package_query_reload_current_view(widgets.get());
  }
  ui_helpers_set_status(widgets->query.status_label,
                        repository_view_main_status_for_result(result),
                        repository_view_main_status_color_for_result(result));
}

// -----------------------------------------------------------------------------
// Update action buttons from the current repository window state.
// -----------------------------------------------------------------------------
void
repository_view_update_action_buttons(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state) {
    return;
  }

  bool has_pending_changes = !state->pending_enabled.empty();
  if (state->refresh_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->refresh_button),
                             !state->loading && !state->applying && !has_pending_changes);
  }
  if (state->apply_button) {
    bool sensitive = has_pending_changes && !state->loading && !state->applying && !state->needs_reload;
    gtk_widget_set_sensitive(GTK_WIDGET(state->apply_button), sensitive);
  }
}

// -----------------------------------------------------------------------------
// Show the current pending repository change count.
// -----------------------------------------------------------------------------
void
repository_view_show_pending_status(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || !state->status_label) {
    return;
  }

  if (state->pending_enabled.empty()) {
    gtk_label_set_text(state->status_label, _("No repository changes pending."));
    return;
  }

  std::string message = dnfui_i18n_format_count(
      state->pending_enabled.size(), "%zu repository change is pending.", "%zu repository changes are pending.");
  gtk_label_set_text(state->status_label, message.c_str());
}

// -----------------------------------------------------------------------------
// Keep text cells from changing the column layout when values are long.
// -----------------------------------------------------------------------------
void
repository_view_prepare_text_cell(GtkWidget *label, int width_chars, int max_width_chars, bool expand)
{
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_width_chars(GTK_LABEL(label), width_chars);
  gtk_label_set_max_width_chars(GTK_LABEL(label), max_width_chars);
  gtk_widget_set_hexpand(label, expand);
}

// -----------------------------------------------------------------------------
// Remove all repository rows from the list.
// -----------------------------------------------------------------------------
void
repository_view_clear_list(GtkListBox *list_box)
{
  if (!list_box) {
    return;
  }

  while (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list_box))) {
    gtk_list_box_remove(list_box, child);
  }
}

// -----------------------------------------------------------------------------
// Update loading controls for the repository window.
// -----------------------------------------------------------------------------
void
repository_view_set_loading(const std::shared_ptr<RepositoryWindowState> &state, bool loading)
{
  if (!state || state->destroyed) {
    return;
  }

  state->loading = loading;
  if (state->list_box) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->list_box), !state->loading && !state->applying && !state->needs_reload);
  }
  repository_view_update_action_buttons(state);

  if (!state->spinner) {
    return;
  }

  if (state->loading || state->applying) {
    gtk_spinner_start(state->spinner);
    gtk_widget_set_visible(GTK_WIDGET(state->spinner), TRUE);
  } else {
    gtk_spinner_stop(state->spinner);
    gtk_widget_set_visible(GTK_WIDGET(state->spinner), FALSE);
  }
}

// -----------------------------------------------------------------------------
// Add one repository row.
// -----------------------------------------------------------------------------
void
repository_view_append_row(GtkListBox *list_box,
                           const RepositoryInfo &repository,
                           const std::shared_ptr<RepositoryWindowState> &state)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  gtk_widget_set_margin_start(grid, 10);
  gtk_widget_set_margin_end(grid, 10);
  gtk_widget_set_margin_top(grid, 6);
  gtk_widget_set_margin_bottom(grid, 6);

  GtkWidget *enabled_check = gtk_check_button_new();
  gtk_check_button_set_active(GTK_CHECK_BUTTON(enabled_check), repository.enabled);
  gtk_widget_set_size_request(enabled_check, kRepositoryStateWidthPx, -1);
  gtk_grid_attach(GTK_GRID(grid), enabled_check, 0, 0, 1, 1);

  GtkWidget *id_label = gtk_label_new(repository.id.c_str());
  repository_view_prepare_text_cell(id_label, kRepositoryIdWidthChars, kRepositoryIdWidthChars, false);
  gtk_label_set_selectable(GTK_LABEL(id_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), id_label, 1, 0, 1, 1);

  GtkWidget *name_label = gtk_label_new(repository.name.c_str());
  repository_view_prepare_text_cell(name_label, kRepositoryNameWidthChars, kRepositoryNameMaxWidthChars, true);
  gtk_label_set_selectable(GTK_LABEL(name_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), name_label, 2, 0, 1, 1);

  auto *toggle_data = new RepositoryToggleData {
    state,
    repository.id,
    repository.enabled,
  };
  g_signal_connect_data(
      enabled_check,
      "toggled",
      G_CALLBACK(+[](GtkCheckButton *check_button, gpointer user_data) {
        auto *toggle_data = static_cast<RepositoryToggleData *>(user_data);
        if (!toggle_data || !toggle_data->state || toggle_data->state->destroyed || toggle_data->state->loading ||
            toggle_data->state->applying) {
          return;
        }

        bool enabled = gtk_check_button_get_active(check_button);
        if (enabled == toggle_data->original_enabled) {
          toggle_data->state->pending_enabled.erase(toggle_data->repository_id);
        } else {
          toggle_data->state->pending_enabled[toggle_data->repository_id] = enabled;
        }

        repository_view_update_action_buttons(toggle_data->state);
        repository_view_show_pending_status(toggle_data->state);
      }),
      toggle_data,
      [](gpointer p, GClosure *) { delete static_cast<RepositoryToggleData *>(p); },
      G_CONNECT_DEFAULT);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), grid);
  gtk_list_box_append(list_box, row);
}

// -----------------------------------------------------------------------------
// Replace the visible repository rows with one daemon list result.
// -----------------------------------------------------------------------------
void
repository_view_render_repositories(const std::shared_ptr<RepositoryWindowState> &state,
                                    const std::vector<RepositoryInfo> &repositories)
{
  if (!state || state->destroyed || !state->list_box) {
    return;
  }

  repository_view_clear_list(state->list_box);
  for (const auto &repository : repositories) {
    repository_view_append_row(state->list_box, repository, state);
  }

  repository_view_update_action_buttons(state);
}

// -----------------------------------------------------------------------------
// Add the static table header.
// -----------------------------------------------------------------------------
GtkWidget *
repository_view_create_header()
{
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  gtk_widget_set_margin_start(grid, 10);
  gtk_widget_set_margin_end(grid, 10);
  gtk_widget_set_margin_top(grid, 6);
  gtk_widget_set_margin_bottom(grid, 6);

  GtkWidget *state_label = gtk_label_new(_("Enabled"));
  repository_view_prepare_text_cell(state_label, kRepositoryStateWidthChars, kRepositoryStateWidthChars, false);
  gtk_widget_add_css_class(state_label, "heading");
  gtk_widget_set_size_request(state_label, kRepositoryStateWidthPx, -1);
  gtk_grid_attach(GTK_GRID(grid), state_label, 0, 0, 1, 1);

  GtkWidget *id_label = gtk_label_new(_("Repository ID"));
  repository_view_prepare_text_cell(id_label, kRepositoryIdWidthChars, kRepositoryIdWidthChars, false);
  gtk_widget_add_css_class(id_label, "heading");
  gtk_grid_attach(GTK_GRID(grid), id_label, 1, 0, 1, 1);

  GtkWidget *name_label = gtk_label_new(_("Name"));
  repository_view_prepare_text_cell(name_label, kRepositoryNameWidthChars, kRepositoryNameMaxWidthChars, true);
  gtk_widget_add_css_class(name_label, "heading");
  gtk_grid_attach(GTK_GRID(grid), name_label, 2, 0, 1, 1);

  return grid;
}

// -----------------------------------------------------------------------------
// Load repositories on a worker thread.
// -----------------------------------------------------------------------------
void
on_repository_load_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  auto *repositories = new std::vector<RepositoryInfo>();
  std::string error;
  if (!repository_service_client_list(*repositories, error, cancellable)) {
    delete repositories;
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", _("Repository load was cancelled."));
    } else {
      g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, error.c_str()));
    }
    return;
  }

  g_task_return_pointer(task, repositories, [](gpointer p) { delete static_cast<std::vector<RepositoryInfo> *>(p); });
}

// -----------------------------------------------------------------------------
// Finish loading repositories on the GTK thread.
// -----------------------------------------------------------------------------
void
on_repository_load_finished(GObject *, GAsyncResult *result, gpointer)
{
  GTask *task = G_TASK(result);
  auto *task_data = static_cast<RepositoryLoadTaskData *>(g_task_get_task_data(task));
  std::shared_ptr<RepositoryWindowState> state = task_data ? task_data->state : nullptr;
  uint64_t load_id = task_data ? task_data->load_id : 0;

  if (!state || state->destroyed || load_id != state->load_id) {
    return;
  }

  repository_view_set_loading(state, false);

  if (state->cancellable) {
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }

  GError *error = nullptr;
  auto *repositories = static_cast<std::vector<RepositoryInfo> *>(g_task_propagate_pointer(task, &error));
  if (!repositories) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      std::string message = _("Failed to load repositories.");
      message += " ";
      message += error->message;
      gtk_label_set_text(state->status_label, message.c_str());
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  state->pending_enabled.clear();
  state->needs_reload = false;
  repository_view_render_repositories(state, *repositories);

  std::string message =
      dnfui_i18n_format_count(repositories->size(), "Showing %zu repository.", "Showing %zu repositories.");
  gtk_label_set_text(state->status_label, message.c_str());

  delete repositories;
}

// -----------------------------------------------------------------------------
// Start or restart the repository load.
// -----------------------------------------------------------------------------
void
repository_view_start_load(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || state->destroyed) {
    return;
  }

  if (!state->pending_enabled.empty()) {
    repository_view_show_pending_status(state);
    return;
  }

  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
  }

  state->cancellable = g_cancellable_new();
  ++state->load_id;
  state->pending_enabled.clear();

  repository_view_clear_list(state->list_box);
  repository_view_set_loading(state, true);
  gtk_label_set_text(state->status_label, _("Loading repositories..."));

  auto *task_data = new RepositoryLoadTaskData {
    state,
    state->load_id,
  };
  GTask *task = g_task_new(nullptr, state->cancellable, on_repository_load_finished, nullptr);
  g_task_set_task_data(task, task_data, [](gpointer p) { delete static_cast<RepositoryLoadTaskData *>(p); });
  g_task_run_in_thread(task, on_repository_load_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Apply pending repository changes on a worker thread.
// -----------------------------------------------------------------------------
void
on_repository_apply_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  const auto *task_data = static_cast<const RepositoryApplyTaskData *>(g_task_get_task_data(task));
  const std::vector<std::string> desired_enable_ids = task_data ? task_data->enable_ids : std::vector<std::string> {};
  const std::vector<std::string> desired_disable_ids = task_data ? task_data->disable_ids : std::vector<std::string> {};

  auto *apply_result = new RepositoryApplyTaskResult;
  std::vector<RepositoryInfo> current_repositories;
  std::string current_list_error;
  if (!repository_service_client_list(current_repositories, current_list_error, nullptr)) {
    apply_result->error =
        current_list_error.empty() ? _("Repository state could not be reloaded.") : current_list_error;
    g_task_return_pointer(task, apply_result, [](gpointer p) { delete static_cast<RepositoryApplyTaskResult *>(p); });
    return;
  }

  apply_result->repository_state_loaded = true;
  apply_result->repositories = current_repositories;

  RepositoryChangePlan plan =
      repository_apply_plan_changes(current_repositories, desired_enable_ids, desired_disable_ids);
  if (!plan.valid) {
    apply_result->error = plan.error;
    apply_result->sync_needed = true;
    apply_result->clear_pending = true;
    try {
      BaseRepoState repo_state = BaseManager::instance().rebuild(BaseRefreshMode::NORMAL);
      apply_result->backend_sync_result = repository_apply_backend_sync_result(true, repo_state);
    } catch (const std::exception &e) {
      apply_result->backend_sync_result = RepositoryBackendSyncResult::FAILED;
      if (!apply_result->error.empty()) {
        apply_result->error += " ";
      }
      apply_result->error += e.what();
    }
    g_task_return_pointer(task, apply_result, [](gpointer p) { delete static_cast<RepositoryApplyTaskResult *>(p); });
    return;
  }

  if (plan.enable_ids.empty() && plan.disable_ids.empty()) {
    apply_result->sync_needed = true;
    apply_result->no_write_needed = true;
    apply_result->clear_pending = true;
    try {
      BaseRepoState repo_state = BaseManager::instance().rebuild(BaseRefreshMode::NORMAL);
      apply_result->backend_sync_result = repository_apply_backend_sync_result(true, repo_state);
    } catch (const std::exception &e) {
      apply_result->backend_sync_result = RepositoryBackendSyncResult::FAILED;
      apply_result->error = e.what();
    }
    g_task_return_pointer(task, apply_result, [](gpointer p) { delete static_cast<RepositoryApplyTaskResult *>(p); });
    return;
  }

  RepositoryWriteResult write_result = repository_service_client_apply_changes(plan.enable_ids, plan.disable_ids);
  apply_result->write_outcome = repository_apply_write_outcome(write_result);
  apply_result->sync_needed = repository_view_write_was_attempted(apply_result->write_outcome);
  if (apply_result->write_outcome == RepositoryWriteOutcome::FAILED ||
      apply_result->write_outcome == RepositoryWriteOutcome::PARTIAL) {
    apply_result->error = write_result.error.empty() ? _("Repository change failed.") : write_result.error;
  }

  if (repository_view_write_was_attempted(apply_result->write_outcome)) {
    apply_result->clear_pending = true;
    try {
      BaseRepoState repo_state = BaseManager::instance().rebuild(BaseRefreshMode::NORMAL);
      apply_result->backend_sync_result = repository_apply_backend_sync_result(true, repo_state);
    } catch (const std::exception &e) {
      apply_result->backend_sync_result = RepositoryBackendSyncResult::FAILED;
      if (!apply_result->error.empty()) {
        apply_result->error += " ";
      }
      apply_result->error += e.what();
    }

    std::vector<RepositoryInfo> repositories;
    std::string list_error;
    if (repository_service_client_list(repositories, list_error, nullptr)) {
      apply_result->repository_state_loaded = true;
      apply_result->repositories = std::move(repositories);
      bool requested_states_match =
          repository_apply_requested_states_match(apply_result->repositories, desired_enable_ids, desired_disable_ids);
      apply_result->verification_outcome = repository_apply_verification_outcome(true, requested_states_match);
      if (apply_result->verification_outcome == RepositoryVerificationOutcome::MISMATCH) {
        if (!apply_result->error.empty()) {
          apply_result->error += " ";
        }
        apply_result->error += _("Some repository changes were not confirmed by dnf5daemon.");
      }
    } else {
      apply_result->repository_state_loaded = false;
      apply_result->verification_outcome = repository_apply_verification_outcome(false, false);
      if (!apply_result->error.empty()) {
        apply_result->error += " ";
      }
      apply_result->error += list_error.empty() ? _("Repository state could not be reloaded.") : list_error;
    }
  }

  g_task_return_pointer(task, apply_result, [](gpointer p) { delete static_cast<RepositoryApplyTaskResult *>(p); });
}

// -----------------------------------------------------------------------------
// Finish applying pending repository changes on the GTK thread.
// -----------------------------------------------------------------------------
void
on_repository_apply_finished(GObject *, GAsyncResult *result, gpointer)
{
  GTask *task = G_TASK(result);
  auto *task_data = static_cast<RepositoryApplyTaskData *>(g_task_get_task_data(task));
  std::shared_ptr<RepositoryWindowState> state = task_data ? task_data->state : nullptr;
  uint64_t load_id = task_data ? task_data->load_id : 0;

  if (!state || load_id != state->load_id) {
    return;
  }

  std::shared_ptr<MainWindowUiState> main_widgets = repository_view_main_widgets(state);
  repository_view_set_main_window_sensitive(state, true);

  state->applying = false;

  GError *error = nullptr;
  auto *apply_result = static_cast<RepositoryApplyTaskResult *>(g_task_propagate_pointer(task, &error));
  if (!apply_result) {
    if (state->destroyed) {
      if (error) {
        g_error_free(error);
      }
      return;
    }

    repository_view_set_loading(state, false);
    std::string message = _("Failed to apply repository changes.");
    if (error && error->message) {
      message += " ";
      message += error->message;
    }
    gtk_label_set_text(state->status_label, message.c_str());
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (apply_result->sync_needed) {
    repository_view_refresh_main_after_apply(main_widgets, *apply_result);
  }

  if (apply_result->clear_pending && apply_result->repository_state_loaded) {
    state->pending_enabled.clear();
    state->needs_reload = false;
    repository_view_render_repositories(state, apply_result->repositories);
  } else if (apply_result->clear_pending && repository_view_write_was_attempted(apply_result->write_outcome)) {
    state->pending_enabled.clear();
    state->needs_reload = true;
  }

  if (state->destroyed) {
    delete apply_result;
    return;
  }

  repository_view_set_loading(state, false);

  std::string message = repository_view_window_status_for_result(*apply_result);
  if (!apply_result->error.empty()) {
    message += " ";
    message += apply_result->error;
  }
  gtk_label_set_text(state->status_label, message.c_str());
  delete apply_result;
}

// -----------------------------------------------------------------------------
// Start applying repository changes after the user has reviewed them.
// -----------------------------------------------------------------------------
void
repository_view_start_apply_changes(const std::shared_ptr<RepositoryWindowState> &state,
                                    std::vector<std::string> enable_ids,
                                    std::vector<std::string> disable_ids)
{
  if (!state || state->destroyed || state->loading || state->applying || (enable_ids.empty() && disable_ids.empty())) {
    return;
  }

  if (!repository_view_check_apply_preconditions(state)) {
    return;
  }

  std::shared_ptr<MainWindowUiState> main_widgets = repository_view_main_widgets(state);
  state->applying = true;
  ++state->load_id;
  package_details_cancel_active_load(main_widgets.get());
  repository_view_set_main_window_sensitive(state, false);
  repository_view_set_loading(state, false);
  gtk_label_set_text(state->status_label, _("Applying repository changes..."));

  auto *task_data = new RepositoryApplyTaskData {
    state,
    state->load_id,
    std::move(enable_ids),
    std::move(disable_ids),
  };
  GTask *task = g_task_new(nullptr, nullptr, on_repository_apply_finished, nullptr);
  g_task_set_task_data(task, task_data, [](gpointer p) { delete static_cast<RepositoryApplyTaskData *>(p); });
  g_task_run_in_thread(task, on_repository_apply_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Add one repository review section to the confirmation dialog.
// -----------------------------------------------------------------------------
void
repository_view_add_review_section(GtkBox *box, const char *title, const std::vector<std::string> &repo_ids)
{
  if (!box || repo_ids.empty()) {
    return;
  }

  GtkWidget *heading = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(box, heading);

  for (const auto &repo_id : repo_ids) {
    GtkWidget *label = gtk_label_new(repo_id.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_focusable(label, FALSE);
    gtk_box_append(box, label);
  }
}

// -----------------------------------------------------------------------------
// Show a review dialog before applying repository changes.
// -----------------------------------------------------------------------------
void
repository_view_confirm_apply_changes(const std::shared_ptr<RepositoryWindowState> &state)
{
  if (!state || state->destroyed || state->loading || state->applying || state->pending_enabled.empty()) {
    return;
  }

  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
  for (const auto &[repo_id, enabled] : state->pending_enabled) {
    if (enabled) {
      enable_ids.push_back(repo_id);
    } else {
      disable_ids.push_back(repo_id);
    }
  }

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, _("Apply Repository Changes"));
  gtk_window_set_modal(dialog, TRUE);
  gtk_window_set_transient_for(dialog, state->window);
  gtk_window_set_destroy_with_parent(dialog, TRUE);
  gtk_window_set_default_size(dialog, 460, 360);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *question = gtk_label_new(_("Apply the following repository changes?"));
  gtk_label_set_xalign(GTK_LABEL(question), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(question), TRUE);
  gtk_box_append(GTK_BOX(outer), question);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(contents, 6);
  gtk_widget_set_margin_end(contents, 6);
  gtk_widget_set_margin_top(contents, 6);
  gtk_widget_set_margin_bottom(contents, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), contents);

  repository_view_add_review_section(GTK_BOX(contents), _("To be enabled"), enable_ids);
  repository_view_add_review_section(GTK_BOX(contents), _("To be disabled"), disable_ids);

  GtkWidget *summary = gtk_label_new(nullptr);
  std::string summary_text = dnfui_i18n_format_count(enable_ids.size() + disable_ids.size(),
                                                     "%zu repository change will be applied.",
                                                     "%zu repository changes will be applied.");
  gtk_label_set_text(GTK_LABEL(summary), summary_text.c_str());
  gtk_label_set_xalign(GTK_LABEL(summary), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(summary), TRUE);
  gtk_box_append(GTK_BOX(outer), summary);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *cancel_button = gtk_button_new_with_label(_("Cancel"));
  gtk_box_append(GTK_BOX(button_box), cancel_button);

  GtkWidget *apply_button = gtk_button_new_with_label(_("Apply"));
  gtk_widget_add_css_class(apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(button_box), apply_button);

  auto *dialog_data = new RepositoryReviewDialogData {
    state,
    std::move(enable_ids),
    std::move(disable_ids),
  };
  g_object_set_data_full(G_OBJECT(dialog), "dnfui-repository-review-data", dialog_data, [](gpointer p) {
    delete static_cast<RepositoryReviewDialogData *>(p);
  });

  g_signal_connect(cancel_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  g_signal_connect(apply_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     auto *dialog_data = static_cast<RepositoryReviewDialogData *>(user_data);
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     std::shared_ptr<RepositoryWindowState> state = dialog_data ? dialog_data->state : nullptr;
                     std::vector<std::string> enable_ids =
                         dialog_data ? dialog_data->enable_ids : std::vector<std::string> {};
                     std::vector<std::string> disable_ids =
                         dialog_data ? dialog_data->disable_ids : std::vector<std::string> {};

                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                     repository_view_start_apply_changes(state, std::move(enable_ids), std::move(disable_ids));
                   }),
                   dialog_data);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Keep the repository window open while a non-cancellable apply is running.
// -----------------------------------------------------------------------------
gboolean
on_repository_window_close_request(GtkWindow *, gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
  std::shared_ptr<RepositoryWindowState> state = state_holder ? *state_holder : nullptr;
  if (!state || !state->applying) {
    return FALSE;
  }

  if (state->status_label) {
    gtk_label_set_text(state->status_label, _("Repository changes are still being applied."));
  }
  return TRUE;
}

// -----------------------------------------------------------------------------
// Mark the repository window as destroyed and cancel any active load.
// -----------------------------------------------------------------------------
void
on_repository_window_destroy(GtkWidget *, gpointer user_data)
{
  auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
  if (!state_holder || !*state_holder) {
    return;
  }

  auto state = *state_holder;
  state->destroyed = true;
  if (g_repository_window == state->window) {
    g_repository_window = nullptr;
  }
  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
    state->cancellable = nullptr;
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Close the repository list window if it is open.
// -----------------------------------------------------------------------------
void
repository_view_close_window()
{
  if (g_repository_window) {
    gtk_window_close(g_repository_window);
  }
}

// -----------------------------------------------------------------------------
// Return true while repository changes are being applied.
// -----------------------------------------------------------------------------
bool
repository_view_is_applying_changes()
{
  std::shared_ptr<RepositoryWindowState> state = g_repository_window_state.lock();
  return state && state->applying;
}

// -----------------------------------------------------------------------------
// Open the repository list window.
// -----------------------------------------------------------------------------
void
repository_view_show_window(GtkWindow *parent, const std::shared_ptr<MainWindowUiState> &main_widgets)
{
  if (g_repository_window) {
    gtk_window_present(g_repository_window);
    return;
  }

  auto state = std::make_shared<RepositoryWindowState>();

  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  state->window = window;
  state->main_widgets = main_widgets;
  g_repository_window = window;
  g_repository_window_state = state;
  gtk_window_set_title(window, _("Repositories"));
  gtk_window_set_default_size(window, 720, 520);
  if (parent) {
    gtk_window_set_application(window, gtk_window_get_application(parent));
  }

  auto *state_holder = new std::shared_ptr<RepositoryWindowState>(state);
  g_object_set_data_full(G_OBJECT(window), "dnfui-repository-window-state", state_holder, [](gpointer p) {
    delete static_cast<std::shared_ptr<RepositoryWindowState> *>(p);
  });
  g_signal_connect(window, "close-request", G_CALLBACK(on_repository_window_close_request), state_holder);
  g_signal_connect(window, "destroy", G_CALLBACK(on_repository_window_destroy), state_holder);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_window_set_child(window, root);

  GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(root), top_row);

  GtkWidget *title_label = gtk_label_new(_("Configured repositories"));
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_widget_add_css_class(title_label, "heading");
  gtk_widget_set_hexpand(title_label, TRUE);
  gtk_box_append(GTK_BOX(top_row), title_label);

  GtkWidget *refresh_button = ui_helpers_create_icon_button("view-refresh-symbolic", _("Refresh"));
  gtk_box_append(GTK_BOX(top_row), refresh_button);
  state->refresh_button = GTK_BUTTON(refresh_button);

  GtkWidget *apply_button = ui_helpers_create_icon_button("system-run-symbolic", _("Apply"));
  gtk_widget_set_sensitive(apply_button, FALSE);
  gtk_box_append(GTK_BOX(top_row), apply_button);
  state->apply_button = GTK_BUTTON(apply_button);

  GtkWidget *status_label = gtk_label_new(_("Loading repositories..."));
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
  gtk_box_append(GTK_BOX(root), status_label);
  state->status_label = GTK_LABEL(status_label);

  gtk_box_append(GTK_BOX(root), repository_view_create_header());

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(root), scrolled);

  GtkWidget *list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_box);
  state->list_box = GTK_LIST_BOX(list_box);

  GtkWidget *bottom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(bottom_row, TRUE);
  gtk_box_append(GTK_BOX(root), bottom_row);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bottom_row), spacer);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(bottom_row), spinner);
  state->spinner = GTK_SPINNER(spinner);

  g_signal_connect(refresh_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
                     if (state_holder) {
                       repository_view_start_load(*state_holder);
                     }
                   }),
                   state_holder);
  g_signal_connect(apply_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *state_holder = static_cast<std::shared_ptr<RepositoryWindowState> *>(user_data);
                     if (state_holder) {
                       repository_view_confirm_apply_changes(*state_holder);
                     }
                   }),
                   state_holder);

  repository_view_start_load(state);
  gtk_window_present(window);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
