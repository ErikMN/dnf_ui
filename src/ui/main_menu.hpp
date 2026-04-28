// src/ui/main_menu.hpp
// Main window menu bar
//
// Owns the top menu bar and window-scoped actions for secondary UI commands.
#pragma once

#include <gtk/gtk.h>

struct SearchWidgets;

struct MainMenuWidgets {
  GtkWidget *window = nullptr;
  GtkWidget *history_panel = nullptr;
  GtkWidget *info_panel = nullptr;
};

// -----------------------------------------------------------------------------
// main_menu_create
// -----------------------------------------------------------------------------
GtkWidget *main_menu_create();
// -----------------------------------------------------------------------------
// main_menu_connect_actions
// -----------------------------------------------------------------------------
void main_menu_connect_actions(const MainMenuWidgets &menu_widgets, SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
