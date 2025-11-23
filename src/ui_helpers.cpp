// -----------------------------------------------------------------------------
// src/ui_helpers.cpp
// UI utility helpers
// Provides helper functions for updating UI widgets, handling status feedback,
// and populating virtualized GTK4 ListView widgets with package data.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "dnf_backend.hpp"
#include "base_manager.hpp"

#include <sstream>
#include <string>
#include <vector>
#include <mutex>

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

  std::string markup = "<span background=\"" + bg + "\" foreground=\"black\">" + text + "</span>";
  gtk_label_set_markup(label, markup.c_str());
}

// -----------------------------------------------------------------------------
// Helper: Pending action CSS class (used when binding list items)
// -----------------------------------------------------------------------------
static const char *
pending_css_class(SearchWidgets *widgets, const char *pkg)
{
  if (!pkg || !*pkg) {
    return nullptr;
  }

  for (const auto &a : widgets->pending) {
    if (a.nevra == pkg) {
      return (a.type == PendingAction::INSTALL) ? "pending-install" : "pending-remove";
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// Virtualized ListView population
// Populates the main package list asynchronously using a GTK4 ListView and
// GtkStringList model. Supports optional installed-package highlighting.
// -----------------------------------------------------------------------------
void
fill_listbox_async(SearchWidgets *widgets, const std::vector<std::string> &items, bool highlight_installed)
{
  // Build a new string list model from provided package names
  GtkStringList *store = gtk_string_list_new(NULL);
  for (const auto &pkg : items) {
    gtk_string_list_append(store, pkg.c_str());
  }

  // Use GTK4 model-view setup
  GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(store));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  // Create label widgets for each list item
  g_signal_connect(factory,
                   "setup",
                   G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                     GtkWidget *label = gtk_label_new(nullptr);
                     gtk_label_set_xalign(GTK_LABEL(label), 0.0);
                     gtk_list_item_set_child(item, label);
                   }),
                   nullptr);

  // Bind callback: called whenever a list item becomes visible
  // Applies highlighting for installed packages if enabled
  g_signal_connect_data(factory,
                        "bind",
                        G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer user_data) {
                          auto *widgets = static_cast<SearchWidgets *>(user_data);

                          GtkStringObject *sobj = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
                          const char *text = gtk_string_object_get_string(sobj);
                          GtkWidget *label = gtk_list_item_get_child(item);
                          gtk_label_set_text(GTK_LABEL(label), text);

                          // -------------------------------------------------------------------
                          // Installed package highlight
                          // -------------------------------------------------------------------
                          {
                            std::lock_guard<std::mutex> lock(g_installed_mutex);
                            if (g_installed_nevras.count(text)) {
                              gtk_widget_add_css_class(label, "installed");
                            } else {
                              gtk_widget_remove_css_class(label, "installed");
                            }
                          }

                          // -------------------------------------------------------------------
                          // Pending action highlight (install/remove)
                          // -------------------------------------------------------------------
                          const char *pclass = pending_css_class(widgets, text);
                          if (pclass) {
                            gtk_widget_add_css_class(label, pclass);
                          } else {
                            gtk_widget_remove_css_class(label, "pending-install");
                            gtk_widget_remove_css_class(label, "pending-remove");
                          }
                        }),
                        widgets,
                        NULL,
                        G_CONNECT_DEFAULT);

  // Create a virtualized GTK4 ListView and attach it to the scrolled container
  GtkListView *list_view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
  gtk_widget_set_hexpand(GTK_WIDGET(list_view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(list_view), TRUE);
  gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(list_view));
  widgets->listbox = nullptr;

  // update count label
  char count_msg[128];
  snprintf(count_msg, sizeof(count_msg), "Items: %zu", items.size());
  gtk_label_set_text(widgets->count_label, count_msg);

  // ---------------------------------------------------------------------------
  // Selection callback: triggered when user selects a package from the list
  // Asynchronously fetches package info and (if installed) its file list.
  // ---------------------------------------------------------------------------
  g_signal_connect(sel,
                   "selection-changed",
                   G_CALLBACK(+[](GtkSingleSelection *self, guint, guint, gpointer user_data) {
                     SearchWidgets *widgets = (SearchWidgets *)user_data;
                     guint index = gtk_single_selection_get_selected(self);
                     if (index == GTK_INVALID_LIST_POSITION) {
                       return;
                     }

                     // Retrieve selected package name
                     GObject *obj = (GObject *)g_list_model_get_item(gtk_single_selection_get_model(self), index);
                     const char *pkg_text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
                     std::string pkg_name = pkg_text;
                     g_object_unref(obj);

                     set_status(widgets->status_label, "Fetching package info...", "blue");

                     // Enable/disable install/remove buttons based on installed state,
                     // but keep them disabled entirely if not running as root.
                     {
                       std::lock_guard<std::mutex> lock(g_installed_mutex);
                       bool is_installed = g_installed_nevras.count(pkg_name) > 0;

                       bool is_root = (geteuid() == 0);

                       gtk_widget_set_sensitive(GTK_WIDGET(widgets->install_button), is_root && !is_installed);
                       gtk_widget_set_sensitive(GTK_WIDGET(widgets->remove_button), is_root && is_installed);
                     }

                     // --- Async task: Fetch and display package info + file list ---
                     GTask *task = g_task_new(
                         NULL,
                         NULL,
                         +[](GObject *, GAsyncResult *res, gpointer user_data) {
                           SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                           GTask *task = G_TASK(res);
                           char *info = static_cast<char *>(g_task_propagate_pointer(task, NULL));
                           if (info) {
                             // Display general package information
                             gtk_label_set_text(widgets->details_label, info);

                             // Fetch and display the file list for the selected package
                             try {
                               std::string files =
                                   get_installed_package_files((const char *)g_task_get_task_data(task));
                               gtk_label_set_text(widgets->files_label, files.c_str());
                             } catch (const std::exception &e) {
                               gtk_label_set_text(widgets->files_label, e.what());
                             }

                             // Fetch and display dependencies for the selected package
                             try {
                               std::string deps = get_package_deps((const char *)g_task_get_task_data(task));
                               gtk_label_set_text(widgets->deps_label, deps.c_str());
                             } catch (const std::exception &e) {
                               gtk_label_set_text(widgets->deps_label, e.what());
                             }

                             // Fetch and display changelog
                             try {
                               std::string changelog = get_package_changelog((const char *)g_task_get_task_data(task));
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

                     // Pass package name to background task
                     g_task_set_task_data(task, g_strdup(pkg_name.c_str()), g_free);

                     // Run background task to fetch metadata using dnf_backend
                     g_task_run_in_thread(
                         task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
                           const char *pkg_name = static_cast<const char *>(task_data);
                           try {
                             std::string info = get_package_info(pkg_name);
                             g_task_return_pointer(t, g_strdup(info.c_str()), g_free);
                           } catch (const std::exception &e) {
                             g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
                           }
                         });
                     g_object_unref(task);
                   }),
                   widgets);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
