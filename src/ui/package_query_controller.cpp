// -----------------------------------------------------------------------------
// src/ui/package_query_controller.cpp
// Signal callbacks and package query controller
// Handles search, list, clear, and history actions plus the shared Stop button
// state for background package queries.
// -----------------------------------------------------------------------------
#include "widgets.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "package_info_controller.hpp"
#include "package_query_cache.hpp"
#include "package_query_controller.hpp"
#include "package_table_view.hpp"
#include "ui_helpers.hpp"
#include "widgets_internal.hpp"

// -----------------------------------------------------------------------------
// Clear cached search results.
// Used by the Clear Cache button, repository refresh, and transaction refresh.
// -----------------------------------------------------------------------------
void
package_query_clear_search_cache()
{
  package_query_cache_clear();
}

// -----------------------------------------------------------------------------
// Data passed to one background search task.
// -----------------------------------------------------------------------------
struct SearchTaskData {
  char *term;
  char *cache_key;
  uint64_t request_id;
  // BaseManager generation recorded when the task starts.
  // Used to drop outdated results if the backend Base is rebuilt before the task ends.
  uint64_t generation;
  bool search_in_description;
  bool exact_match;
};

// Data passed to one background group package task.
struct GroupPackagesTaskData {
  char *group_id;
  char *group_name;
  uint64_t request_id;
  uint64_t generation;
};

// -----------------------------------------------------------------------------
// Free data owned by one background search task.
// -----------------------------------------------------------------------------
static void
search_task_data_free(gpointer p)
{
  SearchTaskData *d = static_cast<SearchTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->term);
  g_free(d->cache_key);
  g_free(d);
}

// -----------------------------------------------------------------------------
// Free data owned by one background group package task.
// -----------------------------------------------------------------------------
static void
group_packages_task_data_free(gpointer p)
{
  GroupPackagesTaskData *d = static_cast<GroupPackagesTaskData *>(p);
  if (!d) {
    return;
  }
  g_free(d->group_id);
  g_free(d->group_name);
  g_free(d);
}

// -----------------------------------------------------------------------------
// Record which main query flow produced the currently displayed table.
// Transaction and repository rebuilds use this to rerun the same query and
// replace outdated rows with fresh backend data.
// -----------------------------------------------------------------------------
static void
set_displayed_query_kind(SearchWidgets *widgets, DisplayedPackageQueryKind kind)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = kind;
}

// -----------------------------------------------------------------------------
// Preserve the active search term and flags so a post-transaction refresh can
// rebuild the visible search results even if the user changes the checkboxes
// while the background work is still running.
// -----------------------------------------------------------------------------
static void
set_displayed_search_query(SearchWidgets *widgets, const SearchTaskData &task_data)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = DisplayedPackageQueryKind::SEARCH;
  widgets->query_state.displayed_query.search_term = task_data.term ? task_data.term : "";
  widgets->query_state.displayed_query.search_in_description = task_data.search_in_description;
  widgets->query_state.displayed_query.exact_match = task_data.exact_match;
}

// -----------------------------------------------------------------------------
// Preserve the selected package group so refresh can rebuild the same group view.
// -----------------------------------------------------------------------------
static void
set_displayed_group_query(SearchWidgets *widgets, const GroupPackagesTaskData &task_data)
{
  if (!widgets) {
    return;
  }

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.displayed_query.kind = DisplayedPackageQueryKind::GROUP;
  widgets->query_state.displayed_query.group_id = task_data.group_id ? task_data.group_id : "";
  widgets->query_state.displayed_query.group_name = task_data.group_name ? task_data.group_name : "";
}

// -----------------------------------------------------------------------------
// Complete one rebuild-triggered refresh. When the old selection survived the
// refreshed query result, leave the details pane intact; otherwise clear it so
// outdated package info is not shown for rows that disappeared.
// -----------------------------------------------------------------------------
static void
finish_results_refresh(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  if (widgets->query_state.preserve_selection_on_reload) {
    PackageRow selected;
    if (!package_table_get_selected_package_row(widgets, selected)) {
      package_info_reset_details_view(widgets);
    }
  } else {
    package_info_reset_details_view(widgets);
  }

  widgets->query_state.preserve_selection_on_reload = false;
  widgets->query_state.reload_selected_nevra.clear();
}

