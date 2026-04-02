// src/widgets_internal.hpp
#pragma once

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Private helpers shared by the split widget controller implementation files
// -----------------------------------------------------------------------------
GCancellable *make_task_cancellable_for(GtkWidget *w);
void spinner_acquire(GtkSpinner *spinner);
void spinner_release(GtkSpinner *spinner);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
