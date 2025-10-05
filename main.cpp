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
#include <fstream>
#include <filesystem>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <gtk/gtk.h>

using namespace std;

// ------------------------------------------------------------
// Config helpers for saving/restoring user settings
// ------------------------------------------------------------
static std::map<std::string, std::string>
load_config_map()
{
  std::map<std::string, std::string> config;
  std::string config_path =
      std::string(getenv("HOME")) + "/.config/dnf_ui.conf";
  std::ifstream file(config_path);
  if (!file.good())
    return config;

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    auto pos = line.find('=');
    if (pos == std::string::npos)
      continue;
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    config[key] = value;
  }
  return config;
}

static void
save_config_map(const std::map<std::string, std::string> &config)
{
  std::string config_dir = std::string(getenv("HOME")) + "/.config";
  std::filesystem::create_directories(config_dir);
  std::ofstream file(config_dir + "/dnf_ui.conf");
  for (auto &[k, v] : config)
    file << k << "=" << v << "\n";
}

static int
load_paned_position()
{
  auto config = load_config_map();
  if (config.count("paned_position"))
    return std::stoi(config["paned_position"]);
  return 300; // default
}

static void
save_paned_position(GtkPaned *paned)
{
  auto config = load_config_map();
  config["paned_position"] = std::to_string(gtk_paned_get_position(paned));
  save_config_map(config);
}

static void
load_window_geometry(GtkWindow *window)
{
  auto config = load_config_map();
  int w = 900, h = 700;
  if (config.count("window_width"))
    w = std::stoi(config["window_width"]);
  if (config.count("window_height"))
    h = std::stoi(config["window_height"]);
  if (w < 600)
    w = 900;
  if (h < 400)
    h = 700;
  gtk_window_set_default_size(window, w, h);
}

static void
save_window_geometry(GtkWindow *window)
{
  auto config = load_config_map();
  int w = 900, h = 700;

#if GTK_CHECK_VERSION(4, 10, 0)
  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds(GTK_WIDGET(window), nullptr, &bounds)) {
    w = static_cast<int>(bounds.size.width);
    h = static_cast<int>(bounds.size.height);
  }
#else
  GtkAllocation alloc;
  gtk_widget_get_allocation(GTK_WIDGET(window), &alloc);
  w = alloc.width;
  h = alloc.height;
#endif

  config["window_width"] = std::to_string(w);
  config["window_height"] = std::to_string(h);
  save_config_map(config);
}

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
  for (auto pkg : query)
    packages.push_back(pkg.get_name() + "-" + pkg.get_evr());

  return packages;
}

// ------------------------------------------------------------
// Helper: Search available packages by substring
// ------------------------------------------------------------
static bool g_search_in_description = false;

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

  if (g_search_in_description) {
    // We’ll manually match pattern in description (case-insensitive)
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(),
                   pattern_lower.end(),
                   pattern_lower.begin(),
                   ::tolower);

    for (auto pkg : query) {
      std::string desc = pkg.get_description();
      std::string name = pkg.get_name();

      std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      if (desc.find(pattern_lower) != std::string::npos ||
          name.find(pattern_lower) != std::string::npos)
        packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
    }
  } else {
    query.filter_name(pattern, libdnf5::sack::QueryCmp::CONTAINS);
    for (auto pkg : query)
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
  if (query.empty())
    return "No details found for " + pkg_name;

  auto pkg = *query.begin();
  std::ostringstream oss;
  oss << "Name: " << pkg.get_name() << "\n"
      << "Version: " << pkg.get_version() << "\n"
      << "Release: " << pkg.get_release() << "\n"
      << "Arch: " << pkg.get_arch() << "\n"
      << "Repo: " << pkg.get_repo_id() << "\n\n"
      << "Summary:\n"
      << pkg.get_summary() << "\n\n"
      << "Description:\n"
      << pkg.get_description();

  return oss.str();
}

