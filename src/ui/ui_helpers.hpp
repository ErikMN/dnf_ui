// src/ui/ui_helpers.hpp
#pragma once

#include <string>

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// UI utility helpers
// -----------------------------------------------------------------------------
GtkWidget *ui_helpers_create_icon_button(const char *icon_name, const char *label);
// -----------------------------------------------------------------------------
// ui_helpers_set_icon_button
// -----------------------------------------------------------------------------
void ui_helpers_set_icon_button(GtkButton *button, const char *icon_name, const char *label);
// -----------------------------------------------------------------------------
// ui_helpers_set_status
// -----------------------------------------------------------------------------
void ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color);
// -----------------------------------------------------------------------------
// ui_helpers_update_action_button_labels
// -----------------------------------------------------------------------------
void ui_helpers_update_action_button_labels(SearchWidgets *widgets, const std::string &pkg);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
