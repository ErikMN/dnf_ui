// -----------------------------------------------------------------------------
// src/ui/transaction/pending_transaction_action_rows.cpp
// Pending transaction action row resolver
//
// Keeps the package ID rules for upgrade, install, remove, and reinstall in one
// place. This file must not run libdnf queries because it is used while GTK is
// updating buttons and opening context menus.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_action_rows.hpp"

#include "upgrade/daemon_upgrade_state.hpp"

namespace {

// -----------------------------------------------------------------------------
// Return the package spec used when asking dnf5daemon to upgrade one installed package.
// -----------------------------------------------------------------------------
std::string
upgrade_transaction_spec(const PackageRow &row)
{
  if (row.arch.empty()) {
    return row.name;
  }

  return row.name + "." + row.arch;
}

// -----------------------------------------------------------------------------
// Return true when one pending action is queued by the install button path.
// -----------------------------------------------------------------------------
bool
pending_action_is_install_side(PendingAction::Type type)
{
  return type == PendingAction::INSTALL || type == PendingAction::UPGRADE || type == PendingAction::DOWNGRADE;
}

// -----------------------------------------------------------------------------
// Return true when one pending action changes an installed package directly.
// -----------------------------------------------------------------------------
bool
pending_action_is_installed_side(PendingAction::Type type)
{
  return type == PendingAction::REMOVE || type == PendingAction::REINSTALL;
}

// -----------------------------------------------------------------------------
// Remove one pending action by package ID.
// -----------------------------------------------------------------------------
void
remove_pending_action_by_nevra(std::vector<PendingAction> &actions, const std::string &nevra)
{
  for (size_t i = 0; i < actions.size();) {
    if (actions[i].nevra == nevra) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Remove stale pending install, upgrade, or downgrade actions for one package name and architecture.
// -----------------------------------------------------------------------------
void
remove_pending_install_side_action_by_package_key(std::vector<PendingAction> &actions, const std::string &package_key)
{
  if (package_key.empty()) {
    return;
  }

  for (size_t i = 0; i < actions.size();) {
    if (pending_action_is_install_side(actions[i].type) && actions[i].package_key == package_key) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Remove pending remove or reinstall actions for one package name and architecture.
// -----------------------------------------------------------------------------
void
remove_pending_installed_side_action_by_package_key(std::vector<PendingAction> &actions, const std::string &package_key)
{
  if (package_key.empty()) {
    return;
  }

  for (size_t i = 0; i < actions.size();) {
    if (pending_action_is_installed_side(actions[i].type) && actions[i].package_key == package_key) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

// -----------------------------------------------------------------------------
// Remove older pending upgrades that were queued before package identity was available.
// -----------------------------------------------------------------------------
void
remove_pending_upgrade_by_transaction_spec(std::vector<PendingAction> &actions, const std::string &transaction_spec)
{
  if (transaction_spec.empty()) {
    return;
  }

  for (size_t i = 0; i < actions.size();) {
    if (actions[i].type == PendingAction::UPGRADE && actions[i].transaction_spec == transaction_spec) {
      actions.erase(actions.begin() + i);
      continue;
    }
    ++i;
  }
}

} // namespace

// -----------------------------------------------------------------------------
// Resolve package IDs for action buttons without running libdnf queries.
// -----------------------------------------------------------------------------
PendingTransactionActionRows
pending_transaction_action_rows_for_resolved_selection(const PackageRow &selected,
                                                       const TransactionServiceUpgradeTarget *upgrade_target,
                                                       uint64_t upgrade_generation,
                                                       const InstalledPackageResolution &installed_resolution)
{
  PendingTransactionActionRows rows;
  rows.state = upgrade_target ? PackageInstallState::UPGRADEABLE : installed_resolution.state;
  rows.install_is_upgrade = rows.state == PackageInstallState::UPGRADEABLE;
  rows.install_is_downgrade = rows.state == PackageInstallState::DOWNGRADEABLE;
  rows.package_key = selected.name_arch_key();
  rows.install_row = selected;
  rows.installed_row = selected;
  rows.has_installed_row = installed_resolution.exact_installed;
  rows.self_protected = installed_resolution.exact_installed && installed_resolution.self_protected;
  if (!installed_resolution.exact_installed && installed_resolution.has_installed_row) {
    rows.has_installed_row = true;
    rows.installed_row = installed_resolution.installed_row;
    rows.self_protected = installed_resolution.self_protected;
  }

  // Upgrade actions need the available package ID, not always the visible row ID.
  if (rows.install_is_upgrade) {
    if (upgrade_target) {
      rows.has_install_row = DaemonUpgradeState::instance().is_current_target(*upgrade_target, upgrade_generation);
      rows.install_row.nevra = upgrade_target->nevra.empty() ? selected.nevra : upgrade_target->nevra;
      rows.upgrade_spec = upgrade_target->upgrade_spec();
      rows.has_installed_row = installed_resolution.has_installed_row;
      rows.self_protected = rows.has_installed_row && installed_resolution.self_protected;
      rows.can_try_reinstall = rows.has_installed_row;
      return rows;
    }

    if (installed_resolution.exact_installed) {
      // Installed-list rows store the matching available upgrade package ID when the backend annotates them.
      rows.has_install_row = !selected.repo_candidate_nevra.empty() && selected.repo_candidate_is_newest_available;
      rows.install_row.nevra = selected.repo_candidate_nevra;
    } else {
      // Available update rows are already repository package rows.
      // Only the newest available row can use the upgrade action.
      rows.has_install_row = selected.is_newest_available;
      rows.has_installed_row = installed_resolution.has_installed_row;
      rows.self_protected = rows.has_installed_row && installed_resolution.self_protected;
    }
    rows.upgrade_spec = upgrade_transaction_spec(rows.has_installed_row ? rows.installed_row : selected);
    rows.can_try_reinstall = rows.has_installed_row;
    return rows;
  }

  // Older available rows can be marked as a downgrade to that exact package ID.
  if (rows.install_is_downgrade) {
    rows.has_install_row = true;
    rows.can_try_reinstall = rows.has_installed_row;
    return rows;
  }

  // Plain available packages can only be installed.
  if (rows.state == PackageInstallState::AVAILABLE) {
    rows.has_install_row = true;
  }

  // Reinstall needs an installed package that is still available from repositories.
  rows.can_try_reinstall = rows.has_installed_row && rows.state != PackageInstallState::LOCAL_ONLY &&
      rows.state != PackageInstallState::INSTALLED_NEWER_THAN_REPO;

  return rows;
}

PendingTransactionActionRows
pending_transaction_action_rows_for_selection(const PackageRow &selected,
                                              const TransactionServiceUpgradeTarget *upgrade_target,
                                              uint64_t upgrade_generation)
{
  InstalledPackageResolution installed_resolution = dnf_backend_resolve_installed_package(selected);
  return pending_transaction_action_rows_for_resolved_selection(
      selected, upgrade_target, upgrade_generation, installed_resolution);
}

// -----------------------------------------------------------------------------
// Return true when the selected row may use its install, upgrade, or downgrade action.
// -----------------------------------------------------------------------------
bool
pending_transaction_selection_allows_install_action(const PackageRow &selected,
                                                    const PendingTransactionActionRows &rows,
                                                    bool projects_upgrade_actions)
{
  if (!rows.has_install_row) {
    return false;
  }

  if (selected.nevra == rows.install_row.nevra) {
    return true;
  }

  return projects_upgrade_actions && rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// Return true when the selected row may modify its installed package.
// -----------------------------------------------------------------------------
bool
pending_transaction_selection_allows_installed_action(const PackageRow &selected,
                                                      const PendingTransactionActionRows &rows,
                                                      bool compact_view)
{
  if (!rows.has_installed_row) {
    return false;
  }

  if (selected.nevra == rows.installed_row.nevra) {
    return true;
  }

  return compact_view && rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// Add or replace one pending install, upgrade, or downgrade action from resolved rows.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_install_side_action(std::vector<PendingAction> &actions,
                                             const PendingTransactionActionRows &action_rows)
{
  if (!action_rows.has_install_row) {
    return false;
  }

  PendingAction::Type action_type = PendingAction::INSTALL;
  std::string transaction_spec = action_rows.install_row.nevra;
  if (action_rows.install_is_upgrade) {
    action_type = PendingAction::UPGRADE;
    transaction_spec = action_rows.upgrade_spec;
  } else if (action_rows.install_is_downgrade) {
    action_type = PendingAction::DOWNGRADE;
  }

  if (transaction_spec.empty()) {
    return false;
  }

  remove_pending_install_side_action_by_package_key(actions, action_rows.package_key);
  remove_pending_installed_side_action_by_package_key(actions, action_rows.package_key);
  if (action_type == PendingAction::UPGRADE) {
    remove_pending_upgrade_by_transaction_spec(actions, transaction_spec);
  }
  remove_pending_action_by_nevra(actions, action_rows.install_row.nevra);
  if (action_rows.has_installed_row) {
    remove_pending_action_by_nevra(actions, action_rows.installed_row.nevra);
  }

  actions.push_back({ action_type, action_rows.install_row.nevra, transaction_spec, action_rows.package_key });
  return true;
}

// -----------------------------------------------------------------------------
// Add or replace one pending remove or reinstall action from resolved rows.
// -----------------------------------------------------------------------------
static bool
pending_transaction_mark_installed_side_action(std::vector<PendingAction> &actions,
                                               const PendingTransactionActionRows &action_rows,
                                               PendingAction::Type action_type)
{
  if (!action_rows.has_installed_row) {
    return false;
  }

  remove_pending_install_side_action_by_package_key(actions, action_rows.package_key);
  remove_pending_action_by_nevra(actions, action_rows.installed_row.nevra);

  actions.push_back(
      { action_type, action_rows.installed_row.nevra, action_rows.installed_row.nevra, action_rows.package_key });
  return true;
}

// -----------------------------------------------------------------------------
// Add or replace one pending remove action from resolved rows.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_remove_action(std::vector<PendingAction> &actions,
                                       const PendingTransactionActionRows &action_rows)
{
  return pending_transaction_mark_installed_side_action(actions, action_rows, PendingAction::REMOVE);
}

// -----------------------------------------------------------------------------
// Add or replace one pending reinstall action from resolved rows.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_reinstall_action(std::vector<PendingAction> &actions,
                                          const PendingTransactionActionRows &action_rows)
{
  return pending_transaction_mark_installed_side_action(actions, action_rows, PendingAction::REINSTALL);
}

// -----------------------------------------------------------------------------
// Add or replace one pending upgrade action from a package table row.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_upgrade_action_for_row(std::vector<PendingAction> &actions,
                                                const PackageRow &row,
                                                const TransactionServiceUpgradeTarget *upgrade_target,
                                                uint64_t upgrade_generation,
                                                bool projects_upgrade_actions)
{
  PendingTransactionActionRows action_rows =
      pending_transaction_action_rows_for_selection(row, upgrade_target, upgrade_generation);
  if (!action_rows.install_is_upgrade ||
      !pending_transaction_selection_allows_install_action(row, action_rows, projects_upgrade_actions)) {
    return false;
  }

  if (!pending_transaction_mark_install_side_action(actions, action_rows)) {
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Add or replace one pending upgrade unless this package identity was already handled.
// -----------------------------------------------------------------------------
bool
pending_transaction_mark_unique_upgrade_action(std::vector<PendingAction> &actions,
                                               std::set<std::string> &marked_package_keys,
                                               const PackageRow &selected,
                                               const PendingTransactionActionRows &action_rows,
                                               bool projects_upgrade_actions)
{
  if (!action_rows.install_is_upgrade || action_rows.package_key.empty()) {
    return false;
  }

  if (!pending_transaction_selection_allows_install_action(selected, action_rows, projects_upgrade_actions)) {
    return false;
  }

  if (marked_package_keys.count(action_rows.package_key) > 0) {
    return false;
  }

  if (!pending_transaction_mark_install_side_action(actions, action_rows)) {
    return false;
  }

  marked_package_keys.insert(action_rows.package_key);
  return true;
}

// -----------------------------------------------------------------------------
// Return true when self-protection should block the install button path.
// A normal upgrade is allowed because dnf5daemon still resolves the final preview.
// -----------------------------------------------------------------------------
bool
pending_transaction_install_action_blocked_by_self_protection(const PendingTransactionActionRows &rows,
                                                              bool self_protected)
{
  return self_protected && !rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// Return true when activating this visible row should use the remove action.
// -----------------------------------------------------------------------------
bool
pending_transaction_activation_should_remove(const PackageRow &selected, const PendingTransactionActionRows &rows)
{
  return rows.has_installed_row && selected.nevra == rows.installed_row.nevra && !rows.install_is_upgrade;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
