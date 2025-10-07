#pragma once

#include <map>
#include <string>
#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Configuration helpers
// -----------------------------------------------------------------------------
std::map<std::string, std::string> load_config_map();
void save_config_map(const std::map<std::string, std::string> &config);
int load_paned_position();
void save_paned_position(GtkPaned *paned);
void load_window_geometry(GtkWindow *window);
void save_window_geometry(GtkWindow *window);
