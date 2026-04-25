// src/ui/main_window.hpp
// Main application window
//
// Owns construction and wiring of the primary GTK window while keeping
// application lifecycle setup in app.cpp.
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

struct MainWindow {
  GtkWidget *window = nullptr;
  // Non-owning pointer used by startup code before window destruction.
  SearchWidgets *widgets = nullptr;
  // Caller owns this reference and should release it after scheduling startup work.
  GCancellable *startup_cancellable = nullptr;
};

MainWindow main_window_create(GtkApplication *app);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
