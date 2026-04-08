// -----------------------------------------------------------------------------
// src/package_info_controller.cpp
// Package selection and details notebook controller
// Handles package selection state, action-button sensitivity, and the async
// package info load that updates the details notebook.
// -----------------------------------------------------------------------------
#include "package_info_controller.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "widgets_internal.hpp"

#include <cstring>
#include <unistd.h>

// Task data for package-info operation.
// Snapshot generation at dispatch time so we can drop stale results after Base rebuild.
struct InfoTaskData {
  char *nevra;
  uint64_t generation;
};

// Text payload returned by the background package-info task.
struct InfoTaskResult {
  char *info;
  char *files;
  char *deps;
  char *changelog;
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

// Release the text payload returned by the background package-info task.
static void
info_task_result_free(gpointer p)
{
  InfoTaskResult *r = static_cast<InfoTaskResult *>(p);
  if (!r) {
    return;
  }

  g_free(r->info);
  g_free(r->files);
  g_free(r->deps);
  g_free(r->changelog);
  g_free(r);
}

// Complete the package-info task when the user cancels the current request.
static void
return_package_info_task_cancelled(GTask *task)
{
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Package info load was cancelled.");
}

// Replace text in a details notebook buffer.
static void
set_notebook_text(GtkTextBuffer *buffer, const char *text)
{
  if (!buffer) {
    return;
  }

  gtk_text_buffer_set_text(buffer, text ? text : "", -1);
}

// Reset the details notebook after repopulating the main package view.
void
package_info_reset_details_view(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  set_notebook_text(widgets->results.details_buffer, "Select a package for details.");
  set_notebook_text(widgets->results.files_buffer, "Select an installed package to view its file list.");
  set_notebook_text(widgets->results.deps_buffer, "Select a package to view dependencies.");
  set_notebook_text(widgets->results.changelog_buffer, "Select a package to view its changelog.");
}

// Disable transaction actions when no package row is currently selected.
void
package_info_clear_selected_package_state(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  widgets->results.selected_nevra.clear();
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button), FALSE);
  ui_helpers_update_action_button_labels(widgets, "");
}

// Enable only the transaction actions that make sense for the selected row.
static void
update_selected_package_actions(SearchWidgets *widgets, const PackageRow &selected)
{
  // Enable install for new packages and upgrade candidates, while
  // remove and reinstall stay reserved for the exact installed row.
  PackageInstallState install_state = dnf_backend_get_package_install_state(selected);

  // FIXME: Replace with Polkit:
  bool is_root = (geteuid() == 0);

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.install_button),
                           is_root && install_state != PackageInstallState::INSTALLED);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.remove_button),
                           is_root && install_state == PackageInstallState::INSTALLED);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->transaction.reinstall_button),
                           is_root && install_state == PackageInstallState::INSTALLED);
  ui_helpers_update_action_button_labels(widgets, selected.nevra);
}

// Async worker: load the package notebook text in the background.
static void
on_package_info_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    return_package_info_task_cancelled(task);
    return;
  }

  InfoTaskData *td = static_cast<InfoTaskData *>(task_data);
  try {
    DNF_UI_TRACE("Package info task start nevra=%s", td ? td->nevra : "");
    InfoTaskResult *result = static_cast<InfoTaskResult *>(g_malloc0(sizeof *result));

    result->info = g_strdup(dnf_backend_get_package_info(td->nevra).c_str());
    DNF_UI_TRACE("Package info details loaded nevra=%s bytes=%zu",
                 td ? td->nevra : "",
                 result->info ? std::strlen(result->info) : 0);

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      info_task_result_free(result);
      return_package_info_task_cancelled(task);
      return;
    }

    try {
      DNF_UI_TRACE("Package info files load start nevra=%s", td ? td->nevra : "");
      // NOTE: Limit displayed files to prevent X11 clipboard socket overflow on copy:
      result->files = g_strdup(dnf_backend_get_installed_package_files(td->nevra, 1500).c_str());
      DNF_UI_TRACE("Package info files loaded nevra=%s bytes=%zu",
                   td ? td->nevra : "",
                   result->files ? std::strlen(result->files) : 0);
    } catch (const std::exception &e) {
      result->files = g_strdup(e.what());
      DNF_UI_TRACE("Package info files failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      info_task_result_free(result);
      return_package_info_task_cancelled(task);
      return;
    }

    try {
      DNF_UI_TRACE("Package info dependencies load start nevra=%s", td ? td->nevra : "");
      result->deps = g_strdup(dnf_backend_get_package_deps(td->nevra).c_str());
      DNF_UI_TRACE("Package info dependencies loaded nevra=%s bytes=%zu",
                   td ? td->nevra : "",
                   result->deps ? std::strlen(result->deps) : 0);
    } catch (const std::exception &e) {
      result->deps = g_strdup(e.what());
      DNF_UI_TRACE("Package info dependencies failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      info_task_result_free(result);
      return_package_info_task_cancelled(task);
      return;
    }

    try {
      DNF_UI_TRACE("Package info changelog load start nevra=%s", td ? td->nevra : "");
      result->changelog = g_strdup(dnf_backend_get_package_changelog(td->nevra).c_str());
      DNF_UI_TRACE("Package info changelog loaded nevra=%s bytes=%zu",
                   td ? td->nevra : "",
                   result->changelog ? std::strlen(result->changelog) : 0);
    } catch (const std::exception &e) {
      result->changelog = g_strdup(e.what());
      DNF_UI_TRACE("Package info changelog failed nevra=%s error=%s", td ? td->nevra : "", e.what());
    }

    DNF_UI_TRACE("Package info task done nevra=%s", td ? td->nevra : "");
    g_task_return_pointer(task, result, info_task_result_free);
  } catch (const std::exception &e) {
    DNF_UI_TRACE("Package info task failed nevra=%s error=%s", td ? td->nevra : "", e.what());
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
  InfoTaskResult *result = static_cast<InfoTaskResult *>(g_task_propagate_pointer(task, &error));

  if (!td) {
    if (result) {
      info_task_result_free(result);
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (td->generation != BaseManager::instance().current_generation() || widgets->results.selected_nevra != td->nevra) {
    if (result) {
      info_task_result_free(result);
    }
    if (error) {
      g_error_free(error);
    }
    return;
  }

  if (!result) {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error loading info.", "red");
    if (error) {
      g_error_free(error);
    }
    return;
  }

  // Display general package information
  set_notebook_text(widgets->results.details_buffer, result->info ? result->info : "No details found.");

  // Display the file list fetched by the background task.
  set_notebook_text(widgets->results.files_buffer,
                    result->files ? result->files : "Select an installed package to view its file list.");

  // Display dependencies fetched by the background task.
  set_notebook_text(widgets->results.deps_buffer,
                    result->deps ? result->deps : "Select a package to view dependencies.");

  // Display changelog fetched by the background task.
  set_notebook_text(widgets->results.changelog_buffer,
                    result->changelog ? result->changelog : "Select a package to view its changelog.");

  ui_helpers_set_status(widgets->query.status_label, "Package info loaded.", "green");
  info_task_result_free(result);
}

// Start the async package info load for the newly selected package row.
void
package_info_load_selected_package_info(SearchWidgets *widgets, const PackageRow &selected)
{
  if (!widgets) {
    return;
  }

  widgets->results.selected_nevra = selected.nevra;
  ui_helpers_set_status(widgets->query.status_label, "Fetching package info...", "blue");
  update_selected_package_actions(widgets, selected);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
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
