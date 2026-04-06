// -----------------------------------------------------------------------------
// src/package_table_view.cpp
// Synaptic-style package table view
// Builds the GTK4 ColumnView, maintains sortable package-row wrappers, and
// keeps package selection wired into the details notebook controller.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"

#include "package_info_controller.hpp"
#include "widgets.hpp"

#include <unistd.h>

#include <string>

// -----------------------------------------------------------------------------
// Column model helpers
// -----------------------------------------------------------------------------
enum class PackageColumnKind {
  STATUS,
  PACKAGE,
  VERSION,
  ARCH,
  REPO,
  SUMMARY,
};

static GQuark
package_row_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("package-table-row");
  }

  return q;
}

// Package row wrapper used by the sortable GTK model.
struct PackageItem {
  PackageRow row;
  std::string status_text;
  int status_rank;
};

// -----------------------------------------------------------------------------
// Package status helpers
// -----------------------------------------------------------------------------
static const char *
install_state_text(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::INSTALLED:
    return "Installed";
  case PackageInstallState::UPGRADEABLE:
    return "Update available";
  case PackageInstallState::AVAILABLE:
  default:
    return "Available";
  }
}

static int
install_state_rank(PackageInstallState state)
{
  switch (state) {
  case PackageInstallState::AVAILABLE:
    return 0;
  case PackageInstallState::INSTALLED:
    return 1;
  case PackageInstallState::UPGRADEABLE:
    return 2;
  default:
    return 0;
  }
}

// Snapshot the visible status text and its sort order for one package row.
static void
fill_package_item_status(SearchWidgets *widgets, PackageItem &item)
{
  // Keep Status sorting tied to the stable package state so marking a pending
  // action does not move the row away from the user in the current view.
  PackageInstallState install_state = dnf_backend_get_package_install_state(item.row);
  item.status_rank = install_state_rank(install_state);

  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == item.row.nevra) {
      switch (a.type) {
      case PendingAction::INSTALL:
        item.status_text = "Pending Install";
        break;
      case PendingAction::REINSTALL:
        item.status_text = "Pending Reinstall";
        break;
      case PendingAction::REMOVE:
        item.status_text = "Pending Removal";
        break;
      }
      return;
    }
  }

  item.status_text = install_state_text(install_state);
}

static const char *
pending_css_class(SearchWidgets *widgets, const std::string &nevra)
{
  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == nevra) {
      switch (a.type) {
      case PendingAction::INSTALL:
        return "package-status-pending-install";
      case PendingAction::REINSTALL:
        return "package-status-pending-reinstall";
      case PendingAction::REMOVE:
        return "package-status-pending-remove";
      }
    }
  }
  return nullptr;
}

static void
clear_status_css(GtkWidget *label)
{
  gtk_widget_remove_css_class(label, "package-status-available");
  gtk_widget_remove_css_class(label, "package-status-installed");
  gtk_widget_remove_css_class(label, "package-status-upgradeable");
  gtk_widget_remove_css_class(label, "package-status-pending-install");
  gtk_widget_remove_css_class(label, "package-status-pending-reinstall");
  gtk_widget_remove_css_class(label, "package-status-pending-remove");
}

// -----------------------------------------------------------------------------
// Package row object helpers
// -----------------------------------------------------------------------------
// Wrap one package row in a GObject so GTK list models can sort and select it.
static GObject *
make_package_object(SearchWidgets *widgets, const PackageRow &row)
{
  GObject *obj = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
  auto *item = new PackageItem { row, {}, 0 };
  fill_package_item_status(widgets, *item);
  g_object_set_qdata_full(obj, package_row_quark(), item, +[](gpointer p) { delete static_cast<PackageItem *>(p); });
  return obj;
}

// Read the sortable package wrapper stored on a GTK list item.
static const PackageItem *
package_item_from_object(GObject *obj)
{
  if (!obj) {
    return nullptr;
  }

  return static_cast<const PackageItem *>(g_object_get_qdata(obj, package_row_quark()));
}

