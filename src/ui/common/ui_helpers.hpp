// -----------------------------------------------------------------------------
// src/ui/common/ui_helpers.hpp
// Shared UI helper functions
//
// Provides small helpers for common labels, status messages, and action buttons.
// -----------------------------------------------------------------------------
#pragma once

#include <string>

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Create a button with an icon and text label.
// -----------------------------------------------------------------------------
GtkWidget *ui_helpers_create_icon_button(const char *icon_name, const char *label);
// -----------------------------------------------------------------------------
// Create a button with an icon fallback for themes that miss the preferred icon.
// -----------------------------------------------------------------------------
GtkWidget *
ui_helpers_create_icon_button_with_fallback(const char *icon_name, const char *fallback_icon_name, const char *label);
// -----------------------------------------------------------------------------
// Update the icon and text label for an icon button.
// -----------------------------------------------------------------------------
void ui_helpers_set_icon_button(GtkButton *button, const char *icon_name, const char *label);
// -----------------------------------------------------------------------------
// Update the icon and text label with an icon fallback.
// -----------------------------------------------------------------------------
void ui_helpers_set_icon_button_with_fallback(GtkButton *button,
                                              const char *icon_name,
                                              const char *fallback_icon_name,
                                              const char *label);
// -----------------------------------------------------------------------------
// Resolve an icon name against the current GTK icon theme.
// -----------------------------------------------------------------------------
const char *ui_helpers_icon_name_with_fallback(const char *icon_name, const char *fallback_icon_name);
// -----------------------------------------------------------------------------
// Set the status label text and background color.
// -----------------------------------------------------------------------------
void ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color);
// -----------------------------------------------------------------------------
// Hide a timing label.
// -----------------------------------------------------------------------------
void ui_helpers_clear_duration_label(GtkLabel *label);
// -----------------------------------------------------------------------------
// Show elapsed time in a timing label.
// -----------------------------------------------------------------------------
void
ui_helpers_show_duration_label(GtkLabel *label, const char *title, const char *fallback_title, gint64 started_at_us);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
