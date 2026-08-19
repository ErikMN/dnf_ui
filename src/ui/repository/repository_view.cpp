// -----------------------------------------------------------------------------
// src/ui/repository/repository_view.cpp
// Read-only repository list window
// -----------------------------------------------------------------------------
#include "ui/repository/repository_view.hpp"

#include "dnf5daemon_client/repository_service_client.hpp"
#include "i18n.hpp"
#include "ui/common/ui_helpers.hpp"

#include <gio/gio.h>

#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kRepositoryIdWidthChars = 28;
constexpr int kRepositoryStateWidthChars = 10;
constexpr int kRepositoryNameWidthChars = 48;
constexpr int kRepositoryNameMaxWidthChars = 80;

GtkWindow *g_repository_window = nullptr;

struct RepositoryWindowState {
  GtkWindow *window = nullptr;
  GtkListBox *list_box = nullptr;
  GtkLabel *status_label = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *refresh_button = nullptr;
  GCancellable *cancellable = nullptr;
  uint64_t load_id = 0;
  bool destroyed = false;
};

struct RepositoryLoadTaskData {
  std::shared_ptr<RepositoryWindowState> state;
  uint64_t load_id = 0;
};

void repository_view_start_load(const std::shared_ptr<RepositoryWindowState> &state);

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

  if (state->refresh_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->refresh_button), !loading);
  }

  if (!state->spinner) {
    return;
  }

  if (loading) {
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
repository_view_append_row(GtkListBox *list_box, const RepositoryInfo &repository)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  gtk_widget_set_margin_start(grid, 10);
  gtk_widget_set_margin_end(grid, 10);
  gtk_widget_set_margin_top(grid, 6);
  gtk_widget_set_margin_bottom(grid, 6);

  GtkWidget *id_label = gtk_label_new(repository.id.c_str());
  repository_view_prepare_text_cell(id_label, kRepositoryIdWidthChars, kRepositoryIdWidthChars, false);
  gtk_label_set_selectable(GTK_LABEL(id_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), id_label, 0, 0, 1, 1);

  GtkWidget *state_label = gtk_label_new(repository.enabled ? _("Enabled") : _("Disabled"));
  repository_view_prepare_text_cell(state_label, kRepositoryStateWidthChars, kRepositoryStateWidthChars, false);
  gtk_grid_attach(GTK_GRID(grid), state_label, 1, 0, 1, 1);

  GtkWidget *name_label = gtk_label_new(repository.name.c_str());
  repository_view_prepare_text_cell(name_label, kRepositoryNameWidthChars, kRepositoryNameMaxWidthChars, true);
  gtk_label_set_selectable(GTK_LABEL(name_label), TRUE);
  gtk_grid_attach(GTK_GRID(grid), name_label, 2, 0, 1, 1);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), grid);
  gtk_list_box_append(list_box, row);
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

  GtkWidget *id_label = gtk_label_new(_("Repository ID"));
  repository_view_prepare_text_cell(id_label, kRepositoryIdWidthChars, kRepositoryIdWidthChars, false);
  gtk_widget_add_css_class(id_label, "heading");
  gtk_grid_attach(GTK_GRID(grid), id_label, 0, 0, 1, 1);

  GtkWidget *state_label = gtk_label_new(_("Status"));
  repository_view_prepare_text_cell(state_label, kRepositoryStateWidthChars, kRepositoryStateWidthChars, false);
  gtk_widget_add_css_class(state_label, "heading");
  gtk_grid_attach(GTK_GRID(grid), state_label, 1, 0, 1, 1);

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

  repository_view_clear_list(state->list_box);
  for (const auto &repository : *repositories) {
    repository_view_append_row(state->list_box, repository);
  }

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

  if (state->cancellable) {
    g_cancellable_cancel(state->cancellable);
    g_object_unref(state->cancellable);
  }

  state->cancellable = g_cancellable_new();
  ++state->load_id;

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
// Open the read-only repository list window.
// -----------------------------------------------------------------------------
void
repository_view_show_window(GtkWindow *parent)
{
  if (g_repository_window) {
    gtk_window_present(g_repository_window);
    return;
  }

  auto state = std::make_shared<RepositoryWindowState>();

  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  state->window = window;
  g_repository_window = window;
  gtk_window_set_title(window, _("Repositories"));
  gtk_window_set_default_size(window, 720, 520);
  if (parent) {
    gtk_window_set_application(window, gtk_window_get_application(parent));
  }

  auto *state_holder = new std::shared_ptr<RepositoryWindowState>(state);
  g_object_set_data_full(G_OBJECT(window), "dnfui-repository-window-state", state_holder, [](gpointer p) {
    delete static_cast<std::shared_ptr<RepositoryWindowState> *>(p);
  });
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

  repository_view_start_load(state);
  gtk_window_present(window);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
