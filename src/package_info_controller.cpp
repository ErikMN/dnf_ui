// -----------------------------------------------------------------------------
// src/package_info_controller.cpp
// Package selection and details notebook controller
// Handles package selection state, action-button sensitivity, and the async
// package info load that updates the details notebook.
// -----------------------------------------------------------------------------
#include "package_info_controller.hpp"

#include "base_manager.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "widgets_internal.hpp"

#include <unistd.h>

// Task data for package-info operation.
// Snapshot generation at dispatch time so we can drop stale results after Base rebuild.
struct InfoTaskData {
  char *nevra;
  uint64_t generation;
};

static void
info_task_data_free(gpointer p)
{
  InfoTaskData *d = static_cast<InfoTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->nevra);
  g_free(d);
}

// Reset the details notebook after repopulating the main package view.
void
reset_package_details_view(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  gtk_label_set_text(widgets->results.details_label, "Select a package for details.");
  gtk_label_set_text(widgets->results.files_label, "Select an installed package to view its file list.");
  gtk_label_set_text(widgets->results.deps_label, "Select a package to view dependencies.");
  gtk_label_set_text(widgets->results.changelog_label, "Select a package to view its changelog.");
}

// Disable transaction actions when no package row is currently selected.
void
clear_selected_package_state(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  widgets->results.selected_nevra.clear();
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button), FALSE);
  update_action_button_labels(widgets, "");
}

// Enable only the transaction actions that make sense for the selected row.
static void
update_selected_package_actions(SearchWidgets *widgets, const PackageRow &selected)
{
  // Enable install for new packages and upgrade candidates, while
  // remove and reinstall stay reserved for the exact installed row.
  PackageInstallState install_state = get_package_install_state(selected);

  // FIXME: Replace with Polkit:
  bool is_root = (geteuid() == 0);

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button),
                           is_root && install_state != PackageInstallState::INSTALLED);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button),
                           is_root && install_state == PackageInstallState::INSTALLED);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button),
                           is_root && install_state == PackageInstallState::INSTALLED);
  update_action_button_labels(widgets, selected.nevra);
}

// Async worker: load the main package information text in the background.
static void
on_package_info_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    return;
  }

  InfoTaskData *td = static_cast<InfoTaskData *>(task_data);
  try {
    std::string info = get_package_info(td->nevra);
    g_task_return_pointer(task, g_strdup(info.c_str()), g_free);
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// Async completion handler: update the details notebook for the selected package.
static void
on_package_info_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      return;
    }
  }

  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const InfoTaskData *td = static_cast<const InfoTaskData *>(g_task_get_task_data(task));
  GError *error = nullptr;
  char *info = static_cast<char *>(g_task_propagate_pointer(task, &error));

  if (!td) {
    if (info) {
      g_free(info);
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (td->generation != BaseManager::instance().current_generation() || widgets->results.selected_nevra != td->nevra) {
    if (info) {
      g_free(info);
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!info) {
    set_status(widgets->query.status_label, error ? error->message : "Error loading info.", "red");
    if (error) {
      g_error_free(error);
    }
    return;
  }

  // Display general package information
  gtk_label_set_text(widgets->results.details_label, info);

  // Fetch and display the file list for the selected package
  try {
    std::string files = get_installed_package_files(td->nevra);
    gtk_label_set_text(widgets->results.files_label, files.c_str());
  } catch (const std::exception &e) {
    gtk_label_set_text(widgets->results.files_label, e.what());
  }

  // Fetch and display dependencies for the selected package
  try {
    std::string deps = get_package_deps(td->nevra);
    gtk_label_set_text(widgets->results.deps_label, deps.c_str());
  } catch (const std::exception &e) {
    gtk_label_set_text(widgets->results.deps_label, e.what());
  }

  // Fetch and display changelog
  try {
    std::string changelog = get_package_changelog(td->nevra);
    gtk_label_set_text(widgets->results.changelog_label, changelog.c_str());
  } catch (const std::exception &e) {
    gtk_label_set_text(widgets->results.changelog_label, e.what());
  }

  set_status(widgets->query.status_label, "Package info loaded.", "green");
  g_free(info);
}

// Start the async package info load for the newly selected package row.
void
load_selected_package_info(SearchWidgets *widgets, const PackageRow &selected)
{
  if (!widgets) {
    return;
  }

  widgets->results.selected_nevra = selected.nevra;
  set_status(widgets->query.status_label, "Fetching package info...", "blue");
  update_selected_package_actions(widgets, selected);

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = g_task_new(nullptr, c, on_package_info_task_finished, widgets);

  // Pass package NEVRA to background task
  InfoTaskData *td = static_cast<InfoTaskData *>(g_malloc0(sizeof *td));
  td->nevra = g_strdup(selected.nevra.c_str());
  td->generation = BaseManager::instance().current_generation();
  g_task_set_task_data(task, td, info_task_data_free);

  // Run background task to fetch metadata using dnf_backend
  g_task_run_in_thread(task, on_package_info_task);

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
