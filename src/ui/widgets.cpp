// -----------------------------------------------------------------------------
// src/ui/widgets.cpp
// Repository refresh and shared widget helpers
// Handles refresh callbacks and helper code shared by the split widget
// controller modules.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"
#include "base_manager.hpp"
#include "package_query_controller.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

namespace {

constexpr const char *kTaskSearchWidgetsHoldKey = "dnfui-task-search-widgets-hold";

}

// -----------------------------------------------------------------------------
// Shared cancellable helper used by background widget tasks.
// -----------------------------------------------------------------------------
GCancellable *
widgets_make_task_cancellable_for(GtkWidget *w)
{
  GCancellable *c = g_cancellable_new();
  if (w) {
    g_signal_connect_object(w, "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);
  }
  return c;
}

// -----------------------------------------------------------------------------
// hold_search_widgets_for_task
// -----------------------------------------------------------------------------
static void
hold_search_widgets_for_task(GTask *task, SearchWidgets *widgets)
{
  if (!task || !widgets) {
    return;
  }

  auto *held_widgets = new std::shared_ptr<SearchWidgets>(widgets->shared_from_this());
  g_object_set_data_full(G_OBJECT(task), kTaskSearchWidgetsHoldKey, held_widgets, [](gpointer p) {
    delete static_cast<std::shared_ptr<SearchWidgets> *>(p);
  });
}

// -----------------------------------------------------------------------------
// Create a task that keeps SearchWidgets alive until its completion callback returns.
// -----------------------------------------------------------------------------
GTask *
widgets_task_new_for_search_widgets(SearchWidgets *widgets, GCancellable *c, GAsyncReadyCallback callback)
{
  GTask *task = g_task_new(nullptr, c, callback, widgets);
  hold_search_widgets_for_task(task, widgets);
  return task;
}

// -----------------------------------------------------------------------------
// widgets_task_should_skip_completion
// -----------------------------------------------------------------------------
bool
widgets_task_should_skip_completion(GTask *task, SearchWidgets *widgets)
{
  if (!widgets || widgets->window_state.destroyed) {
    return true;
  }

  GCancellable *c = task ? g_task_get_cancellable(task) : nullptr;
  return c && g_cancellable_is_cancelled(c);
}

// -----------------------------------------------------------------------------
// Spinner ref-count helpers (prevents one task from hiding spinner used by another)
// -----------------------------------------------------------------------------
static GQuark
spinner_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("spinner-count");
  }

  return q;
}

// -----------------------------------------------------------------------------
// widgets_spinner_acquire
// -----------------------------------------------------------------------------
void
widgets_spinner_acquire(GtkSpinner *spinner)
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

// -----------------------------------------------------------------------------
// widgets_spinner_release
// -----------------------------------------------------------------------------
void
widgets_spinner_release(GtkSpinner *spinner)
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
// Async: Refresh repositories (non-blocking)
// Runs BaseManager::rebuild() in a worker thread so GTK stays responsive
// -----------------------------------------------------------------------------
void
widgets_on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    BaseRepoState refresh_state = BaseManager::instance().rebuild();
    // GTask completion transfers this heap value back to the GTK thread where
    // widgets_on_rebuild_task_finished() deletes it after reading the result.
    g_task_return_pointer(
        task, new BaseRepoState(refresh_state), [](gpointer p) { delete static_cast<BaseRepoState *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Async completion handler: Refresh repositories
// -----------------------------------------------------------------------------
void
widgets_on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  // When rebuild succeeds, this returns the heap value from widgets_on_rebuild_task().
  // This handler owns that pointer and must delete it after use.
  BaseRepoState *refresh_state = static_cast<BaseRepoState *>(g_task_propagate_pointer(task, &error));

  // Refresh temporarily disables the main Search button while the rebuild runs.
  // Restore it once the background refresh finishes so the query UI unlocks.
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), TRUE);

  if (refresh_state) {
    // Search caches are bound to the old Base generation and must be dropped
    // before the user can query against freshly refreshed repositories.
    package_query_clear_search_cache();
    dnf_backend_refresh_installed_nevras();
    if (*refresh_state == BaseRepoState::LIVE_METADATA) {
      ui_helpers_set_status(widgets->query.status_label, "Repositories refreshed.", "green");
    } else if (*refresh_state == BaseRepoState::CACHED_METADATA) {
      ui_helpers_set_status(
          widgets->query.status_label, "Live repo refresh failed. Using cached repository metadata.", "blue");
    } else {
      ui_helpers_set_status(
          widgets->query.status_label, "Live repo refresh failed. Showing installed packages only.", "blue");
    }
    package_query_reload_current_view(widgets);
    delete refresh_state;
  } else {
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Repo refresh failed.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// UI callback: Refresh repositories button
// Starts an asynchronous Base rebuild through the shared widget controller layer
// so application setup code does not depend on widget-internal task helpers.
// -----------------------------------------------------------------------------
void
widgets_on_refresh_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Once a rebuild starts, stop serving cached search results immediately so
  // the UI does not reuse rows from repo state that is actively changing.
  package_query_clear_search_cache();
  ui_helpers_set_status(widgets->query.status_label, "Refreshing repositories...", "blue");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), FALSE);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, widgets_on_rebuild_task_finished);
  g_task_run_in_thread(task, widgets_on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
