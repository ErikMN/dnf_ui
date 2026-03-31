// -----------------------------------------------------------------------------
// src/ui_helpers.cpp
// UI utility helpers
// Provides helper functions for updating UI widgets, handling status feedback,
// and populating a Synaptic-style GTK4 ColumnView with package metadata.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "dnf_backend.hpp"
#include "base_manager.hpp"

#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

// Task data for package-info operation.
// Snapshot generation at dispatch time so we can drop stale results after Base rebuild.
struct InfoTaskData {
  char *nevra;
  uint64_t generation;
};

static void
info_task_data_free(gpointer p)
{
  InfoTaskData *d = static_cast<InfoTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->nevra);
  g_free(d);
}

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
    q = g_quark_from_static_string("dnfui-package-row");
  }

  return q;
}

// Package row wrapper used by the sortable GTK model.
struct PackageItem {
  PackageRow row;
  std::string status_text;
  int status_rank;
};

static bool
package_is_installed(const PackageRow &row)
{
  std::lock_guard<std::mutex> lock(g_installed_mutex);
  return g_installed_nevras.count(row.nevra) > 0;
}

// Snapshot the visible status text and its sort order for one package row.
static void
fill_package_item_status(SearchWidgets *widgets, PackageItem &item)
{
  for (const auto &a : widgets->pending) {
    if (a.nevra == item.row.nevra) {
      switch (a.type) {
      case PendingAction::INSTALL:
        item.status_text = "Pending Install";
        item.status_rank = 2;
        break;
      case PendingAction::REINSTALL:
        item.status_text = "Pending Reinstall";
        item.status_rank = 3;
        break;
      case PendingAction::REMOVE:
        item.status_text = "Pending Removal";
        item.status_rank = 4;
        break;
      }
      return;
    }
  }

  if (package_is_installed(item.row)) {
    item.status_text = "Installed";
    item.status_rank = 1;
    return;
  }

  item.status_text = "Available";
  item.status_rank = 0;
}

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
// Helper: Pending action CSS class (used by the status column)
// -----------------------------------------------------------------------------
static const char *
pending_css_class(SearchWidgets *widgets, const std::string &nevra)
{
  for (const auto &a : widgets->pending) {
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

static std::string
status_text(SearchWidgets *widgets, const PackageRow &row)
{
  for (const auto &a : widgets->pending) {
    if (a.nevra == row.nevra) {
      switch (a.type) {
      case PendingAction::INSTALL:
        return "Pending Install";
      case PendingAction::REINSTALL:
        return "Pending Reinstall";
      case PendingAction::REMOVE:
        return "Pending Removal";
      }
    }
  }

  return package_is_installed(row) ? "Installed" : "Available";
}

static void
clear_status_css(GtkWidget *label)
{
  gtk_widget_remove_css_class(label, "package-status-available");
  gtk_widget_remove_css_class(label, "package-status-installed");
  gtk_widget_remove_css_class(label, "package-status-pending-install");
  gtk_widget_remove_css_class(label, "package-status-pending-reinstall");
  gtk_widget_remove_css_class(label, "package-status-pending-remove");
}

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
// Helper: Build one text column for the package table
// -----------------------------------------------------------------------------
static GtkColumnViewColumn *
create_text_column(SearchWidgets *widgets, const char *title, PackageColumnKind kind, int fixed_width, bool expand)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data(G_OBJECT(factory), "dnfui-column-kind", GINT_TO_POINTER(static_cast<int>(kind)));
  g_object_set_data(G_OBJECT(factory), "dnfui-search-widgets", widgets);

  g_signal_connect(
      factory,
      "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
        PackageColumnKind kind =
            static_cast<PackageColumnKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "dnfui-column-kind")));

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

        gtk_list_item_set_child(item, label);
      }),
      nullptr);

  g_signal_connect(factory,
                   "bind",
                   G_CALLBACK(+[](GtkSignalListItemFactory *factory, GtkListItem *item, gpointer) {
                     SearchWidgets *widgets =
                         static_cast<SearchWidgets *>(g_object_get_data(G_OBJECT(factory), "dnfui-search-widgets"));
                     PackageColumnKind kind = static_cast<PackageColumnKind>(
                         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(factory), "dnfui-column-kind")));

                     GtkWidget *label = gtk_list_item_get_child(item);
                     GObject *obj = G_OBJECT(gtk_list_item_get_item(item));
                     const PackageItem *package_item = package_item_from_object(obj);

                     if (!package_item) {
                       gtk_label_set_text(GTK_LABEL(label), "");
                       clear_status_css(label);
                       return;
                     }

                     std::string text = column_text(*package_item, kind);
                     gtk_label_set_text(GTK_LABEL(label), text.c_str());

                     if (kind == PackageColumnKind::STATUS) {
                       clear_status_css(label);

                       const PackageRow &row = package_item->row;
                       if (const char *pending_class = pending_css_class(widgets, row.nevra)) {
                         gtk_widget_add_css_class(label, pending_class);
                       } else if (package_is_installed(row)) {
                         gtk_widget_add_css_class(label, "package-status-installed");
                       } else {
                         gtk_widget_add_css_class(label, "package-status-available");
                       }
                     }
                   }),
                   nullptr);

  GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
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

