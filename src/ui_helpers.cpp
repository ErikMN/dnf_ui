// -----------------------------------------------------------------------------
// UI utility helpers
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"
#include "widgets.hpp"
#include "dnf_backend.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <libdnf5/rpm/package_query.hpp>

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
// Virtualized ListView population
// -----------------------------------------------------------------------------
void
fill_listbox_async(SearchWidgets *widgets, const std::vector<std::string> &items, bool highlight_installed)

{
  GtkStringList *store = gtk_string_list_new(NULL);
  for (const auto &pkg : items) {
    gtk_string_list_append(store, pkg.c_str());
  }

  GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(store));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  g_signal_connect(factory,
                   "setup",
                   G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                     GtkWidget *label = gtk_label_new(nullptr);
                     gtk_label_set_xalign(GTK_LABEL(label), 0.0);
                     gtk_list_item_set_child(item, label);
                   }),
                   nullptr);

  // Pass highlight flag to the bind callback
  g_signal_connect_data(factory,
                        "bind",
                        G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer user_data) {
                          bool highlight = GPOINTER_TO_INT(user_data);
                          GtkStringObject *sobj = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
                          const char *text = gtk_string_object_get_string(sobj);
                          GtkWidget *label = gtk_list_item_get_child(item);
                          gtk_label_set_text(GTK_LABEL(label), text);

                          if (!highlight) {
                            return;
                          }
                          // Highlight installed packages
                          std::string pkg_name = text;
                          auto dash_pos = pkg_name.find('-');
                          if (dash_pos != std::string::npos) {
                            pkg_name = pkg_name.substr(0, dash_pos);
                          }
                          if (g_installed_names.count(pkg_name)) {
                            gtk_widget_add_css_class(label, "installed");
                          } else {
                            gtk_widget_remove_css_class(label, "installed");
                          }
                        }),
                        GINT_TO_POINTER(highlight_installed),
                        NULL,
                        G_CONNECT_DEFAULT);

  GtkListView *list_view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
  gtk_widget_set_hexpand(GTK_WIDGET(list_view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(list_view), TRUE);
  gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(list_view));
  widgets->listbox = nullptr;

  // update count label
  char count_msg[128];
  snprintf(count_msg, sizeof(count_msg), "Items: %zu", items.size());
  gtk_label_set_text(widgets->count_label, count_msg);

  g_signal_connect(sel,
                   "selection-changed",
                   G_CALLBACK(+[](GtkSingleSelection *self, guint, guint, gpointer user_data) {
                     SearchWidgets *widgets = (SearchWidgets *)user_data;
                     guint index = gtk_single_selection_get_selected(self);
                     if (index == GTK_INVALID_LIST_POSITION) {
                       return;
                     }

                     GObject *obj = (GObject *)g_list_model_get_item(gtk_single_selection_get_model(self), index);
                     const char *pkg_text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
                     std::string pkg_name = pkg_text;
                     g_object_unref(obj);

                     auto pos = pkg_name.find('-');
                     if (pos != std::string::npos) {
                       pkg_name = pkg_name.substr(0, pos);
                     }

                     set_status(widgets->status_label, "Fetching package info...", "blue");

                     GTask *task = g_task_new(
                         NULL,
                         NULL,
                         +[](GObject *, GAsyncResult *res, gpointer user_data) {
                           SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                           GTask *task = G_TASK(res);
                           char *info = static_cast<char *>(g_task_propagate_pointer(task, NULL));
                           if (info) {
                             gtk_label_set_text(widgets->details_label, info);

                             // Fetch and display the file list for the selected package
                             try {
                               auto base = create_fresh_base();
                               libdnf5::rpm::PackageQuery q(*base);

                               q.filter_name((const char *)g_task_get_task_data(task));
                               q.filter_installed(); // only show files for installed packages

                               if (!q.empty()) {
                                 q.filter_latest_evr();
                                 auto pkg = *q.begin();
                                 std::ostringstream files;
                                 for (const auto &f : pkg.get_files()) {
                                   files << f << "\n";
                                 }
                                 std::string files_str = files.str();
                                 if (files_str.empty()) {
                                   files_str = "No files recorded for this installed package.";
                                 }
                                 gtk_label_set_text(widgets->files_label, files_str.c_str());
                               } else {
                                 gtk_label_set_text(widgets->files_label,
                                                    "File list available only for installed packages.");
                               }
                             } catch (const std::exception &e) {
                               gtk_label_set_text(widgets->files_label, e.what());
                             }

                             set_status(widgets->status_label, "Package info loaded.", "green");
                             g_free(info);
                           } else {
                             set_status(widgets->status_label, "Error loading info.", "red");
                           }
                         },
                         widgets);

                     g_task_set_task_data(task, g_strdup(pkg_name.c_str()), g_free);
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