// Data passed to one background package list task.
// The generation lets completion ignore results from an older backend Base.
// The request id keeps the Stop button matched to the task that controls it.
struct PackageListTaskData {
  uint64_t request_id;
  uint64_t generation;
};

// -----------------------------------------------------------------------------
// Return true when a package list task is currently running.
// -----------------------------------------------------------------------------
static bool
has_active_package_list_request(const SearchWidgets *widgets)
{
  return widgets && widgets->query_state.package_list_cancellable &&
      !g_cancellable_is_cancelled(widgets->query_state.package_list_cancellable);
}

// -----------------------------------------------------------------------------
// Return the button that currently works as Stop.
// -----------------------------------------------------------------------------
static GtkButton *
package_list_stop_button(SearchWidgets *widgets, PackageListRequestKind kind)
{
  if (!widgets) {
    return nullptr;
  }

  switch (kind) {
  case PackageListRequestKind::LIST_INSTALLED:
    return widgets->query.list_button;
  case PackageListRequestKind::LIST_AVAILABLE:
    return widgets->query.list_available_button;
  case PackageListRequestKind::GROUP:
    return widgets->query.search_button;
  case PackageListRequestKind::SEARCH:
  case PackageListRequestKind::NONE:
  default:
    return widgets->query.search_button;
  }
}

// -----------------------------------------------------------------------------
// Human-readable cancel status for the current background package list request.
// -----------------------------------------------------------------------------
static const char *
package_list_cancelled_status(PackageListRequestKind kind)
{
  switch (kind) {
  case PackageListRequestKind::SEARCH:
    return "Search cancelled.";
  case PackageListRequestKind::LIST_INSTALLED:
    return "Listing installed packages cancelled.";
  case PackageListRequestKind::LIST_AVAILABLE:
    return "Listing packages cancelled.";
  case PackageListRequestKind::GROUP:
    return "Group package listing cancelled.";
  case PackageListRequestKind::NONE:
  default:
    return "Operation cancelled.";
  }
}

// -----------------------------------------------------------------------------
// Record the active package list task and switch its button to Stop.
// -----------------------------------------------------------------------------
static void
begin_package_list_request(SearchWidgets *widgets, GCancellable *c, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || !c) {
    return;
  }

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
  }

  widgets->query_state.package_list_cancellable = G_CANCELLABLE(g_object_ref(c));
  widgets->query_state.current_package_list_request_id = request_id;
  widgets->query_state.current_package_list_request_kind = kind;
  GtkButton *stop_button = package_list_stop_button(widgets, kind);

  ui_helpers_set_icon_button(widgets->query.search_button, "system-search-symbolic", "Search");
  ui_helpers_set_icon_button(widgets->query.list_button, "view-list-symbolic", "List Installed");
  ui_helpers_set_icon_button(widgets->query.list_available_button, "view-list-symbolic", "List Packages");
  ui_helpers_set_icon_button(stop_button, "process-stop-symbolic", "Stop");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.group_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(stop_button), TRUE);
}

// -----------------------------------------------------------------------------
// Restore the normal search and list controls after a package query stops or finishes.
// -----------------------------------------------------------------------------
static void
restore_package_list_controls(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  ui_helpers_set_icon_button(widgets->query.search_button, "system-search-symbolic", "Search");
  ui_helpers_set_icon_button(widgets->query.list_button, "view-list-symbolic", "List Installed");
  ui_helpers_set_icon_button(widgets->query.list_available_button, "view-list-symbolic", "List Packages");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.desc_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.exact_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.history_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.group_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.list_available_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.search_button), TRUE);
}

