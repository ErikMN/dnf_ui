// -----------------------------------------------------------------------------
// src/widgets.cpp
// Repository refresh and shared widget helpers
// Handles refresh callbacks and helper code shared by the split widget
// controller modules.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"
#include "base_manager.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

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
void
refresh_current_package_view(SearchWidgets *widgets)
{
  ScrollRestoreData *scroll = new ScrollRestoreData { nullptr, nullptr, 0.0, 0.0 };

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(widgets->results.list_scroller);
  if (hadj) {
    scroll->hadj = GTK_ADJUSTMENT(g_object_ref(hadj));
    scroll->hvalue = gtk_adjustment_get_value(hadj);
  }

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(widgets->results.list_scroller);
  if (vadj) {
    scroll->vadj = GTK_ADJUSTMENT(g_object_ref(vadj));
    scroll->vvalue = gtk_adjustment_get_value(vadj);
  }

  fill_package_view(widgets, widgets->results.current_packages);

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
    set_status(widgets->query.status_label, "Repositories refreshed.", "green");
  } else {
    set_status(widgets->query.status_label, error ? error->message : "Repo refresh failed.", "red");
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
on_refresh_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  set_status(widgets->query.status_label, "Refreshing repositories...", "blue");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), FALSE);

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  GTask *task = g_task_new(nullptr, c, on_rebuild_task_finished, widgets);
  g_task_run_in_thread(task, on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
