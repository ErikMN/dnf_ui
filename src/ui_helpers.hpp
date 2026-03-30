// src/ui_helpers.hpp
#pragma once

#include "dnf_backend.hpp"

#include <string>
#include <vector>

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// UI utility helpers
// -----------------------------------------------------------------------------
bool get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg);
void update_action_button_labels(SearchWidgets *widgets, const std::string &pkg);
void set_status(GtkLabel *label, const std::string &text, const std::string &color);
void fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
