// src/config.hpp
#pragma once

#include <map>
#include <string>
#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Configuration helpers
// -----------------------------------------------------------------------------
std::map<std::string, std::string> config_load_map();
int config_load_paned_position();
void config_save_map(const std::map<std::string, std::string> &config);
void config_save_paned_position(GtkPaned *paned);
void config_load_window_geometry(GtkWindow *window);
void config_save_window_geometry(GtkWindow *window);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
