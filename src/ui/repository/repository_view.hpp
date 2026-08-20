// -----------------------------------------------------------------------------
// src/ui/repository/repository_view.hpp
// Repository list window
// -----------------------------------------------------------------------------
#pragma once

#include <gtk/gtk.h>

#include <memory>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Open the repository list window.
// -----------------------------------------------------------------------------
void repository_view_show_window(GtkWindow *parent, const std::shared_ptr<MainWindowUiState> &main_widgets);

// -----------------------------------------------------------------------------
// Close the repository list window if it is open.
// -----------------------------------------------------------------------------
void repository_view_close_window();

// -----------------------------------------------------------------------------
// Return true while repository changes are being applied.
// -----------------------------------------------------------------------------
bool repository_view_is_applying_changes();

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
