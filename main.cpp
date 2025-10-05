// -----------------------------------------------------------------------------
// Graphical user interface for DNF
// Inspired by Synaptic
// Fast, reliable and easy to use
// -----------------------------------------------------------------------------
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
#include <mutex>
#include <glib.h>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
static std::set<std::string> g_installed_names;

// -----------------------------------------------------------------------------
// Config helpers for saving/restoring user settings
// -----------------------------------------------------------------------------
static std::map<std::string, std::string>
load_config_map()
{
  std::map<std::string, std::string> config;
  const char *home = g_get_home_dir();
  std::string config_path = std::string(home ? home : "") + "/.config/dnf_ui.conf";
  std::ifstream file(config_path);
  if (!file.good()) {
    return config;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    config[key] = value;
  }

  return config;
}

static void
save_config_map(const std::map<std::string, std::string> &config)
{
  const char *home = g_get_home_dir();
  std::string config_dir = std::string(home ? home : "") + "/.config";
  std::filesystem::create_directories(config_dir);
  std::ofstream file(config_dir + "/dnf_ui.conf");
  for (auto &[k, v] : config) {
    file << k << "=" << v << "\n";
  }
}

static int
load_paned_position()
{
  auto config = load_config_map();
  if (config.count("paned_position")) {
    return std::stoi(config["paned_position"]);
  }

  return 300;
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

// -----------------------------------------------------------------------------
// Helper: create fresh libdnf5::Base (thread-safe per-thread)
// -----------------------------------------------------------------------------
static std::unique_ptr<libdnf5::Base>
create_fresh_base()
{
  auto base = std::make_unique<libdnf5::Base>();
  base->load_config();
  base->setup();
  auto repo_sack = base->get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  repo_sack->load_repos();

  return base;
}

// -----------------------------------------------------------------------------
// Helper: Query installed packages via libdnf5
// -----------------------------------------------------------------------------
static std::vector<std::string>
get_installed_packages()
{
  std::vector<std::string> packages;

  auto base = create_fresh_base();

  libdnf5::rpm::PackageQuery query(*base);
  query.filter_installed();
  for (auto pkg : query) {
    g_installed_names.insert(pkg.get_name());
    packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: Search available packages by substring or exact match
// -----------------------------------------------------------------------------
static bool g_search_in_description = false;
static bool g_exact_match = false;

static std::vector<std::string>
search_available_packages(const std::string &pattern)
{
  std::vector<std::string> packages;

  auto base = create_fresh_base();

  libdnf5::rpm::PackageQuery query(*base);
  query.filter_available();

  if (g_search_in_description) {
    // Manually match pattern in description (case-insensitive)
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);

    for (auto pkg : query) {
      std::string desc = pkg.get_description();
      std::string name = pkg.get_name();

      std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      if (g_exact_match) {
        if (name == pattern_lower) {
          packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
        }
      } else {
        if (desc.find(pattern_lower) != std::string::npos || name.find(pattern_lower) != std::string::npos) {
          packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
        }
      }
    }
  } else {
    if (g_exact_match) {
      query.filter_name(pattern, libdnf5::sack::QueryCmp::EQ);
    } else {
      query.filter_name(pattern, libdnf5::sack::QueryCmp::CONTAINS);
    }

    for (auto pkg : query) {
      packages.push_back(pkg.get_name() + "-" + pkg.get_evr());
    }
  }

  return packages;
}

// -----------------------------------------------------------------------------
// Helper: get detailed info about a single package
// -----------------------------------------------------------------------------
static std::string
get_package_info(const std::string &pkg_name)
{
  auto base = create_fresh_base();

  libdnf5::rpm::PackageQuery query(*base);
  query.filter_name(pkg_name);

  if (query.empty()) {
    return "No details found for " + pkg_name;
  }

  // Prefer installed package if available
  libdnf5::rpm::PackageQuery installed(query);
  installed.filter_installed();

  libdnf5::rpm::PackageQuery best_candidate = installed.empty() ? query : installed;
  // choose latest version if multiple
  best_candidate.filter_latest_evr();

  auto pkg = *best_candidate.begin();

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

// -----------------------------------------------------------------------------
// Struct for UI state
// -----------------------------------------------------------------------------
struct SearchWidgets {
  GtkEntry *entry;
  GtkListBox *listbox;
  GtkScrolledWindow *list_scroller;
  GtkListBox *history_list;
  GtkSpinner *spinner;
  GtkButton *search_button;
  GtkLabel *status_label;
  GtkLabel *details_label;
  GtkLabel *count_label;
  GtkCheckButton *desc_checkbox;
  GtkCheckButton *exact_checkbox;
  std::vector<std::string> history;
  guint list_idle_id = 0;
};

// -----------------------------------------------------------------------------
// Global cache for search results
// -----------------------------------------------------------------------------
static std::map<std::string, std::vector<std::string>> g_search_cache;

static std::string
cache_key_for(const std::string &term)
{
  std::string key = (g_search_in_description ? "desc:" : "name:");
  key += (g_exact_match ? "exact:" : "contains:");
  key += term;
  return key;
}

// -----------------------------------------------------------------------------
// Helper: Update status label with color
// -----------------------------------------------------------------------------
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

  std::string markup = "<span background=\"" + bg + "\" foreground=\"black\">" + text + "</span>";
  gtk_label_set_markup(label, markup.c_str());
}

// -----------------------------------------------------------------------------
// Virtualized ListView population
// -----------------------------------------------------------------------------
static void
fill_listbox_async(SearchWidgets *widgets, const std::vector<std::string> &items, bool highlight_installed = true)
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

// -----------------------------------------------------------------------------
// Async: Installed packages (non-blocking)
// -----------------------------------------------------------------------------
static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    auto *results = new std::vector<std::string>(get_installed_packages());
    g_task_return_pointer(task, results, NULL);
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

static void
on_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GTask *task = G_TASK(res);
  std::vector<std::string> *packages = (std::vector<std::string> *)g_task_propagate_pointer(task, NULL);

  gtk_label_set_text(widgets->status_label, "");

  // Re-enable UI after async list finishes
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    fill_listbox_async(widgets, *packages, false);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
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
  // --- Refresh global installed package cache ---
  {
    g_installed_names.clear();
    auto base = create_fresh_base();
    libdnf5::rpm::PackageQuery query(*base);
    query.filter_installed();
    for (auto pkg : query) {
      g_installed_names.insert(pkg.get_name());
    }
  }
  GTask *task = g_task_new(NULL, NULL, on_list_task_finished, widgets);
  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Search
// -----------------------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *)
{
  const char *pattern = (const char *)task_data;
  try {
    auto *results = new std::vector<std::string>(search_available_packages(pattern));
    g_task_return_pointer(task, results, NULL);
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

static void
on_search_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GTask *task = G_TASK(res);
  std::vector<std::string> *packages = (std::vector<std::string> *)g_task_propagate_pointer(task, NULL);

  // Stop spinner
  gtk_spinner_stop(widgets->spinner);
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);

  // Re-enable UI
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    // Cache results
    const char *term = (const char *)g_task_get_task_data(task);
    if (term) {
      g_search_cache[cache_key_for(term)] = *packages;
    }
    fill_listbox_async(widgets, *packages, true);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    delete packages;
  } else {
    set_status(widgets->status_label, "Error or no results.", "red");
  }
}

// -----------------------------------------------------------------------------
// History, clear, etc.
// -----------------------------------------------------------------------------
static void
add_to_history(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }
  for (const auto &s : widgets->history)
    if (s == term) {
      return;
    }
  widgets->history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->history_list, row);
}

