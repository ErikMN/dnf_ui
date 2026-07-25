#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"
#include "ui/common/widgets.hpp"
#include "ui/package_table/package_table_view_internal.hpp"

#include <memory>

static PackageRow
make_table_test_row(const std::string &nevra,
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

static PackageItem
make_table_test_item(const PackageRow &row)
{
  PackageItem item {};
  item.row = row;
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution);
  return item;
}

static int
compare_table_test_rows(MainWindowUiState &widgets,
                        const PackageRow &left,
                        const PackageRow &right,
                        PackageColumnKind kind)
{
  GObject *left_object = make_package_object(&widgets, left);
  GObject *right_object = make_package_object(&widgets, right);
  int result =
      package_table_column_sorter_compare(left_object, right_object, GINT_TO_POINTER(static_cast<int>(kind) + 1));
  g_object_unref(left_object);
  g_object_unref(right_object);
  return result;
}

// -----------------------------------------------------------------------------
// Verify that the package table Version column matches the Info tab Version
// field and does not include the package release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version column shows only package version")
{
  reset_backend_globals();

  PackageItem item =
      make_table_test_item(make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64"));

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.3");
}

// -----------------------------------------------------------------------------
// Verify that an available update row still shows the installed package version.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version column uses installed version for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.3");
}

// -----------------------------------------------------------------------------
// Verify that an available update row shows the version that would be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update column uses candidate version for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.4");
}

// -----------------------------------------------------------------------------
// Verify that a non upgradable row leaves the Update column empty.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update column is empty for normal rows")
{
  reset_backend_globals();

  PackageItem item =
      make_table_test_item(make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64"));

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION).empty());
}

// -----------------------------------------------------------------------------
// Verify that the Release column shows the package release.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Release column shows package release")
{
  reset_backend_globals();

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that exact installed rows display themselves when another installed EVR exists.
// -----------------------------------------------------------------------------
TEST_CASE("Package table exact installed row displays its own version")
{
  reset_backend_globals();

  PackageRow older = make_table_test_row("demo-1.0-1.fc44.x86_64", "demo", "1.0", "1.fc44", "x86_64");
  older.repo = "@System";
  PackageRow newer = make_table_test_row("demo-2.0-1.fc44.x86_64", "demo", "2.0", "1.fc44", "x86_64");
  newer.repo = "@System";

  dnf_backend_testonly_replace_installed_snapshot_rows({ older, newer });

  PackageItem item = make_table_test_item(older);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == older.version);
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == older.release);
  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == older.repo);
}

// -----------------------------------------------------------------------------
// Verify that installed-list rows keep installed and update releases separate.
// -----------------------------------------------------------------------------
TEST_CASE("Package table release columns handle installed rows with update candidates")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-1.2.4-1.fc44.x86_64";
  installed.repo_candidate_version = "1.2.4";
  installed.repo_candidate_release = "1.fc44";
  installed.repo_candidate_repo = "updates";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(installed);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.3");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.4");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "4.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that installed-list rows show the repo that provides the update.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Repo column uses candidate repo for installed update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo = "@System";
  installed.repo_candidate_relation = PackageRepoCandidateRelation::NEWER;
  installed.repo_candidate_nevra = "demo-1.2.4-1.fc44.x86_64";
  installed.repo_candidate_version = "1.2.4";
  installed.repo_candidate_release = "1.fc44";
  installed.repo_candidate_repo = "updates";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(installed);

  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == "updates");
}

// -----------------------------------------------------------------------------
// Verify that an available update row can show the release that would be installed.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Release column uses candidate release for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "1.fc44");
}

// -----------------------------------------------------------------------------
// Verify that an available update row shows the repository that provides the update.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Repo column uses candidate repo for update rows")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.3-4.fc44.x86_64", "demo", "1.2.3", "4.fc44", "x86_64");
  installed.repo = "@System";

  PackageRow update = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  update.repo = "updates";

  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item = make_table_test_item(update);

  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == "updates");
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows use daemon target values for update columns.
// -----------------------------------------------------------------------------
TEST_CASE("Package table update columns use daemon upgrade target")
{
  reset_backend_globals();

  PackageItem item {};
  item.row = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  item.row.repo = "metadata-repo";

  TransactionServiceUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "1.2.5";
  target.release = "2.fc44";
  target.nevra = "demo-1.2.5-2.fc44.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "daemon-repo";

  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.5");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE).empty());
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "2.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::REPO) == "daemon-repo");
}