// -----------------------------------------------------------------------------
// Restore the package list controls when the active task is done.
// -----------------------------------------------------------------------------
static void
end_package_list_request(SearchWidgets *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->query_state.current_package_list_request_id != request_id ||
      widgets->query_state.current_package_list_request_kind != kind) {
    return;
  }

  if (widgets->query_state.package_list_cancellable) {
    g_object_unref(widgets->query_state.package_list_cancellable);
    widgets->query_state.package_list_cancellable = nullptr;
  }
  widgets->query_state.current_package_list_request_id = 0;
  widgets->query_state.current_package_list_request_kind = PackageListRequestKind::NONE;
  restore_package_list_controls(widgets);
}

// -----------------------------------------------------------------------------
// Cancel the active package list request and immediately unlock the shared controls.
// -----------------------------------------------------------------------------
static void
cancel_active_package_list_request(SearchWidgets *widgets)
{
  if (!widgets || !widgets->query_state.package_list_cancellable) {
    return;
  }

  uint64_t request_id = widgets->query_state.current_package_list_request_id;
  PackageListRequestKind kind = widgets->query_state.current_package_list_request_kind;
  GCancellable *c = widgets->query_state.package_list_cancellable;
  if (!g_cancellable_is_cancelled(c)) {
    g_cancellable_cancel(c);
  }

  // Release only the spinner slot owned by this request so other running tasks
  // can keep their progress indication visible.
  widgets_spinner_release(widgets->query.spinner);

  // Clear the active request after its controls have been restored.
  end_package_list_request(widgets, request_id, kind);

  ui_helpers_set_status(widgets->query.status_label, package_list_cancelled_status(kind), "gray");
}

// -----------------------------------------------------------------------------
// Background package query tasks
// GTK runs the window on the main thread. GTask lets package queries run on a
// worker thread so the window stays responsive.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Remove all rows from one list box.
// -----------------------------------------------------------------------------
static void
clear_list_box(GtkListBox *list_box)
{
  if (!list_box) {
    return;
  }

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list_box));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(list_box, child);
    child = next;
  }
}

// -----------------------------------------------------------------------------
// Add one plain informational row to a list box.
// -----------------------------------------------------------------------------
static void
append_plain_list_row(GtkListBox *list_box, const char *text)
{
  if (!list_box || !text) {
    return;
  }

  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_margin_start(label, 8);
  gtk_widget_set_margin_end(label, 8);
  gtk_widget_set_margin_top(label, 8);
  gtk_widget_set_margin_bottom(label, 8);
  gtk_list_box_append(list_box, label);
}

// -----------------------------------------------------------------------------
// Build one visible group row for the left panel.
// -----------------------------------------------------------------------------
static GtkWidget *
create_group_row(const PackageGroup &group)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  GtkWidget *name_label = gtk_label_new(group.name.c_str());
  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(name_label), TRUE);
  gtk_box_append(GTK_BOX(box), name_label);

  char package_count[64];
  const char *package_word = group.package_count == 1 ? "package" : "packages";
  snprintf(package_count, sizeof(package_count), "%zu %s", group.package_count, package_word);
  GtkWidget *count_label = gtk_label_new(package_count);
  gtk_label_set_xalign(GTK_LABEL(count_label), 0.0f);
  gtk_widget_add_css_class(count_label, "package-meta");
  gtk_box_append(GTK_BOX(box), count_label);

  if (!group.description.empty()) {
    GtkWidget *description_label = gtk_label_new(group.description.c_str());
    gtk_label_set_xalign(GTK_LABEL(description_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(description_label), TRUE);
    gtk_widget_add_css_class(description_label, "package-meta");
    gtk_box_append(GTK_BOX(box), description_label);
  }

  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "dnfui-group-id", g_strdup(group.id.c_str()), g_free);
  g_object_set_data_full(G_OBJECT(row), "dnfui-group-name", g_strdup(group.name.c_str()), g_free);

  return row;
}

