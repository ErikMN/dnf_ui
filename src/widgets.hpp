// src/widgets.hpp
#pragma once

#include <string>
#include <vector>
#include <mutex>

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Pending actions for mark --> review --> apply workflow
// -----------------------------------------------------------------------------
struct PendingAction {
  enum Type { INSTALL, REMOVE } type;
  std::string nevra;
};

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
  GtkButton *install_button;
  GtkButton *remove_button;
  GtkButton *apply_button;
  GtkButton *clear_pending_button;
  GtkLabel *status_label;
  GtkLabel *details_label;
  GtkLabel *count_label;
  GtkCheckButton *desc_checkbox;
  GtkCheckButton *exact_checkbox;
  GtkLabel *files_label;
  GtkLabel *deps_label;
  GtkLabel *changelog_label;
  std::vector<std::string> history;
  std::vector<PendingAction> pending;
  GtkListBox *pending_list;
};

void on_list_button_clicked(GtkButton *, gpointer user_data);
void on_search_button_clicked(GtkButton *, gpointer user_data);
void on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data);
void on_clear_button_clicked(GtkButton *, gpointer user_data);
void on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *);
void on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data);
void on_install_button_clicked(GtkButton *, gpointer user_data);
void on_remove_button_clicked(GtkButton *, gpointer user_data);
void on_apply_button_clicked(GtkButton *, gpointer user_data);
void on_clear_pending_button_clicked(GtkButton *, gpointer user_data);
void clear_search_cache();

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