// -----------------------------------------------------------------------------
// Helper: Update status label with color
// -----------------------------------------------------------------------------
void
set_status(GtkLabel *label, const std::string &text, const std::string &color)
{
  std::string bg;
  if (color == "green")
    bg = "#ccffcc";
  else if (color == "red")
    bg = "#ffcccc";
  else if (color == "blue")
    bg = "#cce5ff";
  else if (color == "gray")
    bg = "#f0f0f0";
  else
    bg = "#ffffff";

  char *escaped = g_markup_escape_text(text.c_str(), -1);
  std::string markup = "<span background=\"" + bg + "\" foreground=\"black\">" + escaped + "</span>";
  g_free(escaped);

  gtk_label_set_markup(label, markup.c_str());
}

// -----------------------------------------------------------------------------
// Helper: Retrieve the selected package row from the current package table
// -----------------------------------------------------------------------------
bool
get_selected_package_row(SearchWidgets *widgets, PackageRow &out_pkg)
{
  if (!widgets || !widgets->list_scroller) {
    return false;
  }

  GtkWidget *child = gtk_scrolled_window_get_child(widgets->list_scroller);
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
// Helper: Update transaction action button labels based on pending actions
// -----------------------------------------------------------------------------
void
update_action_button_labels(SearchWidgets *widgets, const std::string &pkg)
{
  bool pending_install = false;
  bool pending_remove = false;
  bool pending_reinstall = false;

  for (const auto &a : widgets->pending) {
    if (a.nevra == pkg) {
      pending_install = (a.type == PendingAction::INSTALL);
      pending_remove = (a.type == PendingAction::REMOVE);
      pending_reinstall = (a.type == PendingAction::REINSTALL);
      break;
    }
  }

  if (pending_install) {
    gtk_button_set_label(widgets->install_button, "Unmark Install");
    gtk_button_set_label(widgets->remove_button, "Mark for Removal");
    gtk_button_set_label(widgets->reinstall_button, "Mark for Reinstall");
  } else if (pending_reinstall) {
    gtk_button_set_label(widgets->install_button, "Mark for Install");
    gtk_button_set_label(widgets->remove_button, "Mark for Removal");
    gtk_button_set_label(widgets->reinstall_button, "Unmark Reinstall");
  } else if (pending_remove) {
    gtk_button_set_label(widgets->install_button, "Mark for Install");
    gtk_button_set_label(widgets->remove_button, "Unmark Removal");
    gtk_button_set_label(widgets->reinstall_button, "Mark for Reinstall");
  } else {
    gtk_button_set_label(widgets->install_button, "Mark for Install");
    gtk_button_set_label(widgets->remove_button, "Mark for Removal");
    gtk_button_set_label(widgets->reinstall_button, "Mark for Reinstall");
  }
}

// -----------------------------------------------------------------------------
// Package table population
// Builds a virtualized GTK4 ColumnView with structured package metadata while
// preserving the selected NEVRA across list refreshes when possible.
// -----------------------------------------------------------------------------
void
fill_package_view(SearchWidgets *widgets, const std::vector<PackageRow> &items)
{
  widgets->current_packages = items;

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
                       widgets->selected_nevra.clear();
                       gtk_widget_set_sensitive(GTK_WIDGET(widgets->install_button), FALSE);
                       gtk_widget_set_sensitive(GTK_WIDGET(widgets->remove_button), FALSE);
                       gtk_widget_set_sensitive(GTK_WIDGET(widgets->reinstall_button), FALSE);
                       update_action_button_labels(widgets, "");
                       return;
                     }

                     GObject *obj = G_OBJECT(g_list_model_get_item(gtk_single_selection_get_model(self), index));
                     const PackageRow *row = package_row_from_object(obj);
                     if (!row) {
                       g_object_unref(obj);
                       return;
                     }

                     PackageRow selected = *row;
                     widgets->selected_nevra = selected.nevra;
                     g_object_unref(obj);

                     set_status(widgets->status_label, "Fetching package info...", "blue");

                     // Enable or disable install and remove buttons based on installed state,
                     // but keep them disabled entirely if not running as root.
                     bool is_installed = package_is_installed(selected);

                     // FIXME: Replace with Polkit:
                     bool is_root = (geteuid() == 0);

                     gtk_widget_set_sensitive(GTK_WIDGET(widgets->install_button), is_root && !is_installed);
                     gtk_widget_set_sensitive(GTK_WIDGET(widgets->remove_button), is_root && is_installed);
                     gtk_widget_set_sensitive(GTK_WIDGET(widgets->reinstall_button), is_root && is_installed);
                     update_action_button_labels(widgets, selected.nevra);

                     // --- Async task: Fetch and display package info + file list ---
                     GCancellable *c = g_cancellable_new();
                     g_signal_connect_object(
                         GTK_WIDGET(widgets->entry), "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);

                     GTask *task = g_task_new(
                         NULL,
                         c,
                         +[](GObject *, GAsyncResult *res, gpointer user_data) {
                           GTask *task = G_TASK(res);
                           if (GCancellable *c = g_task_get_cancellable(task)) {
                             if (g_cancellable_is_cancelled(c)) {
                               return;
                             }
                           }

                           SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

                           const InfoTaskData *td = static_cast<const InfoTaskData *>(g_task_get_task_data(task));
                           if (!td) {
                             char *info = static_cast<char *>(g_task_propagate_pointer(task, NULL));
                             if (info) {
                               g_free(info);
                             }
                             return;
                           }

                           if (td->generation != BaseManager::instance().current_generation()) {
                             // Drop stale results, but still propagate to ensure the task result is released.
                             char *info = static_cast<char *>(g_task_propagate_pointer(task, NULL));
                             if (info) {
                               g_free(info);
                             }
                             return;
                           }

                           char *info = static_cast<char *>(g_task_propagate_pointer(task, NULL));
                           if (info) {
                             // Display general package information
                             gtk_label_set_text(widgets->details_label, info);

                             // Fetch and display the file list for the selected package
                             try {
                               std::string files = get_installed_package_files(td->nevra);
                               gtk_label_set_text(widgets->files_label, files.c_str());
                             } catch (const std::exception &e) {
                               gtk_label_set_text(widgets->files_label, e.what());
                             }

                             // Fetch and display dependencies for the selected package
                             try {
                               std::string deps = get_package_deps(td->nevra);
                               gtk_label_set_text(widgets->deps_label, deps.c_str());
                             } catch (const std::exception &e) {
                               gtk_label_set_text(widgets->deps_label, e.what());
                             }

                             // Fetch and display changelog
                             try {
                               std::string changelog = get_package_changelog(td->nevra);
                               gtk_label_set_text(widgets->changelog_label, changelog.c_str());
                             } catch (const std::exception &e) {
                               gtk_label_set_text(widgets->changelog_label, e.what());
                             }

                             set_status(widgets->status_label, "Package info loaded.", "green");
                             g_free(info);
                           } else {
                             set_status(widgets->status_label, "Error loading info.", "red");
                           }
                         },
                         widgets);

                     // Pass package NEVRA to background task
                     InfoTaskData *td = static_cast<InfoTaskData *>(g_malloc0(sizeof *td));
                     td->nevra = g_strdup(selected.nevra.c_str());
                     td->generation = BaseManager::instance().current_generation();
                     g_task_set_task_data(task, td, info_task_data_free);

                     // Run background task to fetch metadata using dnf_backend
                     g_task_run_in_thread(
                         task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *cancellable) {
                           if (cancellable && g_cancellable_is_cancelled(cancellable)) {
                             return;
                           }
                           InfoTaskData *td = static_cast<InfoTaskData *>(task_data);
                           try {
                             std::string info = get_package_info(td->nevra);
                             g_task_return_pointer(t, g_strdup(info.c_str()), g_free);
                           } catch (const std::exception &e) {
                             g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
                           }
                         });

                     g_object_unref(task);
                     g_object_unref(c);
                   }),
                   widgets);

  gtk_column_view_set_model(view, GTK_SELECTION_MODEL(sel));

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

                     bool is_installed = package_is_installed(*row);
                     gtk_single_selection_set_selected(sel, position);
                     g_object_unref(obj);

                     if (is_installed) {
                       on_remove_button_clicked(nullptr, widgets);
                     } else {
                       on_install_button_clicked(nullptr, widgets);
                     }
                   }),
                   widgets);

  gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(view));
  widgets->listbox = nullptr;

  // Update count label
  char count_msg[128];
  snprintf(count_msg, sizeof(count_msg), "Items: %zu", items.size());
  gtk_label_set_text(widgets->count_label, count_msg);

  // Restore selection when the same package is still present after a refresh.
  bool restored = false;
  if (!widgets->selected_nevra.empty()) {
    GListModel *selected_model = gtk_single_selection_get_model(sel);
    guint n_items = g_list_model_get_n_items(selected_model);

    for (guint i = 0; i < n_items; ++i) {
      GObject *obj = G_OBJECT(g_list_model_get_item(selected_model, i));
      const PackageRow *row = package_row_from_object(obj);
      bool match = row && row->nevra == widgets->selected_nevra;
      g_object_unref(obj);

      if (match) {
        gtk_single_selection_set_selected(sel, i);
        restored = true;
        break;
      }
    }
  }

  if (!restored && widgets->selected_nevra.size() > 0) {
    widgets->selected_nevra.clear();
  }

  if (!restored) {
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->install_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->remove_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->reinstall_button), FALSE);
    update_action_button_labels(widgets, "");
  }

  g_object_unref(sort_model);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
