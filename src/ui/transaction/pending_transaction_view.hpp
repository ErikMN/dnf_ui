// -----------------------------------------------------------------------------
// pending_transaction_view.hpp
// Pending transaction view helpers
//
// Owns the small UI helpers for the Pending Actions tab and transaction action labels.
// -----------------------------------------------------------------------------
#pragma once

#include "ui/transaction/pending_transaction_state.hpp"

#include <string>

struct MainWindowUiState;

// -----------------------------------------------------------------------------
// Rebuild the Pending Actions tab from the current pending actions.
// -----------------------------------------------------------------------------
void pending_transaction_refresh_pending_tab(MainWindowUiState *widgets);
// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
bool pending_transaction_remove_action(MainWindowUiState *widgets, const std::string &nevra);
// -----------------------------------------------------------------------------
// Return the pending action type for one package ID.
// -----------------------------------------------------------------------------
bool pending_transaction_get_action_type(MainWindowUiState *widgets,
                                         const std::string &nevra,
                                         PendingAction::Type &out_type);
// -----------------------------------------------------------------------------
// Update package action button labels based on pending actions.
// -----------------------------------------------------------------------------
void pending_transaction_update_action_button_labels_for_selection(MainWindowUiState *widgets,
                                                                   const std::string &install_nevra,
                                                                   const std::string &remove_nevra,
                                                                   const std::string &reinstall_nevra,
                                                                   bool install_is_upgrade,
                                                                   bool install_is_downgrade = false);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