// -----------------------------------------------------------------------------
// Query package groups on a worker thread.
// -----------------------------------------------------------------------------
static void
on_group_list_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    auto *groups = new std::vector<PackageGroup>(dnf_backend_get_package_groups_interruptible(cancellable));
    g_task_return_pointer(task, groups, [](gpointer p) { delete static_cast<std::vector<PackageGroup> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish package group loading on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_group_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets_task_should_skip_completion(task, widgets)) {
    return;
  }

  GError *error = nullptr;
  std::vector<PackageGroup> *groups = static_cast<std::vector<PackageGroup> *>(g_task_propagate_pointer(task, &error));

  clear_list_box(widgets->query.group_list);

  if (groups) {
    if (groups->empty()) {
      append_plain_list_row(widgets->query.group_list, "No package groups found.");
    } else {
      for (const auto &group : *groups) {
        gtk_list_box_append(widgets->query.group_list, create_group_row(group));
      }
    }
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.group_list), !has_active_package_list_request(widgets));
    delete groups;
  } else {
    append_plain_list_row(widgets->query.group_list, error ? error->message : "Could not load package groups.");
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.group_list), !has_active_package_list_request(widgets));
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Query installed packages on a worker thread.
// -----------------------------------------------------------------------------
static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    // Query all installed packages.
    auto *results = new std::vector<PackageRow>(dnf_backend_get_installed_package_rows_interruptible(cancellable));
    // Let GTask free results if completion is skipped or cancelled.
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish installed package listing on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
        }
      }
    }
    return;
  }

  // Drop outdated results if the backend Base changed while the worker was running.
  // This prevents showing rows that no longer match the current repository state.
  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  }

  if (packages) {
    set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_INSTALLED);

    // Fill the package table and update status.
    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error listing packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Query the merged package list on a worker thread.
