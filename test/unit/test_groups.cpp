#include <catch2/catch_test_macros.hpp>

#include "dnf_backend/dnf_backend.hpp"
#include "test_utils.hpp"

#include <algorithm>

#include <gio/gio.h>

TEST_CASE("Package group query returns usable metadata when groups are available")
{
  reset_backend_globals();

  auto groups = dnf_backend_get_package_groups_interruptible(nullptr);
  if (groups.empty()) {
    SKIP("No DNF package groups available in current repository metadata.");
  }

  for (const auto &group : groups) {
    REQUIRE_FALSE(group.id.empty());
    REQUIRE_FALSE(group.name.empty());
  }

  std::vector<PackageRow> rows;
  for (const auto &group : groups) {
    if (group.package_count == 0) {
      continue;
    }

    rows = dnf_backend_get_package_group_package_rows_interruptible(group.id, nullptr);
    if (!rows.empty()) {
      break;
    }
  }

  if (rows.empty()) {
    SKIP("No DNF package group with resolvable package rows available in current repository metadata.");
  }

  REQUIRE_FALSE(rows.empty());
  for (const auto &row : rows) {
    REQUIRE_FALSE(row.name.empty());
    REQUIRE_FALSE(row.nevra.empty());
    REQUIRE_FALSE(row.arch.empty());
  }
}

TEST_CASE("Cancelled package group queries return no results")
{
  GCancellable *cancellable = g_cancellable_new();
  g_cancellable_cancel(cancellable);

  auto groups = dnf_backend_get_package_groups_interruptible(cancellable);
  auto rows = dnf_backend_get_package_group_package_rows_interruptible("development-tools", cancellable);

  g_object_unref(cancellable);

  REQUIRE(groups.empty());
  REQUIRE(rows.empty());
}
