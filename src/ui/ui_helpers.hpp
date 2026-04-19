// src/ui/ui_helpers.hpp
#pragma once

#include <string>

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// UI utility helpers
// -----------------------------------------------------------------------------
void ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color);
void ui_helpers_update_action_button_labels(SearchWidgets *widgets, const std::string &pkg);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
