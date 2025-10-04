#include <array>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <thread>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <gtk/gtk.h>

using namespace std;

// ------------------------------------------------------------
// Helper: Query installed packages via libdnf5
// ------------------------------------------------------------
static std::vector<std::string>
get_installed_packages()
{
  std::vector<std::string> packages;

  libdnf5::Base base;
  base.load_config();
  base.setup();

  // Load system repositories (RPM DB only, fast and offline)
  auto repo_sack = base.get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  repo_sack->load_repos();

  libdnf5::rpm::PackageQuery query(base);
  query.filter_installed();

  for (auto pkg : query) {
    packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
  }

  return packages;
}

// ------------------------------------------------------------
// Helper: Search available packages by substring
// ------------------------------------------------------------
static std::vector<std::string>
search_available_packages(const std::string &pattern)
{
  std::vector<std::string> packages;

  libdnf5::Base base;
  base.load_config();
  base.setup();

  // Load configured repositories (may take a few seconds)
  auto repo_sack = base.get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  repo_sack->load_repos();

  libdnf5::rpm::PackageQuery query(base);
  query.filter_available();
  query.filter_name(pattern, libdnf5::sack::QueryCmp::CONTAINS);

  for (auto pkg : query) {
    packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
  }

  return packages;
}

// ------------------------------------------------------------
// Utility: fill a GtkListBox with strings
// ------------------------------------------------------------
static void
fill_listbox(GtkListBox *listbox, const std::vector<std::string> &items)
{
#if GTK_CHECK_VERSION(4, 10, 0)
  gtk_list_box_remove_all(listbox);
#else
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(listbox, 0)) {
    gtk_list_box_remove(listbox, GTK_WIDGET(row));
  }
#endif

  for (const auto &text : items) {
    GtkWidget *row = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(row), 0.0);
    gtk_list_box_append(listbox, row);
  }
}

// ------------------------------------------------------------
// Struct to hold widgets for the search callback
// ------------------------------------------------------------
struct SearchWidgets {
  GtkEntry *entry;
  GtkListBox *listbox;
  GtkSpinner *spinner;
};

// ------------------------------------------------------------
// Button callbacks
// ------------------------------------------------------------
static void
on_list_button_clicked(GtkButton *button, gpointer user_data)
{
  GtkListBox *listbox = GTK_LIST_BOX(user_data);
  auto packages = get_installed_packages();
  fill_listbox(listbox, packages);
}

// Async task completion handler
static void
on_search_task_finished(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GTask *task = G_TASK(res);
  std::vector<std::string> *packages =
      (std::vector<std::string> *)g_task_propagate_pointer(task, NULL);

  // Stop spinner
  gtk_spinner_stop(widgets->spinner);
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);

  if (packages) {
    fill_listbox(widgets->listbox, *packages);
    delete packages;
  }
}

// Background thread function
static void
on_search_task(GTask *task,
               gpointer source,
               gpointer task_data,
               GCancellable *cancellable)
{
  const char *pattern = (const char *)task_data;
  auto *results =
      new std::vector<std::string>(search_available_packages(pattern));
  g_task_return_pointer(task, results, NULL);
}

// Search button clicked
static void
on_search_button_clicked(GtkButton *button, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  const char *pattern = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  if (!pattern || !*pattern)
    return;

  // Show spinner while searching
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), TRUE);
  gtk_spinner_start(widgets->spinner);

  // Run async task
  GTask *task = g_task_new(NULL, NULL, on_search_task_finished, widgets);
  g_task_set_task_data(task, g_strdup(pattern), g_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
}

static void
on_clear_button_clicked(GtkButton *button, gpointer user_data)
{
  GtkListBox *listbox = GTK_LIST_BOX(user_data);

#if GTK_CHECK_VERSION(4, 10, 0)
  gtk_list_box_remove_all(listbox);
#else
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(listbox, 0)) {
    gtk_list_box_remove(listbox, GTK_WIDGET(row));
  }
#endif
}

// ------------------------------------------------------------
// GTK app setup
// ------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer user_data)
{
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "DNF Package Viewer");
  gtk_window_set_default_size(GTK_WINDOW(window), 750, 600);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox);

  // --- Top row with buttons and search entry ---
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox), hbox);

  GtkWidget *list_button = gtk_button_new_with_label("List Installed");
  gtk_box_append(GTK_BOX(hbox), list_button);

  GtkWidget *clear_button = gtk_button_new_with_label("Clear List");
  gtk_box_append(GTK_BOX(hbox), clear_button);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                 "Search available packages...");
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox), entry);

  GtkWidget *search_button = gtk_button_new_with_label("Search");
  gtk_box_append(GTK_BOX(hbox), search_button);

  // --- Spinner indicator ---
  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox), spinner);

  // --- Scrollable list ---
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(vbox), scrolled);

  GtkWidget *listbox = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), listbox);

  // --- Connect signals ---
  g_signal_connect(
      list_button, "clicked", G_CALLBACK(on_list_button_clicked), listbox);

  g_signal_connect(
      clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), listbox);

  SearchWidgets *widgets = g_new(SearchWidgets, 1);
  widgets->entry = GTK_ENTRY(entry);
  widgets->listbox = GTK_LIST_BOX(listbox);
  widgets->spinner = GTK_SPINNER(spinner);

  g_signal_connect(
      search_button, "clicked", G_CALLBACK(on_search_button_clicked), widgets);

  gtk_window_present(GTK_WINDOW(window));
}

// ------------------------------------------------------------
// main()
// ------------------------------------------------------------
int
main(int argc, char *argv[])
{
  GtkApplication *app =
      gtk_application_new("com.example.dnfgtk", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
