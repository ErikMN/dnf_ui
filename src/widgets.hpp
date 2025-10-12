// src/widgets.hpp
#pragma once

#include <string>
#include <vector>

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Struct for holding UI elements and signal callbacks
// -----------------------------------------------------------------------------
struct SearchWidgets {
  GtkEntry *entry;
  GtkListBox *listbox;
  GtkScrolledWindow *list_scroller;
  GtkListBox *history_list;
  GtkSpinner *spinner;
  GtkButton *search_button;
  GtkLabel *status_label;
  GtkLabel *details_label;
  GtkLabel *count_label;
  GtkCheckButton *desc_checkbox;
  GtkCheckButton *exact_checkbox;
  GtkLabel *files_label;
  std::vector<std::string> history;
};

void on_list_button_clicked(GtkButton *, gpointer user_data);
void on_search_button_clicked(GtkButton *, gpointer user_data);
void on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data);
void on_clear_button_clicked(GtkButton *, gpointer user_data);
void on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *);
void on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data);
void clear_search_cache();

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
