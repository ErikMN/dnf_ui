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
// Helper: get detailed info about a single package
// ------------------------------------------------------------
static std::string
get_package_info(const std::string &pkg_name)
{
  libdnf5::Base base;
  base.load_config();
  base.setup();

  auto repo_sack = base.get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  repo_sack->load_repos();

  libdnf5::rpm::PackageQuery query(base);
  query.filter_name(pkg_name);

  if (query.empty()) {
    return "No details found for " + pkg_name;
  }

  auto pkg = *query.begin();
  std::ostringstream oss;
  oss << "Name: " << pkg.get_name() << "\n"
      << "Version: " << pkg.get_version() << "\n"
      << "Release: " << pkg.get_release() << "\n"
      << "Arch: " << pkg.get_arch() << "\n"
      << "Repo: " << pkg.get_repo_id() << "\n"
      << "Summary: " << pkg.get_summary() << "\n"
      << "Description:\n"
      << pkg.get_description();
  return oss.str();
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
// Struct to hold widgets for callbacks
// ------------------------------------------------------------
struct SearchWidgets {
  GtkEntry *entry;
  GtkListBox *listbox;
  GtkSpinner *spinner;
  GtkButton *search_button;
  GtkLabel *status_label;
  GtkLabel *details_label;
};

// ------------------------------------------------------------
// Button callbacks
// ------------------------------------------------------------
static void
on_list_button_clicked(GtkButton *button, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GtkListBox *listbox = widgets->listbox;

  gtk_label_set_text(widgets->status_label, "Listing installed packages...");

  // Allow UI update (GTK4-safe)
  while (g_main_context_iteration(nullptr, false))
    ;

  auto packages = get_installed_packages();
  fill_listbox(listbox, packages);

  char msg[256];
  snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages.size());
  gtk_label_set_text(widgets->status_label, msg);
  gtk_label_set_text(widgets->details_label, "Select a package for details.");
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

  // Re-enable UI
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    fill_listbox(widgets->listbox, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    gtk_label_set_text(widgets->status_label, msg);
    gtk_label_set_text(widgets->details_label, "Select a package for details.");
    delete packages;
  } else {
    gtk_label_set_text(widgets->status_label, "No results or error occurred.");
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
  try {
    auto *results =
        new std::vector<std::string>(search_available_packages(pattern));
    g_task_return_pointer(task, results, NULL);
  } catch (const std::exception &e) {
    g_task_return_error(
        task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// Search button clicked
static void
on_search_button_clicked(GtkButton *button, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  const char *pattern = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  if (!pattern || !*pattern)
    return;

  gtk_label_set_text(widgets->status_label, "Searching...");
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), TRUE);
  gtk_spinner_start(widgets->spinner);

  // Disable search input and button during search
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

  GTask *task = g_task_new(NULL, NULL, on_search_task_finished, widgets);
  g_task_set_task_data(task, g_strdup(pattern), g_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
}

static void
on_clear_button_clicked(GtkButton *button, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GtkListBox *listbox = widgets->listbox;

#if GTK_CHECK_VERSION(4, 10, 0)
  gtk_list_box_remove_all(listbox);
#else
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(listbox, 0)) {
    gtk_list_box_remove(listbox, GTK_WIDGET(row));
  }
#endif

  gtk_label_set_text(widgets->status_label, "Ready.");
  gtk_label_set_text(widgets->details_label, "");
}

// ------------------------------------------------------------
// When selecting a package, show detailed info
// ------------------------------------------------------------
static void
on_package_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  if (!row)
    return;

  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GtkWidget *child = gtk_list_box_row_get_child(row);
  const char *pkg_text = gtk_label_get_text(GTK_LABEL(child));

  std::string pkg_name = pkg_text;
  // Remove version suffix if present
  auto pos = pkg_name.find('-');
  if (pos != std::string::npos)
    pkg_name = pkg_name.substr(0, pos);

  gtk_label_set_text(widgets->status_label, "Fetching package info...");
  while (g_main_context_iteration(nullptr, false))
    ;

  std::string info = get_package_info(pkg_name);
  gtk_label_set_text(widgets->details_label, info.c_str());
  gtk_label_set_text(widgets->status_label, "Package info loaded.");
}

// ------------------------------------------------------------
// GTK app setup
// ------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer user_data)
{
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "DNF Package Viewer");
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 700);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox);

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

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox), spinner);

  GtkWidget *status_label = gtk_label_new("Ready.");
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_box_append(GTK_BOX(vbox), status_label);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(vbox), scrolled);

  GtkWidget *listbox = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), listbox);

  // --- New details area ---
  GtkWidget *details_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(details_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(details_label), PANGO_WRAP_WORD);
  gtk_box_append(GTK_BOX(vbox), details_label);

  // --- Struct setup ---
  SearchWidgets *widgets = g_new(SearchWidgets, 1);
  widgets->entry = GTK_ENTRY(entry);
  widgets->listbox = GTK_LIST_BOX(listbox);
  widgets->spinner = GTK_SPINNER(spinner);
  widgets->search_button = GTK_BUTTON(search_button);
  widgets->status_label = GTK_LABEL(status_label);
  widgets->details_label = GTK_LABEL(details_label);

  // --- Connect signals ---
  g_signal_connect(
      list_button, "clicked", G_CALLBACK(on_list_button_clicked), widgets);

  g_signal_connect(
      clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), widgets);

  g_signal_connect(
      search_button, "clicked", G_CALLBACK(on_search_button_clicked), widgets);

  g_signal_connect(
      listbox, "row-selected", G_CALLBACK(on_package_selected), widgets);

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