// ------------------------------------------------------------
// Struct for UI state
// ------------------------------------------------------------
struct SearchWidgets {
  GtkEntry *entry;
  GtkListBox *listbox;
  GtkScrolledWindow *list_scroller;
  GtkListBox *history_list;
  GtkSpinner *spinner;
  GtkButton *search_button;
  GtkLabel *status_label;
  GtkLabel *details_label;
  GtkCheckButton *desc_checkbox;
  std::vector<std::string> history;
  guint list_idle_id = 0;
};

// ------------------------------------------------------------
// Global cache for search results
// ------------------------------------------------------------
static std::map<std::string, std::vector<std::string>> g_search_cache;

static std::string
cache_key_for(const std::string &term)
{
  return (g_search_in_description ? "desc:" : "name:") + term;
}

// ------------------------------------------------------------
// Helper: Update status label with color
// ------------------------------------------------------------
static void
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

  std::string markup = "<span background=\"" + bg + "\" foreground=\"black\">" +
      text + "</span>";
  gtk_label_set_markup(label, markup.c_str());
}

// ------------------------------------------------------------
// Virtualized ListView population
// ------------------------------------------------------------
static void
fill_listbox_async(SearchWidgets *widgets,
                   const std::vector<std::string> &items)
{
  GtkStringList *store = gtk_string_list_new(NULL);
  for (const auto &pkg : items)
    gtk_string_list_append(store, pkg.c_str());

  GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(store));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  g_signal_connect(
      factory,
      "setup",
      G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
        GtkWidget *label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_list_item_set_child(item, label);
      }),
      nullptr);

  g_signal_connect(
      factory,
      "bind",
      G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
        GtkStringObject *sobj = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
        const char *text = gtk_string_object_get_string(sobj);
        GtkWidget *label = gtk_list_item_get_child(item);
        gtk_label_set_text(GTK_LABEL(label), text);
      }),
      nullptr);

  GtkListView *list_view =
      GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
  gtk_widget_set_hexpand(GTK_WIDGET(list_view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(list_view), TRUE);
  gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(list_view));
  widgets->listbox = nullptr;

  g_signal_connect(
      sel,
      "selection-changed",
      G_CALLBACK(
          +[](GtkSingleSelection *self, guint, guint, gpointer user_data) {
            SearchWidgets *widgets = (SearchWidgets *)user_data;
            guint index = gtk_single_selection_get_selected(self);
            if (index == GTK_INVALID_LIST_POSITION)
              return;

            GObject *obj = (GObject *)g_list_model_get_item(
                gtk_single_selection_get_model(self), index);
            const char *pkg_text =
                gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
            std::string pkg_name = pkg_text;
            g_object_unref(obj);

            auto pos = pkg_name.find('-');
            if (pos != std::string::npos)
              pkg_name = pkg_name.substr(0, pos);

            set_status(
                widgets->status_label, "Fetching package info...", "blue");
            while (g_main_context_iteration(nullptr, false))
              ;

            std::string info = get_package_info(pkg_name);
            gtk_label_set_text(widgets->details_label, info.c_str());
            set_status(widgets->status_label, "Package info loaded.", "green");
          }),
      widgets);
}

// ------------------------------------------------------------
// Async: Installed packages (non-blocking)
// ------------------------------------------------------------
static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    auto *results = new std::vector<std::string>(get_installed_packages());
    g_task_return_pointer(task, results, NULL);
  } catch (const std::exception &e) {
    g_task_return_error(
        task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

static void
on_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GTask *task = G_TASK(res);
  std::vector<std::string> *packages =
      (std::vector<std::string> *)g_task_propagate_pointer(task, NULL);

  gtk_label_set_text(widgets->status_label, "");

  // Re-enable UI after async list finishes
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    fill_listbox_async(widgets, *packages);
    char msg[256];
    snprintf(
        msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    gtk_label_set_text(widgets->details_label, "Select a package for details.");
    delete packages;
  } else {
    set_status(widgets->status_label, "Error listing packages.", "red");
  }
}


static void
on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  set_status(widgets->status_label, "Listing installed packages...", "blue");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

  GTask *task = g_task_new(NULL, NULL, on_list_task_finished, widgets);
  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
}