// The result includes repository candidates and installed-only local RPMs.
// -----------------------------------------------------------------------------
static void
on_list_available_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    auto *results = new std::vector<PackageRow>(dnf_backend_get_browse_package_rows_interruptible(cancellable));
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish merged package listing on the GTK thread.
// Refresh the installed snapshot before updating package status in the table.
// -----------------------------------------------------------------------------
static void
on_list_available_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const PackageListTaskData *td = static_cast<const PackageListTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
        }
      }
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  }

  if (packages) {
    set_displayed_query_kind(widgets, DisplayedPackageQueryKind::LIST_AVAILABLE);

    dnf_backend_refresh_installed_nevras();

    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error listing packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Query packages that belong to one package group on a worker thread.
// -----------------------------------------------------------------------------
static void
on_group_packages_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  const GroupPackagesTaskData *td = static_cast<const GroupPackagesTaskData *>(task_data);
  const char *group_id = td && td->group_id ? td->group_id : "";

  try {
    auto *results =
        new std::vector<PackageRow>(dnf_backend_get_package_group_package_rows_interruptible(group_id, cancellable));
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish group package listing on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_group_packages_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  const GroupPackagesTaskData *td = static_cast<const GroupPackagesTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed) {
      if (GCancellable *c = g_task_get_cancellable(task)) {
        if (g_cancellable_is_cancelled(c) && td) {
          end_package_list_request(widgets, td->request_id, PackageListRequestKind::GROUP);
        }
      }
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::GROUP);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::GROUP);
  }

  if (packages) {
    if (td) {
      set_displayed_group_query(widgets, *td);
    }

    dnf_backend_refresh_installed_nevras();

    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(widgets, *packages);

    char msg[256];
    const char *group_name = td && td->group_name ? td->group_name : "group";
    snprintf(msg, sizeof(msg), "Found %zu packages in %s.", packages->size(), group_name);
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error listing group packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Search the merged package list on a worker thread.
// -----------------------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  const SearchTaskData *td = static_cast<const SearchTaskData *>(task_data);
  const char *pattern = td ? td->term : "";
  try {
    DNFUI_TRACE(
        "Search task start request=%llu pattern=%s", td ? static_cast<unsigned long long>(td->request_id) : 0, pattern);
    auto *results = new std::vector<PackageRow>(dnf_backend_search_package_rows_interruptible(pattern, cancellable));
    DNFUI_TRACE("Search task done request=%llu results=%zu",
                td ? static_cast<unsigned long long>(td->request_id) : 0,
                results->size());
    // Let GTask free results if completion is skipped or cancelled.
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
  } catch (const std::exception &e) {
    DNFUI_TRACE(
        "Search task failed request=%llu error=%s", td ? static_cast<unsigned long long>(td->request_id) : 0, e.what());
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Finish a package search on the GTK thread.
// -----------------------------------------------------------------------------
static void
on_search_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GCancellable *c = g_task_get_cancellable(task);
  const SearchTaskData *td = static_cast<const SearchTaskData *>(g_task_get_task_data(task));

  if (widgets_task_should_skip_completion(task, widgets)) {
    if (widgets && !widgets->window_state.destroyed && c && g_cancellable_is_cancelled(c) && td) {
      end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    widgets_spinner_release(widgets->query.spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = static_cast<std::vector<PackageRow> *>(g_task_propagate_pointer(task, &error));

  // Release this task's spinner slot.
  widgets_spinner_release(widgets->query.spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
  }

  if (packages) {
    // Save rows so the same search can be shown faster next time.
    // Search results are only reusable while the backend Base generation stays
    // the same, otherwise repo state may have changed underneath the cache.
    if (td && td->cache_key) {
      package_query_cache_store(td->cache_key, td->generation, *packages);
    }

    if (td) {
      set_displayed_search_query(widgets, *td);
    }

    dnf_backend_refresh_installed_nevras();

    // Fill the package table and display the result count.
    if (widgets->query_state.preserve_selection_on_reload) {
      widgets->results.selected_nevra = widgets->query_state.reload_selected_nevra;
    } else {
      widgets->results.selected_nevra.clear();
    }
    package_table_fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    ui_helpers_set_status(widgets->query.status_label, msg, "green");
    finish_results_refresh(widgets);
    delete packages;
  } else {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    ui_helpers_set_status(widgets->query.status_label, error ? error->message : "Error or no results.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Add a new search term to history if it is not already present.
// -----------------------------------------------------------------------------
static void
add_to_history(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Prevent duplicate history entries.
  for (const auto &s : widgets->query_state.history) {
    if (s == term) {
      return;
    }
  }

  // Append the new term to internal state and the visible history list.
  widgets->query_state.history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->query.history_list, row);
}

// -----------------------------------------------------------------------------
// Run a search from cache or start a background search task.
// -----------------------------------------------------------------------------
static void
perform_search(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Include the current checkboxes in the cache key even for history searches.
  dnf_backend_set_search_options({
      .search_in_description =
          static_cast<bool>(gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.desc_checkbox))),
      .exact_match = static_cast<bool>(gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->query.exact_checkbox))),
  });
  const DnfBackendSearchOptions search_options = dnf_backend_get_search_options();

  gtk_editable_set_text(GTK_EDITABLE(widgets->query.entry), term.c_str());
  ui_helpers_set_status(widgets->query.status_label, ("Searching for '" + term + "'...").c_str(), "blue");
  if (!widgets->query_state.preserve_selection_on_reload) {
    widgets->results.selected_nevra.clear();
  }

  // Look up saved rows before starting a new backend query.
  // Reuse only results produced from the current Base generation so refreshes
  // and transaction rebuilds cannot surface outdated package metadata.
  const std::string key = package_query_cache_key_for(term);
  const uint64_t generation = BaseManager::instance().current_generation();
  std::vector<PackageRow> cached_packages;
  if (package_query_cache_lookup(key, generation, cached_packages)) {
    // Show saved rows and skip the worker thread.
    SearchTaskData cached_td {};
    cached_td.term = const_cast<char *>(term.c_str());
    cached_td.search_in_description = search_options.search_in_description;
    cached_td.exact_match = search_options.exact_match;
    set_displayed_search_query(widgets, cached_td);

    package_table_fill_package_view(widgets, cached_packages);

    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded %zu cached results.", cached_packages.size());
    ui_helpers_set_status(widgets->query.status_label, msg, "gray");
    finish_results_refresh(widgets);

    return;
  }

  // No cache match, so start a worker thread search.
  widgets_spinner_acquire(widgets->query.spinner);

  SearchTaskData *td = static_cast<SearchTaskData *>(g_malloc0(sizeof *td));
  td->term = g_strdup(term.c_str());
  td->cache_key = g_strdup(key.c_str());
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = generation;
  td->search_in_description = search_options.search_in_description;
  td->exact_match = search_options.exact_match;

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  // Disable the search controls and make the Search button stop this task.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::SEARCH);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_search_task_finished);
  g_task_set_task_data(task, td, search_task_data_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Start a background package listing for one DNF package group.
// -----------------------------------------------------------------------------
static void
perform_group_package_list(SearchWidgets *widgets, const std::string &group_id, const std::string &group_name)
{
  if (!widgets || group_id.empty()) {
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, ("Listing packages in " + group_name + "...").c_str(), "blue");

  if (!widgets->query_state.preserve_selection_on_reload) {
    widgets->results.selected_nevra.clear();
  }

  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.group_list));
  GroupPackagesTaskData *td = static_cast<GroupPackagesTaskData *>(g_malloc0(sizeof *td));
  td->group_id = g_strdup(group_id.c_str());
  td->group_name = g_strdup(group_name.c_str());
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::GROUP);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_group_packages_task_finished);
  g_task_set_task_data(task, td, group_packages_task_data_free);
  g_task_run_in_thread(task, on_group_packages_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Load DNF package groups into the left panel.
// -----------------------------------------------------------------------------
void
package_query_load_groups(SearchWidgets *widgets)
{
  if (!widgets || !widgets->query.group_list) {
    return;
  }

  clear_list_box(widgets->query.group_list);
  append_plain_list_row(widgets->query.group_list, "Loading package groups...");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->query.group_list), FALSE);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.group_list));
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_group_list_task_finished);
  g_task_run_in_thread(task, on_group_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Handle the List Installed button.
// The same button changes to Stop while its worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_INSTALLED) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, "Listing installed packages...", "blue");

  // Show the spinner for this task.
  widgets_spinner_acquire(widgets->query.spinner);

  // Start the installed package query on a worker thread.
  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  // Store the generation so completion can reject outdated results.
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  // Disable the query controls and make List Installed stop this task.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_list_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Handle the List Packages button.
// Starts background listing of the merged package view.
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_list_available_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::LIST_AVAILABLE) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  ui_helpers_set_status(widgets->query.status_label, "Listing packages...", "blue");

  // Show the spinner for this task.
  widgets_spinner_acquire(widgets->query.spinner);

  GCancellable *c = widgets_make_task_cancellable_for(GTK_WIDGET(widgets->query.entry));
  PackageListTaskData *td = new PackageListTaskData;
  td->request_id = widgets->query_state.next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  // Disable the query controls and make List Packages stop this task.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_AVAILABLE);
  GTask *task = widgets_task_new_for_search_widgets(widgets, c, on_list_available_task_finished);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<PackageListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_available_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Handle the Search button or Enter in the search field.
