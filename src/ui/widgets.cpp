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

// Shared cancellable helper used by background widget tasks.
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
widgets_on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
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

  // Refresh temporarily disables the main Search button while the rebuild runs.
  // Restore it once the background refresh finishes so the query UI unlocks.
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), TRUE);

  if (success) {
    // Search caches are bound to the old Base generation and must be dropped
    // before the user can query against freshly refreshed repositories.
    package_query_clear_search_cache();
    dnf_backend_refresh_installed_nevras();
    ui_helpers_set_status(widgets->query.status_label, "Repositories refreshed.", "green");
    package_query_reload_current_view(widgets);
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
  GTask *task = g_task_new(nullptr, c, widgets_on_rebuild_task_finished, widgets);
  g_task_run_in_thread(task, widgets_on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