// ------------------------------------------------------------
// Search
// ------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *)
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

static void
on_search_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
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
    // Cache results
    const char *term = (const char *)g_task_get_task_data(task);
    if (term)
      g_search_cache[cache_key_for(term)] = *packages;

    fill_listbox_async(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    delete packages;
  } else {
    set_status(widgets->status_label, "Error or no results.", "red");
  }
}

// ------------------------------------------------------------
// History, clear, etc.
// ------------------------------------------------------------
static void
add_to_history(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty())
    return;
  for (const auto &s : widgets->history)
    if (s == term)
      return;
  widgets->history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->history_list, row);
}

// Search button clicked
static void
perform_search(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty())
    return;

  gtk_editable_set_text(GTK_EDITABLE(widgets->entry), term.c_str());
  set_status(widgets->status_label, "Searching for '" + term + "'...", "blue");
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), TRUE);
  gtk_spinner_start(widgets->spinner);

  // Disable search input and button during search
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

  // Check cache first
  auto it = g_search_cache.find(cache_key_for(term));
  if (it != g_search_cache.end()) {
    gtk_spinner_stop(widgets->spinner);
    gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);
    fill_listbox_async(widgets, it->second);
    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded %zu cached results.", it->second.size());
    set_status(widgets->status_label, msg, "gray");
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
    return;
  }

  // Otherwise, perform real search
  GTask *task = g_task_new(NULL, NULL, on_search_task_finished, widgets);
  g_task_set_task_data(task, g_strdup(term.c_str()), g_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
}

static void
on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  g_search_in_description =
      gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  std::string pattern = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  if (pattern.empty())
    return;

  add_to_history(widgets, pattern);
  perform_search(widgets, pattern);
}

static void
on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row)
    return;
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GtkWidget *child = gtk_list_box_row_get_child(row);
  const char *term = gtk_label_get_text(GTK_LABEL(child));
  perform_search(widgets, term);
}

static void
on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  if (widgets->listbox) {
    while (GtkListBoxRow *row =
               gtk_list_box_get_row_at_index(widgets->listbox, 0))
      gtk_list_box_remove(widgets->listbox, GTK_WIDGET(row));
  } else if (widgets->list_scroller) {
    GtkStringList *empty = gtk_string_list_new(NULL);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(empty));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    GtkListView *lv =
        GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
    gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(lv));
  }
  set_status(widgets->status_label, "Ready.", "gray");
  gtk_label_set_text(widgets->details_label, "");
}

