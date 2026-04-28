// src/config.hpp
#pragma once

#include <map>
#include <string>
#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Load all persisted configuration key value pairs.
// -----------------------------------------------------------------------------
std::map<std::string, std::string> config_load_map();
// -----------------------------------------------------------------------------
// Load the saved main paned divider position.
// -----------------------------------------------------------------------------
int config_load_paned_position();
// -----------------------------------------------------------------------------
// Save the full configuration key value map.
// -----------------------------------------------------------------------------
void config_save_map(const std::map<std::string, std::string> &config);
// -----------------------------------------------------------------------------
// Save the current main paned divider position.
// -----------------------------------------------------------------------------
void config_save_paned_position(GtkPaned *paned);
// -----------------------------------------------------------------------------
// Load the saved window size into the GTK window.
// -----------------------------------------------------------------------------
void config_load_window_geometry(GtkWindow *window);
// -----------------------------------------------------------------------------
// Save the current GTK window size.
// -----------------------------------------------------------------------------
void config_save_window_geometry(GtkWindow *window);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
