// src/widgets.hpp
#pragma once

#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "dnf_backend.hpp"

// -----------------------------------------------------------------------------
// Pending actions for mark --> review --> apply workflow
// -----------------------------------------------------------------------------
struct PendingAction {
  enum Type { INSTALL, REMOVE, REINSTALL } type;
  std::string nevra;
};

// -----------------------------------------------------------------------------
// Struct for holding UI elements and signal callbacks
// -----------------------------------------------------------------------------
struct SearchWidgets {
  GtkEntry *entry;
  GtkListBox *listbox;
  GtkScrolledWindow *list_scroller;
  GtkPaned *inner_paned;
  GtkListBox *history_list;
  GtkSpinner *spinner;
  GtkButton *search_button;
  GtkButton *install_button;
  GtkButton *remove_button;
  GtkButton *reinstall_button;
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
  // Active cancellable for the current background search, if any.
  GCancellable *search_cancellable;
  // Next search request id used to distinguish overlapping search tasks.
  uint64_t next_search_request_id;
  // Current search request id owned by the search UI state.
  uint64_t current_search_request_id;
  // Allow the next window close after the user confirms discarding pending changes.
  bool allow_close_with_pending;
  // Prevent opening multiple quit-confirmation dialogs for the same pending state.
  bool pending_quit_dialog_open;
  std::vector<std::string> history;
  std::vector<PackageRow> current_packages;
  std::vector<PendingAction> pending;
  std::string selected_nevra;
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
void on_reinstall_button_clicked(GtkButton *, gpointer user_data);
void on_apply_button_clicked(GtkButton *, gpointer user_data);
void on_clear_pending_button_clicked(GtkButton *, gpointer user_data);
void clear_search_cache();

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
