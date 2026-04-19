// src/ui_helpers.hpp
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <string>
#include <vector>

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// UI utility helpers
// -----------------------------------------------------------------------------
bool package_table_get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg);
void package_table_fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items);
void package_table_refresh_statuses(SearchWidgets *widgets);
void ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color);
void ui_helpers_update_action_button_labels(SearchWidgets *widgets, const std::string &pkg);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
