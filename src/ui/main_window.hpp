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
  SearchWidgets *widgets = nullptr;
  // Caller owns this reference and should release it after scheduling startup work.
  GCancellable *startup_cancellable = nullptr;
};

MainWindow main_window_create(GtkApplication *app);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
