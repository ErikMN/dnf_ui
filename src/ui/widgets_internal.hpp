// src/ui/widgets_internal.hpp
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// Private helpers shared by the split widget controller implementation files
// -----------------------------------------------------------------------------
GCancellable *widgets_make_task_cancellable_for(GtkWidget *w);
// SearchWidgets must be owned by std::shared_ptr before calling this helper.
GTask *widgets_task_new_for_search_widgets(SearchWidgets *widgets, GCancellable *c, GAsyncReadyCallback callback);
bool widgets_task_should_skip_completion(GTask *task, SearchWidgets *widgets);
void widgets_spinner_acquire(GtkSpinner *spinner);
void widgets_spinner_release(GtkSpinner *spinner);
void widgets_on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *);
void widgets_on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