// Map a package wrapper back to the package row used elsewhere in the UI.
static const PackageRow *
package_row_from_object(GObject *obj)
{
  const PackageItem *item = package_item_from_object(obj);
  if (!item) {
    return nullptr;
  }

  return &item->row;
}

// -----------------------------------------------------------------------------
// Column sorter helpers
// -----------------------------------------------------------------------------
// Return the visible text for one package table cell.
static std::string
column_text(const PackageItem &item, PackageColumnKind kind)
{
  switch (kind) {
  case PackageColumnKind::STATUS:
    return item.status_text;
  case PackageColumnKind::PACKAGE:
    return item.row.name;
  case PackageColumnKind::VERSION:
    return item.row.display_version();
  case PackageColumnKind::ARCH:
    return item.row.arch;
  case PackageColumnKind::REPO:
    return item.row.repo;
  case PackageColumnKind::SUMMARY:
    return item.row.summary;
  }

  return {};
}

// Per-column data carried by the custom GTK sorter.
struct ColumnSorterData {
  PackageColumnKind kind;
};

static void
column_sorter_data_free(gpointer p)
{
  delete static_cast<ColumnSorterData *>(p);
}

// Compare two strings case-insensitively while keeping a stable fallback order.
static int
compare_text(const std::string &lhs, const std::string &rhs)
{
  char *lhs_folded = g_utf8_casefold(lhs.c_str(), -1);
  char *rhs_folded = g_utf8_casefold(rhs.c_str(), -1);

  int result = g_utf8_collate(lhs_folded, rhs_folded);
  if (result == 0) {
    result = g_utf8_collate(lhs.c_str(), rhs.c_str());
  }

  g_free(lhs_folded);
  g_free(rhs_folded);
  return result;
}

// Compare two package items for the active package table column.
static int
compare_package_items(const PackageItem &lhs, const PackageItem &rhs, PackageColumnKind kind)
{
  int result = 0;

  switch (kind) {
  case PackageColumnKind::STATUS:
    result = lhs.status_rank - rhs.status_rank;
    break;
  case PackageColumnKind::PACKAGE:
    result = compare_text(lhs.row.name, rhs.row.name);
    break;
  case PackageColumnKind::VERSION:
    result = compare_text(lhs.row.display_version(), rhs.row.display_version());
    break;
  case PackageColumnKind::ARCH:
    result = compare_text(lhs.row.arch, rhs.row.arch);
    break;
  case PackageColumnKind::REPO:
    result = compare_text(lhs.row.repo, rhs.row.repo);
    break;
  case PackageColumnKind::SUMMARY:
    result = compare_text(lhs.row.summary, rhs.row.summary);
    break;
  }

  if (result != 0) {
    return result;
  }

  result = compare_text(lhs.row.name, rhs.row.name);
  if (result != 0) {
    return result;
  }

  return compare_text(lhs.row.nevra, rhs.row.nevra);
}

// Adapter from GTK's custom sorter callback to the package item comparator.
static int
column_sorter_compare(gconstpointer item1, gconstpointer item2, gpointer user_data)
{
  const auto *data = static_cast<const ColumnSorterData *>(user_data);
  const PackageItem *lhs = package_item_from_object(G_OBJECT(const_cast<gpointer>(item1)));
  const PackageItem *rhs = package_item_from_object(G_OBJECT(const_cast<gpointer>(item2)));
  if (!data || !lhs || !rhs) {
    return 0;
  }

  return compare_package_items(*lhs, *rhs, data->kind);
}

// -----------------------------------------------------------------------------
// Package context menu helpers
// -----------------------------------------------------------------------------

// Select the package row that owns the context menu action.
static bool
select_package_table_row(GtkColumnView *view, const std::string &nevra)
{
  if (!view) {
    return false;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(view);
  if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
    return false;
  }

  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(model);
  GListModel *items_model = gtk_single_selection_get_model(selection);
  if (!items_model) {
    return false;
  }

  guint n_items = g_list_model_get_n_items(items_model);
  for (guint i = 0; i < n_items; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(items_model, i));
    const PackageRow *row = package_row_from_object(obj);
    bool match = row && row->nevra == nevra;
    g_object_unref(obj);

    if (match) {
      gtk_single_selection_set_selected(selection, i);
      return true;
    }
  }

  return false;
}

