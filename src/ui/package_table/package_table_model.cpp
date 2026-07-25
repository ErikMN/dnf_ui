// -----------------------------------------------------------------------------
// package_table_model.cpp
// Package row storage for the GTK package table.
// Wraps PackageRow values in GObjects so the ColumnView can sort and select rows without owning backend data directly.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_view_internal.hpp"

#include "ui/package_table/package_table_status.hpp"
#include "ui/common/widgets.hpp"

#include <utility>

// -----------------------------------------------------------------------------
// Return the private key used to store package rows on GTK objects.
// -----------------------------------------------------------------------------
static GQuark
package_row_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("package-table-row");
  }

  return q;
}

// -----------------------------------------------------------------------------
// Snapshot the visible status text and its sort order for one package row.
// -----------------------------------------------------------------------------
void
package_table_fill_item_status(MainWindowUiState *widgets, PackageItem &item)
{
  // Keep Status sorting tied to the stable package state so marking a pending
  // action does not move the row away from the user in the current view.
  PackageInstallState install_state =
      item.upgrade_target() ? PackageInstallState::UPGRADEABLE : dnf_backend_get_package_install_state(item.row);
  item.status_rank = dnf_backend_get_install_state_sort_rank(install_state);

  PackageTableRow table_row {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };
  PendingAction::Type action_type;
  if (package_table_pending_action_for_row(widgets, table_row, action_type)) {
    item.status_text = package_table_pending_action_status_text(action_type, install_state);
    return;
  }

  item.status_text = package_table_status_text(install_state);
}

// -----------------------------------------------------------------------------
// Snapshot display values that depend on installed-package state.
// -----------------------------------------------------------------------------
void
package_table_fill_item_display_values(PackageItem &item)
{
  PackageTableDisplayValues values {
    .version = item.row.version,
    .update_version = {},
    .release = item.row.release,
    .update_release = {},
    .repo = item.row.repo,
  };

  if (const TransactionServiceUpgradeTarget *upgrade_target = item.upgrade_target()) {
    PackageRow installed_row;
    if (dnf_backend_get_installed_package_row_by_name_arch(item.row, installed_row)) {
      values.version = installed_row.version;
      values.release = installed_row.release;
    } else {
      values.version.clear();
      values.release.clear();
    }

    values.update_version = upgrade_target->version;
    values.update_release = upgrade_target->release;
    values.repo = upgrade_target->repo_id;
    item.display_values = std::make_shared<PackageTableDisplayValues>(std::move(values));
    return;
  }

  PackageRow installed_row;
  if (dnf_backend_get_installed_package_row_by_name_arch(item.row, installed_row)) {
    if (installed_row.nevra != item.row.nevra) {
      // The table column is named Version, so keep it aligned with the Info tab Version field.
      values.version = installed_row.version;
      values.release = installed_row.release;
    }
    values.repo = installed_row.repo;
  }

  if (dnf_backend_get_package_install_state(item.row) == PackageInstallState::UPGRADEABLE) {
    values.update_version =
        item.row.repo_candidate_version.empty() ? item.row.version : item.row.repo_candidate_version;
    values.update_release =
        item.row.repo_candidate_release.empty() ? item.row.release : item.row.repo_candidate_release;

    if (!item.row.repo_candidate_repo.empty()) {
      values.repo = item.row.repo_candidate_repo;
    } else {
      values.repo = item.row.repo;
    }
  }

  const bool display_values_differ = values.version != item.row.version || !values.update_version.empty() ||
      values.release != item.row.release || !values.update_release.empty() || values.repo != item.row.repo;
  if (display_values_differ) {
    item.display_values = std::make_shared<PackageTableDisplayValues>(std::move(values));
  } else {
    item.display_values.reset();
  }
}

// -----------------------------------------------------------------------------
// Wrap one package row in a GObject so GTK list models can sort and select it.
// -----------------------------------------------------------------------------
GObject *
make_package_object(MainWindowUiState *widgets, const PackageRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem;
  item->row = row;
  package_table_fill_item_display_values(*item);
  package_table_fill_item_status(widgets, *item);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });

  return obj;
}

GObject *
make_package_object(MainWindowUiState *widgets, const PackageTableRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem;
  item->row = row.row;
  item->daemon_upgrade = row.daemon_upgrade;
  package_table_fill_item_display_values(*item);
  package_table_fill_item_status(widgets, *item);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });

  return obj;
}

// -----------------------------------------------------------------------------
// Read the sortable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
const PackageItem *
package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<const PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// -----------------------------------------------------------------------------
// Read the mutable package wrapper stored on a GTK list item.
// -----------------------------------------------------------------------------
PackageItem *
mutable_package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// -----------------------------------------------------------------------------
// Map a package wrapper back to the package row used elsewhere in the UI.
// -----------------------------------------------------------------------------
const PackageRow *
package_row_from_object(GObject *obj)
{
  const PackageItem *item = package_item_from_object(obj);
  if (!item) {
    return nullptr;
  }

  return &item->row;
}

// -----------------------------------------------------------------------------
// Convert the stored package wrapper back to the public table row value.
// -----------------------------------------------------------------------------
PackageTableRow
package_table_row_from_item(const PackageItem &item)
{
  return {
    .row = item.row,
    .daemon_upgrade = item.daemon_upgrade,
  };
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