// Search button clicked
static void
perform_search(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

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
    fill_listbox_async(widgets, it->second, true);
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
  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->exact_checkbox));
  std::string pattern = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  if (pattern.empty()) {
    return;
  }

  add_to_history(widgets, pattern);
  perform_search(widgets, pattern);
}

static void
on_history_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer user_data)
{
  if (!row) {
    return;
  }

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
    while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->listbox, 0)) {
      gtk_list_box_remove(widgets->listbox, GTK_WIDGET(row));
    }
  } else if (widgets->list_scroller) {
    GtkStringList *empty = gtk_string_list_new(NULL);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(empty));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
    gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(lv));
  }
  gtk_label_set_text(widgets->count_label, "Items: 0");
  set_status(widgets->status_label, "Ready.", "gray");
  gtk_label_set_text(widgets->details_label, "");
}

// -----------------------------------------------------------------------------
// GTK app setup
// -----------------------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer)
{
  GtkWidget *window = gtk_application_window_new(app);
  // Preload installed package names for highlighting
  {
    auto base = create_fresh_base();
    libdnf5::rpm::PackageQuery query(*base);
    query.filter_installed();
    for (auto pkg : query) {
      g_installed_names.insert(pkg.get_name());
    }
  }
  gtk_window_set_title(GTK_WINDOW(window), "DNF Package Viewer");
  load_window_geometry(GTK_WINDOW(window));

  // Keyboard shortcuts: Ctrl+Q and Ctrl+W to close window
  GtkEventController *shortcuts = GTK_EVENT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_widget_add_controller(window, shortcuts);

  auto shortcut_callback = +[](GtkWidget *widget, GVariant *args, gpointer) -> gboolean {
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
  };

  // Ctrl+Q
  GtkShortcut *close_shortcut_q = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_q, GDK_CONTROL_MASK),
                                                   gtk_callback_action_new(shortcut_callback, NULL, NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), close_shortcut_q);

  // Ctrl+W
  GtkShortcut *close_shortcut_w = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_w, GDK_CONTROL_MASK),
                                                   gtk_callback_action_new(shortcut_callback, NULL, NULL));
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), close_shortcut_w);

  GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(window), vbox_root);

  GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_root), outer_paned);
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
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_history), history_list);

  // --- Search bar row ---
  GtkWidget *hbox_search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_search);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search available packages...");
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(hbox_search), entry);

  GtkWidget *search_button = gtk_button_new_with_label("Search");
  gtk_box_append(GTK_BOX(hbox_search), search_button);

  GtkWidget *desc_checkbox = gtk_check_button_new_with_label("Search in description");
  gtk_box_append(GTK_BOX(hbox_search), desc_checkbox);

  GtkWidget *exact_checkbox = gtk_check_button_new_with_label("Exact match");
  gtk_box_append(GTK_BOX(hbox_search), exact_checkbox);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_visible(spinner, FALSE);
  gtk_box_append(GTK_BOX(hbox_search), spinner);

  // --- Buttons row ---
  GtkWidget *hbox_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_append(GTK_BOX(vbox_main), hbox_buttons);

  GtkWidget *list_button = gtk_button_new_with_label("List Installed");
  gtk_box_append(GTK_BOX(hbox_buttons), list_button);

  GtkWidget *clear_button = gtk_button_new_with_label("Clear List");
  gtk_box_append(GTK_BOX(hbox_buttons), clear_button);

  GtkWidget *clear_cache_button = gtk_button_new_with_label("Clear Cache");
  gtk_box_append(GTK_BOX(hbox_buttons), clear_cache_button);
  g_signal_connect(
      clear_cache_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { g_search_cache.clear(); }), NULL);

  GtkWidget *status_label = gtk_label_new("Ready.");
  gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
  gtk_box_append(GTK_BOX(vbox_main), status_label);

  // --- Inner paned (packages | details) ---
  GtkWidget *inner_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(vbox_main), inner_paned);
  gtk_widget_set_vexpand(inner_paned, TRUE);
  gtk_widget_set_hexpand(inner_paned, TRUE);
  int pos = load_paned_position();
  if (pos < 100) {
    pos = 300;
  }
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
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_details), details_box);

  GtkWidget *details_label = gtk_label_new("Select a package for details.");
  gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(details_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(details_label), PANGO_WRAP_WORD);
  gtk_label_set_selectable(GTK_LABEL(details_label), TRUE);
  gtk_box_append(GTK_BOX(details_box), details_label);

  // --- Bottom bar with item count ---
  GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_hexpand(bottom_bar, TRUE);
  gtk_widget_add_css_class(bottom_bar, "bottom-bar");
  gtk_box_append(GTK_BOX(vbox_root), bottom_bar);

  GtkWidget *count_label = gtk_label_new("Items: 0");
  gtk_label_set_xalign(GTK_LABEL(count_label), 0.0);
  gtk_box_append(GTK_BOX(bottom_bar), count_label);

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
  widgets->exact_checkbox = GTK_CHECK_BUTTON(exact_checkbox);
  widgets->count_label = GTK_LABEL(count_label);

  // --- Modern GTK4 CSS for status bar background ---
  {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
                                      "label.status-bar { padding: 4px; border-radius: 4px; } "
                                      ".bottom-bar { padding: 5px; border-top: 1px solid #666; } "
                                      ".installed { "
                                      "  background-color: #b3f0b3; " /* softer green */
                                      "  color: black; "              /* readable text */
                                      "  padding: 2px 4px; "
                                      "  border-radius: 2px; "
                                      "}");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_widget_add_css_class(GTK_WIDGET(widgets->status_label), "status-bar");
    g_object_unref(css);
  }
  set_status(widgets->status_label, "Ready.", "gray");

  // --- Connect signals ---
  g_signal_connect(list_button, "clicked", G_CALLBACK(on_list_button_clicked), widgets);

  g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_button_clicked), widgets);

  g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), widgets);

  g_signal_connect(entry, "activate", G_CALLBACK(on_search_button_clicked), widgets);
  g_signal_connect(history_list, "row-selected", G_CALLBACK(on_history_row_selected), widgets);

  g_signal_connect(window,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer data) { delete static_cast<SearchWidgets *>(data); }),
                   widgets);

  g_signal_connect(window,
                   "close-request",
                   G_CALLBACK(+[](GtkWindow *w, gpointer user_data) -> gboolean {
                     save_window_geometry(w);
                     save_paned_position(GTK_PANED(user_data));
                     return FALSE;
                   }),
                   inner_paned);

  gtk_window_present(GTK_WINDOW(window));
}

// -----------------------------------------------------------------------------
// main()
// -----------------------------------------------------------------------------
int
main(int argc, char *argv[])
{
  GtkApplication *app = gtk_application_new("com.example.dnfgtk", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}