// Find the pending action for the clicked package row, if one exists.
static bool
get_context_menu_pending_action(SearchWidgets *widgets, const std::string &nevra, PendingAction::Type &out_type)
{
  for (const auto &action : widgets->transaction.actions) {
    if (action.nevra == nevra) {
      out_type = action.type;
      return true;
    }
  }

  return false;
}

// Add one transaction action to the package context menu.
static void
append_context_menu_action(GtkBox *box,
                           const char *label,
                           gboolean sensitive,
                           GCallback callback,
                           SearchWidgets *widgets)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  gtk_widget_set_sensitive(button, sensitive);
  g_signal_connect(button, "clicked", callback, widgets);
  gtk_box_append(box, button);
}

// Show the package transaction menu at the clicked table cell.
static void
show_package_context_menu(GtkWidget *anchor, SearchWidgets *widgets, const PackageRow &row, double x, double y)
{
  if (!anchor || !widgets) {
    return;
  }

  GtkWidget *view = gtk_widget_get_ancestor(anchor, GTK_TYPE_COLUMN_VIEW);
  if (!view || !GTK_IS_COLUMN_VIEW(view)) {
    return;
  }

  if (!select_package_table_row(GTK_COLUMN_VIEW(view), row.nevra)) {
    return;
  }

  GtkWidget *popover = gtk_popover_new();
  gtk_widget_set_parent(popover, anchor);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  GdkRectangle rect = { static_cast<int>(x), static_cast<int>(y), 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_popover_set_child(GTK_POPOVER(popover), box);

  PackageInstallState install_state = dnf_backend_get_package_install_state(row);
  bool is_root = (geteuid() == 0);

  PendingAction::Type pending_type;
  bool has_pending = get_context_menu_pending_action(widgets, row.nevra, pending_type);

  // Keep context menu actions aligned with the normal package action buttons.
  const char *install_label =
      has_pending && pending_type == PendingAction::INSTALL ? "Unmark Install" : "Mark for Install";
  const char *remove_label =
      has_pending && pending_type == PendingAction::REMOVE ? "Unmark Removal" : "Mark for Removal";
  const char *reinstall_label =
      has_pending && pending_type == PendingAction::REINSTALL ? "Unmark Reinstall" : "Mark for Reinstall";

  append_context_menu_action(GTK_BOX(box),
                             install_label,
                             is_root && install_state != PackageInstallState::INSTALLED,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_install_button_clicked(button, user_data);
                             }),
                             widgets);

  append_context_menu_action(GTK_BOX(box),
                             remove_label,
                             is_root && install_state == PackageInstallState::INSTALLED,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_remove_button_clicked(button, user_data);
                             }),
                             widgets);

  append_context_menu_action(GTK_BOX(box),
                             reinstall_label,
                             is_root && install_state == PackageInstallState::INSTALLED,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_reinstall_button_clicked(button, user_data);
                             }),
                             widgets);

  g_signal_connect(popover,
                   "closed",
                   G_CALLBACK(+[](GtkPopover *popover, gpointer) { gtk_widget_unparent(GTK_WIDGET(popover)); }),
                   nullptr);

  gtk_popover_popup(GTK_POPOVER(popover));
}