// Reads options, checks the cache, and starts background search when needed.
// The same button acts as Stop while a search worker task is running.
// -----------------------------------------------------------------------------
void
package_query_on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    if (widgets->query_state.current_package_list_request_kind == PackageListRequestKind::SEARCH ||
        widgets->query_state.current_package_list_request_kind == PackageListRequestKind::GROUP) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  const char *txt = gtk_editable_get_text(GTK_EDITABLE(widgets->query.entry));
  std::string pattern = txt ? txt : "";

  if (pattern.empty()) {
    return;
  }

  // Save the search term and start lookup.
  add_to_history(widgets, pattern);
  perform_search(widgets, pattern);
}

// -----------------------------------------------------------------------------
// Handle selecting a search term from the history list.
// -----------------------------------------------------------------------------
void
package_query_on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row) {
    return;
  }

  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GtkWidget *child = gtk_list_box_row_get_child(row);
  const char *term = gtk_label_get_text(GTK_LABEL(child));
  perform_search(widgets, term);
}

// -----------------------------------------------------------------------------
// Handle selecting a package group from the left panel.
// -----------------------------------------------------------------------------
void
package_query_on_group_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row) {
    return;
  }

  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  if (has_active_package_list_request(widgets)) {
    return;
  }

  const char *group_id = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "dnfui-group-id"));
  const char *group_name = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "dnfui-group-name"));
  if (!group_id || !*group_id) {
    return;
  }

  perform_group_package_list(widgets, group_id, group_name && *group_name ? group_name : group_id);
}

