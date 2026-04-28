// src/ui/package_table_status.hpp
// Package table status rendering helpers
//
// Owns status text, sort priority, tooltip text, and CSS updates for the
// package table Status column.
#pragma once

#include "dnf_backend/dnf_backend.hpp"

#include <gtk/gtk.h>

struct SearchWidgets;

// -----------------------------------------------------------------------------
// package_table_status_text
// -----------------------------------------------------------------------------
const char *package_table_status_text(PackageInstallState state);
// -----------------------------------------------------------------------------
// package_table_status_rank
// -----------------------------------------------------------------------------
int package_table_status_rank(PackageInstallState state);
// -----------------------------------------------------------------------------
// package_table_clear_status_css
// -----------------------------------------------------------------------------
void package_table_clear_status_css(GtkWidget *label);
// -----------------------------------------------------------------------------
// package_table_update_status_label
// -----------------------------------------------------------------------------
void package_table_update_status_label(GtkWidget *label, SearchWidgets *widgets, const PackageRow &row);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
