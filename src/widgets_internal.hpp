// src/widgets_internal.hpp
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Private helpers shared by the split widget controller implementation files
// -----------------------------------------------------------------------------
GCancellable *make_task_cancellable_for(GtkWidget *w);
void spinner_acquire(GtkSpinner *spinner);
void spinner_release(GtkSpinner *spinner);
// Rebuild the current package view while preserving the visible scroll position.
void refresh_current_package_view(SearchWidgets *widgets);
// Run a non-blocking Base rebuild and its GTK completion handler.
void on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *);
void on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