// ------------------------------------------------------------
// GTK app setup
// ------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer)
{
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "DNF Package Viewer");
  load_window_geometry(GTK_WINDOW(window));

  GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(outer_paned), 200);

  GtkWidget *vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_end_child(GTK_PANED(outer_paned), vbox_main);

  GtkWidget *vbox_history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_vexpand(vbox_history, TRUE);
  gtk_widget_set_hexpand(vbox_history, TRUE);
  gtk_paned_set_start_child(GTK_PANED(outer_paned), vbox_history);

  GtkWidget *history_label = gtk_label_new("Search History");
  gtk_label_set_xalign(GTK_LABEL(history_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_history), history_label);

  GtkWidget *scrolled_history = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled_history, TRUE);
  gtk_widget_set_hexpand(scrolled_history, TRUE);
  gtk_box_append(GTK_BOX(vbox_history), scrolled_history);

  GtkWidget *history_list = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_history),
                                history_list);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox);

  GtkWidget *list_button = gtk_button_new_with_label("List Installed");
  gtk_box_append(GTK_BOX(hbox), list_button);

  GtkWidget *clear_button = gtk_button_new_with_label("Clear List");
  gtk_box_append(GTK_BOX(hbox), clear_button);

  GtkWidget *clear_cache_button = gtk_button_new_with_label("Clear Cache");
  gtk_box_append(GTK_BOX(hbox), clear_cache_button);
  g_signal_connect(
      clear_cache_button,
      "clicked",
      G_CALLBACK(+[](GtkButton *, gpointer) { g_search_cache.clear(); }),
      NULL);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                 "Search available packages...");
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox), entry);

  GtkWidget *search_button = gtk_button_new_with_label("Search");
  gtk_box_append(GTK_BOX(hbox), search_button);

  GtkWidget *desc_checkbox =
      gtk_check_button_new_with_label("Search in description");
  gtk_box_append(GTK_BOX(hbox), desc_checkbox);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox), spinner);

  GtkWidget *status_label = gtk_label_new("Ready.");
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_main), status_label);

  // --- Inner paned (packages | details) ---
  GtkWidget *inner_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_main), inner_paned);
  gtk_widget_set_vexpand(inner_paned, TRUE);
  gtk_widget_set_hexpand(inner_paned, TRUE);
  int pos = load_paned_position();
  if (pos < 100)
    pos = 300;
  gtk_paned_set_position(GTK_PANED(inner_paned), pos);

  // --- Left: package list ---
  GtkWidget *scrolled_list = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_list, TRUE);
  gtk_widget_set_vexpand(scrolled_list, TRUE);
  gtk_paned_set_start_child(GTK_PANED(inner_paned), scrolled_list);

  GtkWidget *listbox = gtk_list_box_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_list), listbox);

  // --- Right: details view ---
  GtkWidget *scrolled_details = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scrolled_details, TRUE);
  gtk_widget_set_vexpand(scrolled_details, TRUE);
  gtk_paned_set_end_child(GTK_PANED(inner_paned), scrolled_details);

  // container to keep label top-aligned
  GtkWidget *details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(details_box, GTK_ALIGN_START);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_details),
                                details_box);

  GtkWidget *details_label = gtk_label_new("Select a package for details.");
  gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(details_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(details_label), PANGO_WRAP_WORD);
  gtk_label_set_selectable(GTK_LABEL(details_label), TRUE);
  gtk_box_append(GTK_BOX(details_box), details_label);

  gtk_window_set_child(GTK_WINDOW(window), outer_paned);

  // --- Struct setup ---
  SearchWidgets *widgets = new SearchWidgets();
  widgets->entry = GTK_ENTRY(entry);
  widgets->listbox = GTK_LIST_BOX(listbox);
  widgets->list_scroller = GTK_SCROLLED_WINDOW(scrolled_list);
  widgets->history_list = GTK_LIST_BOX(history_list);
  widgets->spinner = GTK_SPINNER(spinner);
  widgets->search_button = GTK_BUTTON(search_button);
  widgets->status_label = GTK_LABEL(status_label);
  widgets->details_label = GTK_LABEL(details_label);
  widgets->desc_checkbox = GTK_CHECK_BUTTON(desc_checkbox);

  // --- Modern GTK4 CSS for status bar background ---
  {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        css, "label.status-bar { padding: 4px; border-radius: 4px; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_widget_add_css_class(GTK_WIDGET(widgets->status_label), "status-bar");
    g_object_unref(css);
  }
  set_status(widgets->status_label, "Ready.", "gray");

  // --- Connect signals ---
  g_signal_connect(
      list_button, "clicked", G_CALLBACK(on_list_button_clicked), widgets);

  g_signal_connect(
      clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), widgets);

  g_signal_connect(
      search_button, "clicked", G_CALLBACK(on_search_button_clicked), widgets);

  g_signal_connect(
      entry, "activate", G_CALLBACK(on_search_button_clicked), widgets);
  g_signal_connect(history_list,
                   "row-selected",
                   G_CALLBACK(on_history_row_selected),
                   widgets);

  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) {
                     delete static_cast<SearchWidgets *>(data);
                   }),
                   widgets);

  g_signal_connect(
      window,
      "close-request",
      G_CALLBACK(+[](GtkWindow *w, gpointer user_data) -> gboolean {
        save_window_geometry(w);
        save_paned_position(GTK_PANED(user_data));
        return FALSE;
      }),
      inner_paned);

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
