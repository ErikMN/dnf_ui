#include <catch2/catch_test_macros.hpp>

#include "test_utils.hpp"
#include "ui/common/widgets.hpp"
#include "ui/package_table/package_table_status.hpp"
#include "ui/package_table/package_table_view_internal.hpp"

#include <memory>

static PackageRow
make_status_test_row(const std::string &nevra,
                     const std::string &name,
                     const std::string &version,
                     const std::string &release,
                     const std::string &arch)
{
  PackageRow row;
  row.nevra = nevra;
  row.name = name;
  row.version = version;
  row.release = release;
  row.arch = arch;
  row.repo = "fedora";
  row.summary = "Test package";
  return row;
}

static void
require_stored_status_matches_pending_action(MainWindowUiState &widgets, PackageItem &item, const char *expected_text)
{
  PackageTableRow table_row {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };

  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      table_row.row, table_row.upgrade_target(), table_row.upgrade_generation());
  PendingAction::Type action_type;
  REQUIRE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));

  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  PackageInstallState install_state = item.upgrade_target() ? PackageInstallState::UPGRADEABLE : resolution.state;
  REQUIRE(std::string(package_table_pending_action_status_text(action_type, install_state)) == expected_text);

  package_table_fill_item_status(&widgets, item, resolution);
  REQUIRE(item.status_text == expected_text);
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == expected_text);
}

// -----------------------------------------------------------------------------
// Mark the test table as a compact List Packages view.
// -----------------------------------------------------------------------------
static void
set_compact_package_view(MainWindowUiState &widgets)
{
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_AVAILABLE;
  widgets.query_state.displayed_query.latest_only = true;
}

// -----------------------------------------------------------------------------
// Mark the test table as an all-version List Packages view.
// -----------------------------------------------------------------------------
static void
set_all_version_package_view(MainWindowUiState &widgets)
{
  widgets.query_state.displayed_query.kind = DisplayedPackageQueryKind::LIST_AVAILABLE;
  widgets.query_state.displayed_query.latest_only = false;
}

// -----------------------------------------------------------------------------
// Verify that pending installs use the same Status text in stored and visible paths.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending install status is shared")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageItem item;
  item.row = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  widgets.transaction.actions = {
    { PendingAction::INSTALL, item.row.nevra, item.row.nevra },
  };

  require_stored_status_matches_pending_action(widgets, item, "Pending Install");
}

// -----------------------------------------------------------------------------
// Verify that downgradeable rows have their own Status text.
// -----------------------------------------------------------------------------
TEST_CASE("Package table downgradeable status is shared")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item;
  item.row = older;

  MainWindowUiState widgets;
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_status(&widgets, item, resolution);

  REQUIRE(item.status_text == "Older in repository");
  REQUIRE(package_table_column_text(item, PackageColumnKind::STATUS) == "Older in repository");
}

// -----------------------------------------------------------------------------
// Verify that an installed row can show a pending upgrade queued for its install row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending upgrade status uses resolved install row")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-2.0-1.x86_64";
  installed.repo_candidate_is_newest_available = true;
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  widgets.transaction.actions = {
    { PendingAction::UPGRADE, installed.repo_candidate_nevra, "demo.x86_64" },
  };

  PackageItem item;
  item.row = installed;

  require_stored_status_matches_pending_action(widgets, item, "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// Verify that compact update rows show pending removal queued for the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact update row shows pending removal")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  widgets.transaction.actions = {
    { PendingAction::REMOVE, installed.nevra, installed.nevra },
  };

  PackageItem item;
  item.row = update;

  require_stored_status_matches_pending_action(widgets, item, "Pending Removal");
  item.row = installed;
  require_stored_status_matches_pending_action(widgets, item, "Pending Removal");
}

// -----------------------------------------------------------------------------
// Verify that compact update rows show pending reinstall queued for the installed row.
// -----------------------------------------------------------------------------
TEST_CASE("Package table compact update row shows pending reinstall")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow update = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_compact_package_view(widgets);
  widgets.transaction.actions = {
    { PendingAction::REINSTALL, installed.nevra, installed.nevra },
  };

  PackageItem item;
  item.row = update;

  require_stored_status_matches_pending_action(widgets, item, "Pending Reinstall");
  item.row = installed;
  require_stored_status_matches_pending_action(widgets, item, "Pending Reinstall");
}

// -----------------------------------------------------------------------------
// Verify that pending status does not leak between two exact installed EVRs.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending status keeps exact installed NEVRAs separate")
{
  reset_backend_globals();

  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow newer = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  MainWindowUiState widgets;
  widgets.transaction.actions = {
    { PendingAction::REMOVE, newer.nevra, newer.nevra },
  };

  PackageTableRow table_row {
    .row = older,
    .daemon_upgrade = {},
  };

  PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
      table_row.row, table_row.upgrade_target(), table_row.upgrade_generation());
  PendingAction::Type action_type;
  REQUIRE_FALSE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));
}

// -----------------------------------------------------------------------------
// Verify that removal status does not leak onto all-version repository rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending removal status does not leak to exact available versions")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-3.0-1.x86_64", "demo", "3.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow intermediate = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction.actions = {
    { PendingAction::REMOVE, installed.nevra, installed.nevra, installed.name_arch_key() },
  };

  for (const auto &row : { older, intermediate }) {
    PackageTableRow table_row {
      .row = row,
      .daemon_upgrade = {},
    };

    PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
        table_row.row, table_row.upgrade_target(), table_row.upgrade_generation());
    PendingAction::Type action_type;
    REQUIRE_FALSE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));
  }
}

// -----------------------------------------------------------------------------
// Verify that reinstall status does not leak onto all-version repository rows.
// -----------------------------------------------------------------------------
TEST_CASE("Package table pending reinstall status does not leak to exact available versions")
{
  reset_backend_globals();

  PackageRow installed = make_status_test_row("demo-3.0-1.x86_64", "demo", "3.0", "1", "x86_64");
  PackageRow older = make_status_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow intermediate = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  MainWindowUiState widgets;
  set_all_version_package_view(widgets);
  widgets.transaction.actions = {
    { PendingAction::REINSTALL, installed.nevra, installed.nevra, installed.name_arch_key() },
  };

  for (const auto &row : { older, intermediate }) {
    PackageTableRow table_row {
      .row = row,
      .daemon_upgrade = {},
    };

    PendingTransactionActionRows action_rows = pending_transaction_action_rows_for_selection(
        table_row.row, table_row.upgrade_target(), table_row.upgrade_generation());
    PendingAction::Type action_type;
    REQUIRE_FALSE(package_table_pending_action_for_resolved_row(&widgets, table_row, action_rows, action_type));
  }
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows use the same Status text for stored table data.
// -----------------------------------------------------------------------------
TEST_CASE("Package table daemon upgrade pending status is shared")
{
  reset_backend_globals();

  TransactionServiceUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "2.0";
  target.release = "1";
  target.nevra = "demo-2.0-1.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "updates";

  MainWindowUiState widgets;
  widgets.transaction.actions = {
    { PendingAction::UPGRADE, target.nevra, target.upgrade_spec() },
  };

  PackageItem item;
  item.row = make_status_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });

  require_stored_status_matches_pending_action(widgets, item, "Pending Upgrade");
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