// -----------------------------------------------------------------------------
// Helper: Build one text column for the package table
// -----------------------------------------------------------------------------
static GtkColumnViewColumn *
create_text_column(SearchWidgets *widgets, const char *title, PackageColumnKind kind, int fixed_width, bool expand)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data(G_OBJECT(factory), "package-column-kind", GINT_TO_POINTER(static_cast<int>(kind)));
  g_object_set_data(G_OBJECT(factory), "package-table-widgets", widgets);

  g_signal_connect(
      factory,
      "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
        PackageColumnKind kind = static_cast<PackageColumnKind>(
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "package-column-kind")));

        GtkWidget *label = gtk_label_new(nullptr);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_list_item_set_activatable(item, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(label), kind == PackageColumnKind::STATUS ? 0.5f : 0.0f);

        if (kind == PackageColumnKind::STATUS) {
          gtk_widget_add_css_class(label, "package-status");
        }
        if (kind == PackageColumnKind::VERSION || kind == PackageColumnKind::ARCH || kind == PackageColumnKind::REPO) {
          gtk_widget_add_css_class(label, "package-meta");
        }
        if (kind == PackageColumnKind::SUMMARY) {
          gtk_widget_add_css_class(label, "package-summary");
        }

        // Right-click opens the same package actions as the main buttons.
        GtkGesture *context_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context_click), GDK_BUTTON_SECONDARY);
        g_signal_connect(context_click,
                         "pressed",
                         G_CALLBACK(+[](GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
                           SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                           GtkWidget *label = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
                           PackageRow *row =
                               static_cast<PackageRow *>(g_object_get_data(G_OBJECT(label), "package-context-row"));
                           if (!row) {
                             return;
                           }

                           show_package_context_menu(label, widgets, *row, x, y);
                         }),
                         g_object_get_data(G_OBJECT(factory), "package-table-widgets"));
        gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(context_click));

        gtk_list_item_set_child(item, label);
      }),
      nullptr);

  g_signal_connect(factory,
                   "bind",
                   G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
                     SearchWidgets *widgets =
                         static_cast<SearchWidgets *>(g_object_get_data(G_OBJECT(factory), "package-table-widgets"));
                     PackageColumnKind kind = static_cast<PackageColumnKind>(
                         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "package-column-kind")));

                     GtkWidget *label = gtk_list_item_get_child(item);
                     GObject *obj = G_OBJECT(gtk_list_item_get_item(item));
                     const PackageItem *package_item = package_item_from_object(obj);

                     if (!package_item) {
                       gtk_label_set_text(GTK_LABEL(label), "");
                       clear_status_css(label);
                       g_object_set_data_full(G_OBJECT(label), "package-context-row", nullptr, nullptr);
                       return;
                     }

                     // Store the package row currently bound to this reused table cell.
                     g_object_set_data_full(
                         G_OBJECT(label), "package-context-row", new PackageRow(package_item->row), +[](gpointer p) {
                           delete static_cast<PackageRow *>(p);
                         });

                     std::string text = column_text(*package_item, kind);
                     gtk_label_set_text(GTK_LABEL(label), text.c_str());

                     if (kind == PackageColumnKind::STATUS) {
                       clear_status_css(label);

                       const PackageRow &row = package_item->row;
                       if (const char *pending_class = pending_css_class(widgets, row.nevra)) {
                         gtk_widget_add_css_class(label, pending_class);
                       } else if (dnf_backend_get_package_install_state(row) == PackageInstallState::INSTALLED) {
                         gtk_widget_add_css_class(label, "package-status-installed");
                       } else if (dnf_backend_get_package_install_state(row) == PackageInstallState::UPGRADEABLE) {
                         gtk_widget_add_css_class(label, "package-status-upgradeable");
                       } else {
                         gtk_widget_add_css_class(label, "package-status-available");
                       }
                     }
                   }),
                   nullptr);

  GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
  g_object_set_data(G_OBJECT(column), "package-column-kind", GINT_TO_POINTER(static_cast<int>(kind)));
  gtk_column_view_column_set_resizable(column, TRUE);
  gtk_column_view_column_set_expand(column, expand);
  gtk_column_view_column_set_sorter(
      column,
      GTK_SORTER(gtk_custom_sorter_new(column_sorter_compare, new ColumnSorterData { kind }, column_sorter_data_free)));

  if (fixed_width > 0) {
    gtk_column_view_column_set_fixed_width(column, fixed_width);
  }
  return column;
}

