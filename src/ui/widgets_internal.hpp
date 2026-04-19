// src/ui/widgets_internal.hpp
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Private helpers shared by the split widget controller implementation files
// -----------------------------------------------------------------------------
GCancellable *widgets_make_task_cancellable_for(GtkWidget *w);
void widgets_spinner_acquire(GtkSpinner *spinner);
void widgets_spinner_release(GtkSpinner *spinner);
void widgets_on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *);
void widgets_on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
