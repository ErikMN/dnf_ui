// src/ui/package_query_controller.hpp
// Public package query controller entry points
//
// Owns the GTK callbacks and refresh hooks for search, package listing,
// query history, and package-query cache invalidation.
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

void package_query_on_list_button_clicked(GtkButton *, gpointer user_data);
void package_query_on_list_available_button_clicked(GtkButton *, gpointer user_data);
void package_query_on_search_button_clicked(GtkButton *, gpointer user_data);
void package_query_on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data);
void package_query_on_clear_button_clicked(GtkButton *, gpointer user_data);
void package_query_clear_search_cache();
void package_query_reload_current_view(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