// Read the current primary package table sort before rebuilding the GTK view.
static bool
get_package_view_sort_state(SearchWidgets *widgets, PackageColumnKind &out_kind, GtkSortType &out_order)
{
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return false;
  }

  GtkSorter *sorter = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(child));
  if (!sorter || !GTK_IS_COLUMN_VIEW_SORTER(sorter)) {
    return false;
  }

  GtkColumnViewColumn *column = gtk_column_view_sorter_get_primary_sort_column(GTK_COLUMN_VIEW_SORTER(sorter));
  if (!column) {
    return false;
  }

  out_kind =
      static_cast<PackageColumnKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "package-column-kind")));
  out_order = gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(sorter));
  return true;
}

// Reapply the primary package table sort after rebuilding the GTK view.
static void
restore_package_view_sort_state(GtkColumnView *view, PackageColumnKind kind, GtkSortType order)
{
  if (!view) {
    return;
  }

  GListModel *columns = gtk_column_view_get_columns(view);
  guint n_columns = g_list_model_get_n_items(columns);

  for (guint i = 0; i < n_columns; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(columns, i));
    GtkColumnViewColumn *column = GTK_COLUMN_VIEW_COLUMN(obj);
    PackageColumnKind column_kind =
        static_cast<PackageColumnKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "package-column-kind")));

    if (column_kind == kind) {
      gtk_column_view_sort_by_column(view, column, order);
      g_object_unref(obj);
      return;
    }

    g_object_unref(obj);
  }
}

// -----------------------------------------------------------------------------
// Helper: Retrieve the selected package row from the current package table
// -----------------------------------------------------------------------------
bool
package_table_get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg)
{
  if (!widgets || !widgets->results.list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->results.list_scroller);
  if (!child || !GTK_IS_COLUMN_VIEW(child)) {
    return false;
  }

  GtkSelectionModel *model = gtk_column_view_get_model(GTK_COLUMN_VIEW(child));
  if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
    return false;
  }

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  guint index = gtk_single_selection_get_selected(sel);
  if (index == GTK_INVALID_LIST_POSITION) {
    return false;
  }

  GObject *obj = G_OBJECT(g_list_model_get_item(gtk_single_selection_get_model(sel), index));
  if (!obj) {
    return false;
  }

  const PackageRow *row = package_row_from_object(obj);
  bool ok = row != nullptr;
  if (ok) {
    out_pkg = *row;
  }

  g_object_unref(obj);
  return ok;
}