// -----------------------------------------------------------------------------
// Handle the Clear List button.
// Clears displayed package rows and resets the details panel.
// -----------------------------------------------------------------------------
void
package_query_on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  cancel_active_package_list_request(widgets);

  widgets->query_state.displayed_query = DisplayedPackageQueryState();
  widgets->query_state.preserve_selection_on_reload = false;
  widgets->query_state.reload_selected_nevra.clear();
  widgets->results.current_packages.clear();
  widgets->results.selected_nevra.clear();
  package_table_fill_package_view(widgets, {});

  // Reset status labels and package actions.
  ui_helpers_set_status(widgets->query.status_label, "Ready.", "gray");
  package_info_reset_details_view(widgets);
  ui_helpers_update_action_button_labels(widgets, "");
}

// -----------------------------------------------------------------------------
// Rebuild the currently displayed package table after a transaction or repo
// refresh. Query-backed views are replayed through their normal async entry
// points, exact one-package views are refreshed from the selected NEVRA.
// -----------------------------------------------------------------------------
void
package_query_reload_current_view(SearchWidgets *widgets)
{
  if (!widgets || has_active_package_list_request(widgets)) {
    return;
  }

  widgets->query_state.preserve_selection_on_reload = !widgets->results.selected_nevra.empty();
  widgets->query_state.reload_selected_nevra = widgets->results.selected_nevra;

  const DisplayedPackageQueryState view_state = widgets->query_state.displayed_query;

  switch (view_state.kind) {
  case DisplayedPackageQueryKind::SEARCH:
    if (view_state.search_term.empty()) {
      widgets->query_state.preserve_selection_on_reload = false;
      widgets->query_state.reload_selected_nevra.clear();
      return;
    }

    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->query.desc_checkbox), view_state.search_in_description);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->query.exact_checkbox), view_state.exact_match);
    perform_search(widgets, view_state.search_term);
    return;
  case DisplayedPackageQueryKind::LIST_INSTALLED:
    package_query_on_list_button_clicked(nullptr, widgets);
    return;
  case DisplayedPackageQueryKind::LIST_AVAILABLE:
    package_query_on_list_available_button_clicked(nullptr, widgets);
    return;
  case DisplayedPackageQueryKind::GROUP:
    if (view_state.group_id.empty()) {
      widgets->query_state.preserve_selection_on_reload = false;
      widgets->query_state.reload_selected_nevra.clear();
      return;
    }

    perform_group_package_list(widgets, view_state.group_id, view_state.group_name);
    return;
  case DisplayedPackageQueryKind::NONE:
  default:
    break;
  }

  // Exact one-package views are not part of the main query state. When the
  // user is reviewing one package from the pending-actions sidebar, refresh the
  // selected NEVRA directly so removed rows disappear without carrying extra
  // global view bookkeeping.
  if (widgets->results.selected_nevra.empty()) {
    widgets->query_state.preserve_selection_on_reload = false;
    widgets->query_state.reload_selected_nevra.clear();
    return;
  }

  std::vector<PackageRow> rows = dnf_backend_get_installed_package_rows_by_nevra(widgets->results.selected_nevra);
  if (rows.empty()) {
    rows = dnf_backend_get_available_package_rows_by_nevra(widgets->results.selected_nevra);
  }

  widgets->results.selected_nevra = rows.empty() ? "" : widgets->query_state.reload_selected_nevra;
  package_table_fill_package_view(widgets, rows);
  finish_results_refresh(widgets);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
