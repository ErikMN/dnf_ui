// -----------------------------------------------------------------------------
// pending_transaction_request.cpp
// Pending transaction request helpers
// Keeps transaction request construction and validation separate from GTK pending action callbacks.
// -----------------------------------------------------------------------------
#include "ui/transaction/pending_transaction_request.hpp"

#include "dnf_backend/base_manager.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

#include <exception>
#include <set>

namespace {

struct PendingRequestBaseDropGuard {
  ~PendingRequestBaseDropGuard()
  {
    BaseManager::instance().drop_cached_base();
  }
};

// -----------------------------------------------------------------------------
// Return true when one pending action is queued by the install button path.
// -----------------------------------------------------------------------------
static bool
pending_action_is_install_side(PendingAction::Type type)
{
  return type == PendingAction::INSTALL || type == PendingAction::UPGRADE || type == PendingAction::DOWNGRADE;
}

// -----------------------------------------------------------------------------
// Return true when one pending action changes an installed package directly.
// -----------------------------------------------------------------------------
static bool
pending_action_is_installed_side(PendingAction::Type type)
{
  return type == PendingAction::REMOVE || type == PendingAction::REINSTALL;
}

}

// -----------------------------------------------------------------------------
// Split the pending queue into transaction specs by requested action.
// -----------------------------------------------------------------------------
static bool
build_pending_transaction_specs(const std::vector<PendingAction> &actions,
                                std::vector<std::string> &install,
                                std::vector<std::string> &upgrade,
                                std::vector<std::string> &downgrade,
                                std::vector<std::string> &remove,
                                std::vector<std::string> &reinstall,
                                std::string &error_out)
{
  install.clear();
  upgrade.clear();
  downgrade.clear();
  remove.clear();
  reinstall.clear();
  error_out.clear();

  install.reserve(actions.size());
  upgrade.reserve(actions.size());
  downgrade.reserve(actions.size());
  remove.reserve(actions.size());
  reinstall.reserve(actions.size());

  std::set<std::string> install_side_keys;
  std::set<std::string> installonly_install_keys;
  std::set<std::string> installed_side_keys;

  for (const auto &action : actions) {
    if (!pending_action_is_install_side(action.type) && !pending_action_is_installed_side(action.type)) {
      install.clear();
      upgrade.clear();
      downgrade.clear();
      remove.clear();
      reinstall.clear();
      error_out = _("Unknown pending package action.");
      return false;
    }

    if (action.transaction_spec.empty()) {
      install.clear();
      upgrade.clear();
      downgrade.clear();
      remove.clear();
      reinstall.clear();
      error_out = _("Pending package action is missing its transaction spec.");
      return false;
    }

    if (action.package_key.empty()) {
      install.clear();
      upgrade.clear();
      downgrade.clear();
      remove.clear();
      reinstall.clear();
      error_out = _("Pending package action is missing its package identity.");
      return false;
    }

    if (pending_action_is_install_side(action.type)) {
      const bool installonly_install =
          action.type == PendingAction::INSTALL && action.installonly && action.nevra == action.transaction_spec;
      if (installed_side_keys.count(action.package_key) > 0 ||
          (installonly_install && install_side_keys.count(action.package_key) > 0) ||
          (!installonly_install &&
           (installonly_install_keys.count(action.package_key) > 0 ||
            !install_side_keys.insert(action.package_key).second))) {
        install.clear();
        upgrade.clear();
        downgrade.clear();
        remove.clear();
        reinstall.clear();
        error_out = _("Pending package actions contain conflicting package identities.");
        return false;
      }
      if (installonly_install) {
        installonly_install_keys.insert(action.package_key);
      }
    } else if (pending_action_is_installed_side(action.type)) {
      if (install_side_keys.count(action.package_key) > 0 || installonly_install_keys.count(action.package_key) > 0) {
        install.clear();
        upgrade.clear();
        downgrade.clear();
        remove.clear();
        reinstall.clear();
        error_out = _("Pending package actions contain conflicting package identities.");
        return false;
      }
      installed_side_keys.insert(action.package_key);
    }

    switch (action.type) {
    case PendingAction::INSTALL:
      install.push_back(action.transaction_spec);
      break;
    case PendingAction::UPGRADE:
      upgrade.push_back(action.transaction_spec);
      break;
    case PendingAction::DOWNGRADE:
      downgrade.push_back(action.transaction_spec);
      break;
    case PendingAction::REMOVE:
      remove.push_back(action.transaction_spec);
      break;
    case PendingAction::REINSTALL:
      reinstall.push_back(action.transaction_spec);
      break;
    default:
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// Convert marked UI actions into a transaction request.
// -----------------------------------------------------------------------------
bool
pending_transaction_build_request(const std::vector<PendingAction> &actions,
                                  TransactionRequest &request,
                                  std::string &error_out)
{
  request.upgrade_all = false;
  return build_pending_transaction_specs(
      actions, request.install, request.upgrade, request.downgrade, request.remove, request.reinstall, error_out);
}

// -----------------------------------------------------------------------------
// Reject direct downgrade, remove, or reinstall requests for the package owning the running GUI.
// Selected upgrades are allowed here and checked again after dnf5daemon resolves the preview.
// -----------------------------------------------------------------------------
bool
pending_transaction_validate_request(const TransactionRequest &request, std::string &error_out)
{
  PendingRequestBaseDropGuard base_drop_guard;

  try {
    for (const auto &spec : request.downgrade) {
      if (dnf_backend_is_self_protected_transaction_spec(spec)) {
        error_out = _("DNF UI cannot downgrade the package that owns the running application while it is running.");
        return false;
      }
    }

    for (const auto &spec : request.remove) {
      // Re-check remove specs so stale UI state or bypassed button sensitivity cannot remove the running app.
      if (dnf_backend_is_self_protected_transaction_spec(spec)) {
        error_out = _("DNF UI cannot remove the package that owns the running application. Close DNF UI and remove it "
                      "from another tool.");
        return false;
      }
    }

    for (const auto &spec : request.reinstall) {
      // Re-check reinstall specs so stale UI state or bypassed button sensitivity
      // cannot reinstall the running app.
      if (dnf_backend_is_self_protected_transaction_spec(spec)) {
        error_out = _("DNF UI cannot reinstall the package that owns the running application while it is running.");
        return false;
      }
    }
  } catch (const std::exception &e) {
    error_out = e.what();
    return false;
  } catch (...) {
    error_out = _("Could not verify whether the transaction modifies DNF UI itself.");
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