// -----------------------------------------------------------------------------
// Package table population
// Builds a virtualized GTK4 ColumnView with structured package metadata while
// preserving the selected NEVRA across list refreshes when possible.
// -----------------------------------------------------------------------------
void
package_table_fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items)
{
  widgets->results.current_packages = items;

  PackageColumnKind sort_kind = PackageColumnKind::STATUS;
  GtkSortType sort_order = GTK_SORT_ASCENDING;
  bool have_sort_state = get_package_view_sort_state(widgets, sort_kind, sort_order);

  GListStore *store = g_list_store_new(G_TYPE_OBJECT);
  for (const auto &row : items) {
    GObject *obj = make_package_object(widgets, row);
    g_list_store_append(store, obj);
    g_object_unref(obj);
  }

  GtkColumnView *view = GTK_COLUMN_VIEW(gtk_column_view_new(nullptr));
  gtk_widget_set_hexpand(GTK_WIDGET(view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
  gtk_column_view_set_single_click_activate(view, FALSE);
  gtk_column_view_set_show_row_separators(view, TRUE);
  gtk_column_view_set_show_column_separators(view, TRUE);

  gtk_column_view_append_column(view, create_text_column(widgets, "Status", PackageColumnKind::STATUS, 160, FALSE));
  gtk_column_view_append_column(view, create_text_column(widgets, "Package", PackageColumnKind::PACKAGE, 180, FALSE));
  gtk_column_view_append_column(view, create_text_column(widgets, "Version", PackageColumnKind::VERSION, 150, FALSE));
  gtk_column_view_append_column(view, create_text_column(widgets, "Arch", PackageColumnKind::ARCH, 95, FALSE));
  gtk_column_view_append_column(view, create_text_column(widgets, "Repo", PackageColumnKind::REPO, 130, FALSE));
  gtk_column_view_append_column(view, create_text_column(widgets, "Summary", PackageColumnKind::SUMMARY, 0, TRUE));

  // Wrap the package list in a GTK sort model so column header clicks reorder it.
  GtkSortListModel *sort_model = gtk_sort_list_model_new(G_LIST_MODEL(store), gtk_column_view_get_sorter(view));

  GtkSingleSelection *sel = gtk_single_selection_new(nullptr);
  gtk_single_selection_set_autoselect(sel, FALSE);
  gtk_single_selection_set_can_unselect(sel, TRUE);
  gtk_single_selection_set_model(sel, G_LIST_MODEL(sort_model));
  gtk_single_selection_set_selected(sel, GTK_INVALID_LIST_POSITION);

  g_signal_connect(sel,
                   "selection-changed",
                   G_CALLBACK(+[](GtkSingleSelection *self, guint, guint, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     guint index = gtk_single_selection_get_selected(self);

                     if (index == GTK_INVALID_LIST_POSITION) {
                       package_info_clear_selected_package_state(widgets);
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(gtk_single_selection_get_model(self), index));
                     const PackageRow *row = package_row_from_object(obj);
                     if (!row) {
                       g_object_unref(obj);
                       package_info_clear_selected_package_state(widgets);
                       return;
                     }

                     PackageRow selected = *row;
                     g_object_unref(obj);
                     package_info_load_selected_package_info(widgets, selected);
                   }),
                   widgets);

  gtk_column_view_set_model(view, GTK_SELECTION_MODEL(sel));

  if (have_sort_state) {
    restore_package_view_sort_state(view, sort_kind, sort_order);
  }

  g_signal_connect(view,
                   "activate",
                   G_CALLBACK(+[](GtkColumnView *self, guint position, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     GtkSelectionModel *model = gtk_column_view_get_model(self);
                     if (!model || !GTK_IS_SINGLE_SELECTION(model)) {
                       return;
                     }

                     GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
                     GListModel *items_model = gtk_single_selection_get_model(sel);
                     if (!items_model) {
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(items_model, position));
                     if (!obj) {
                       return;
                     }

                     const PackageRow *row = package_row_from_object(obj);
                     if (!row) {
                       g_object_unref(obj);
                       return;
                     }

                     PackageInstallState install_state = dnf_backend_get_package_install_state(*row);
                     gtk_single_selection_set_selected(sel, position);
                     g_object_unref(obj);

                     if (install_state == PackageInstallState::INSTALLED) {
                       pending_transaction_on_remove_button_clicked(nullptr, widgets);
                     } else {
                       pending_transaction_on_install_button_clicked(nullptr, widgets);
                     }
                   }),
                   widgets);

  gtk_scrolled_window_set_child(widgets->results.list_scroller, GTK_WIDGET(view));
  widgets->results.listbox = nullptr;

  // Update count label
  char count_msg[128];
  snprintf(count_msg, sizeof(count_msg), "Items: %zu", items.size());
  gtk_label_set_text(widgets->results.count_label, count_msg);

  // Restore selection when the same package is still present after a refresh.
  bool restored = false;
  if (!widgets->results.selected_nevra.empty()) {
    GListModel *selected_model = gtk_single_selection_get_model(sel);
    guint n_items = g_list_model_get_n_items(selected_model);

    for (guint i = 0; i < n_items; ++i) {
      GObject *obj = G_OBJECT(g_list_model_get_item(selected_model, i));
      const PackageRow *row = package_row_from_object(obj);
      bool match = row && row->nevra == widgets->results.selected_nevra;
      g_object_unref(obj);

      if (match) {
        gtk_single_selection_set_selected(sel, i);
        restored = true;
        break;
      }
    }
  }

  if (!restored) {
    package_info_clear_selected_package_state(widgets);
  }

  g_object_unref(sort_model);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
