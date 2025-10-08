// src/ui_helpers.hpp
#pragma once

#include <string>
#include <vector>

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// UI utility helpers
// -----------------------------------------------------------------------------
void set_status(GtkLabel *label, const std::string &text, const std::string &color);
void fill_listbox_async(SearchWidgets *widgets, const std::vector<std::string> &items, bool highlight_installed);