// -----------------------------------------------------------------------------
// Verify that daemon upgrade rows keep installed and target versions separate.
// -----------------------------------------------------------------------------
TEST_CASE("Package table daemon upgrade rows use installed version when available")
{
  reset_backend_globals();

  PackageRow installed = make_table_test_row("demo-1.2.4-1.fc44.x86_64", "demo", "1.2.4", "1.fc44", "x86_64");
  installed.repo = "@System";
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageItem item {};
  item.row = make_table_test_row("demo-1.2.5-2.fc44.x86_64", "demo", "1.2.5", "2.fc44", "x86_64");

  TransactionServiceUpgradeTarget target;
  target.name = "demo";
  target.arch = "x86_64";
  target.version = "1.2.5";
  target.release = "2.fc44";
  target.nevra = "demo-1.2.5-2.fc44.x86_64";
  target.full_nevra = target.nevra;
  target.repo_id = "daemon-repo";

  item.daemon_upgrade = std::make_shared<DaemonUpgradeRowContext>(DaemonUpgradeRowContext {
      .target = target,
  });
  InstalledPackageResolution resolution = dnf_backend_resolve_installed_package(item.row);
  package_table_fill_item_display_values(item, resolution);

  REQUIRE(package_table_column_text(item, PackageColumnKind::VERSION) == "1.2.4");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_VERSION) == "1.2.5");
  REQUIRE(package_table_column_text(item, PackageColumnKind::RELEASE) == "1.fc44");
  REQUIRE(package_table_column_text(item, PackageColumnKind::UPDATE_RELEASE) == "2.fc44");
}

// -----------------------------------------------------------------------------
// Verify that Version sorting uses values stored on the package items.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Version sorter uses stored item values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  PackageRow right = make_table_test_row("right-2.0-1.x86_64", "right", "2.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Update Version sorting uses values stored on the package items.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Update Version sorter uses stored item values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow installed_left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  PackageRow installed_right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed_left, installed_right });

  PackageRow left = make_table_test_row("left-2.0-1.x86_64", "left", "2.0", "1", "x86_64");
  PackageRow right = make_table_test_row("right-3.0-1.x86_64", "right", "3.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::UPDATE_VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that Repository sorting uses values stored on the package items.
// -----------------------------------------------------------------------------
TEST_CASE("Package table Repo sorter uses stored item values")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("left-1.0-1.x86_64", "left", "1.0", "1", "x86_64");
  left.repo = "fedora";
  PackageRow right = make_table_test_row("right-1.0-1.x86_64", "right", "1.0", "1", "x86_64");
  right.repo = "updates";

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::REPO) < 0);
}

// -----------------------------------------------------------------------------
// Verify that equal column values fall back to package name and then NEVRA.
// -----------------------------------------------------------------------------
TEST_CASE("Package table sorter falls back to name and NEVRA")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow left = make_table_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  PackageRow right = make_table_test_row("other-1.0-1.x86_64", "other", "1.0", "1", "x86_64");

  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);

  right.name = left.name;
  right.nevra = "demo-1.0-2.x86_64";
  REQUIRE(compare_table_test_rows(widgets, left, right, PackageColumnKind::VERSION) < 0);
}

// -----------------------------------------------------------------------------
// Verify that replacing installed state does not change existing item comparisons.
// -----------------------------------------------------------------------------
TEST_CASE("Package table sorter keeps existing item values after installed snapshot changes")
{
  reset_backend_globals();

  MainWindowUiState widgets;
  PackageRow installed = make_table_test_row("demo-1.0-1.x86_64", "demo", "1.0", "1", "x86_64");
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  PackageRow update = make_table_test_row("demo-2.0-1.x86_64", "demo", "2.0", "1", "x86_64");
  PackageRow comparison = make_table_test_row("other-1.2-1.x86_64", "other", "1.2", "1", "x86_64");

  GObject *update_object = make_package_object(&widgets, update);
  GObject *comparison_object = make_package_object(&widgets, comparison);

  installed.version = "1.3";
  dnf_backend_testonly_replace_installed_snapshot_rows({ installed });

  int result = package_table_column_sorter_compare(
      update_object, comparison_object, GINT_TO_POINTER(static_cast<int>(PackageColumnKind::VERSION) + 1));

  g_object_unref(update_object);
  g_object_unref(comparison_object);

  REQUIRE(result < 0);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
