// src/widgets.hpp
#pragma once

#include <cstdint>
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
// Active background request using the package-list action buttons
// -----------------------------------------------------------------------------
enum class PackageListRequestKind { NONE, SEARCH, LIST_INSTALLED, LIST_AVAILABLE };

// -----------------------------------------------------------------------------
// Query controls and status widgets shared by search and package-list actions
// -----------------------------------------------------------------------------
struct PackageQueryWidgets {
  GtkEntry *entry = nullptr;
  GtkListBox *history_list = nullptr;
  GtkSpinner *spinner = nullptr;
  GtkButton *search_button = nullptr;
  GtkButton *list_button = nullptr;
  GtkButton *list_available_button = nullptr;
  GtkLabel *status_label = nullptr;
  GtkCheckButton *desc_checkbox = nullptr;
  GtkCheckButton *exact_checkbox = nullptr;
};

// -----------------------------------------------------------------------------
// Package list view, details notebook, and current selection state
// -----------------------------------------------------------------------------
struct PackageResultsWidgets {
  GtkListBox *listbox = nullptr;
  GtkScrolledWindow *list_scroller = nullptr;
  GtkPaned *inner_paned = nullptr;
  GtkLabel *details_label = nullptr;
  GtkLabel *files_label = nullptr;
  GtkLabel *deps_label = nullptr;
  GtkLabel *changelog_label = nullptr;
  GtkLabel *count_label = nullptr;
  std::vector<PackageRow> current_packages;
  std::string selected_nevra;
};

// -----------------------------------------------------------------------------
// Pending transaction widgets and marked package actions
// -----------------------------------------------------------------------------
struct PendingTransactionWidgets {
  GtkButton *install_button = nullptr;
  GtkButton *remove_button = nullptr;
  GtkButton *reinstall_button = nullptr;
  GtkButton *apply_button = nullptr;
  GtkButton *clear_pending_button = nullptr;
  GtkListBox *pending_list = nullptr;
  std::vector<PendingAction> actions;
};

// -----------------------------------------------------------------------------
// Runtime state for the active background package query flow
// -----------------------------------------------------------------------------
struct PackageQueryState {
  // Active cancellable for the current background package-list request, if any.
  GCancellable *package_list_cancellable = nullptr;
  // Next package-list request id used to distinguish overlapping background tasks.
  uint64_t next_package_list_request_id = 1;
  // Current package-list request id owned by the active package-list button UI state.
  uint64_t current_package_list_request_id = 0;
  // Identifies whether the active Stop button belongs to search, installed listing,
  // or available-package listing.
  PackageListRequestKind current_package_list_request_kind = PackageListRequestKind::NONE;
  std::vector<std::string> history;
};

// -----------------------------------------------------------------------------
// Top-level window close state shared by the main app and widget controllers
// -----------------------------------------------------------------------------
struct MainWindowState {
  // Allow the next window close after the user confirms discarding pending changes.
  bool allow_close_with_pending = false;
  // Prevent opening multiple quit-confirmation dialogs for the same pending state.
  bool pending_quit_dialog_open = false;
  // Passive bottom-bar label used for quiet startup backend status.
  GtkLabel *backend_warmup_label = nullptr;
  // Cancellable owned by the startup backend warm up task.
  GCancellable *backend_warmup_cancellable = nullptr;
};

// -----------------------------------------------------------------------------
// Shared widget state bag passed between the split controller modules
// -----------------------------------------------------------------------------
struct SearchWidgets {
  PackageQueryWidgets query;
  PackageResultsWidgets results;
  PendingTransactionWidgets transaction;
  PackageQueryState query_state;
  MainWindowState window_state;
};

void package_query_on_list_button_clicked(GtkButton *, gpointer user_data);
void package_query_on_list_available_button_clicked(GtkButton *, gpointer user_data);
void package_query_on_search_button_clicked(GtkButton *, gpointer user_data);
void package_query_on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data);
void package_query_on_clear_button_clicked(GtkButton *, gpointer user_data);
void package_query_clear_search_cache();
void pending_transaction_on_install_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_remove_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_reinstall_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_clear_pending_button_clicked(GtkButton *, gpointer user_data);
void widgets_on_refresh_button_clicked(GtkButton *, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
