// -----------------------------------------------------------------------------
// src/widgets.cpp
// Signal callbacks and search logic
// Handles user-triggered actions (search, clear, history, etc.)
// and asynchronous DNF queries for package information.
// https://dnf5.readthedocs.io/en/latest/
// -----------------------------------------------------------------------------
#include "widgets.hpp"
#include "ui_helpers.hpp"
#include "dnf_backend.hpp"
#include "config.hpp"
#include "base_manager.hpp"

// Forward declarations
static void add_to_history(SearchWidgets *widgets, const std::string &term);
static void perform_search(SearchWidgets *widgets, const std::string &term);

// Global cache for previous search results
static std::map<std::string, std::vector<std::string>> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache

// -----------------------------------------------------------------------------
// Clear cached search results (called from Clear Cache button)
// -----------------------------------------------------------------------------
void
clear_search_cache()
{
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_search_cache.clear();
}

// -----------------------------------------------------------------------------
// Helper: Build a unique cache key based on search flags and term
// -----------------------------------------------------------------------------
static std::string
cache_key_for(const std::string &term)
{
  std::string key = (g_search_in_description ? "desc:" : "name:");
  key += (g_exact_match ? "exact:" : "contains:");
  key += term;

  return key;
}

// -----------------------------------------------------------------------------
// Async Operations
// GTK4 uses GTask for running expensive libdnf5 operations in worker threads
// without freezing the UI. These functions handle installed and available
// package queries off the main loop.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Async: Installed packages (non-blocking)
// Executes a background query using libdnf5 to fetch the list of installed
// packages. Runs in a worker thread via GTask to avoid blocking the GTK UI.
// Returns a std::vector<std::string> containing package names.
// -----------------------------------------------------------------------------
static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    // Query all installed packages
    auto *results = new std::vector<std::string>(get_installed_packages());
    g_task_return_pointer(task, results, NULL);
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Async completion handler: Installed package listing
// Runs on the GTK main thread after on_list_task() finishes. Retrieves the
// result vector from the GTask, repopulates the package list UI, and updates
// the status message accordingly.
// -----------------------------------------------------------------------------
static void
on_list_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GTask *task = G_TASK(res);
  std::vector<std::string> *packages = (std::vector<std::string> *)g_task_propagate_pointer(task, NULL);

  // Stop spinner
  gtk_spinner_stop(widgets->spinner);
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);

  gtk_label_set_text(widgets->status_label, "");

  // Re-enable UI after async list finishes
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

  if (packages) {
    // Populate list asynchronously and update status
    fill_listbox_async(widgets, *packages, true); // NOTE: mark all installed packages
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    gtk_label_set_text(widgets->details_label, "Select a package for details.");
    delete packages;
  } else {
    set_status(widgets->status_label, "Error listing packages.", "red");
  }
}

// -----------------------------------------------------------------------------
// Async: Search available packages (non-blocking)
// Executes a background libdnf5 query to find packages matching the search term.
// Runs off the GTK main thread via GTask to keep the UI responsive.
// Returns a std::vector<std::string> of matching package names.
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

// -----------------------------------------------------------------------------
// Called when background search finishes
// Updates UI, caches results, and repopulates the listbox
// -----------------------------------------------------------------------------
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
    // Cache results for faster re-display next time
    const char *term = (const char *)g_task_get_task_data(task);
    if (term) {
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      g_search_cache[cache_key_for(term)] = *packages;
    }

    refresh_installed_nevras();

    // Fill UI list and display result count
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
// UI callback: List Installed button
// Starts async listing of all installed packages
// -----------------------------------------------------------------------------
void
on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  set_status(widgets->status_label, "Listing installed packages...", "blue");

  // Show spinner
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), TRUE);
  gtk_spinner_start(widgets->spinner);

  // Disable search controls while loading
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), FALSE);

  // Run query asynchronously
  GTask *task = g_task_new(NULL, NULL, on_list_task_finished, widgets);
  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// UI callback: Search button (or pressing Enter in entry field)
// Reads options, caches query, and triggers background search
// -----------------------------------------------------------------------------
void
on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->exact_checkbox));
  std::string pattern = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  if (pattern.empty()) {
    return;
  }

  // Save search term and start lookup
  add_to_history(widgets, pattern);
  perform_search(widgets, pattern);
}

// -----------------------------------------------------------------------------
// UI callback: Selecting a search term from the history list
// -----------------------------------------------------------------------------
void
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

// -----------------------------------------------------------------------------
// UI callback: Clear List button
// Clears all displayed results, details and file info
// -----------------------------------------------------------------------------
void
on_clear_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;

  // Remove all listbox rows
  if (widgets->listbox) {
    while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->listbox, 0)) {
      gtk_list_box_remove(widgets->listbox, GTK_WIDGET(row));
    }
  }
  // Fallback: recreate empty scrolled window content if listbox unavailable
  else if (widgets->list_scroller) {
    GtkStringList *empty = gtk_string_list_new(NULL);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(empty));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory));
    gtk_scrolled_window_set_child(widgets->list_scroller, GTK_WIDGET(lv));
  }

  // Reset UI labels
  gtk_label_set_text(widgets->count_label, "Items: 0");
  set_status(widgets->status_label, "Ready.", "gray");
  gtk_label_set_text(widgets->details_label, "");
  gtk_label_set_text(widgets->files_label, "");
}

// -----------------------------------------------------------------------------
// Add new search term to search history if not already present
// -----------------------------------------------------------------------------
static void
add_to_history(SearchWidgets *widgets, const std::string &term)
{
  if (term.empty()) {
    return;
  }

  // Prevent duplicates in history
  for (const auto &s : widgets->history) {
    if (s == term) {
      return;
    }
  }

  // Append new term to internal list and UI widget
  widgets->history.push_back(term);
  GtkWidget *row = gtk_label_new(term.c_str());
  gtk_label_set_xalign(GTK_LABEL(row), 0.0);
  gtk_list_box_append(widgets->history_list, row);
}

// -----------------------------------------------------------------------------
// Perform search operation (cached or live)
// -----------------------------------------------------------------------------
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
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_search_cache.find(cache_key_for(term));
    if (it != g_search_cache.end()) {
      // Use cached results and skip background thread
      gtk_spinner_stop(widgets->spinner);
      gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);
      fill_listbox_async(widgets, it->second, true);

      char msg[256];
      snprintf(msg, sizeof(msg), "Loaded %zu cached results.", it->second.size());
      set_status(widgets->status_label, msg, "gray");

      // Re-enable search controls
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);

      return;
    }
  }

  // Otherwise perform real background search
  GTask *task = g_task_new(NULL, NULL, on_search_task_finished, widgets);
  g_task_set_task_data(task, g_strdup(term.c_str()), g_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Async: Refresh repositories (non-blocking)
// Runs BaseManager::rebuild() in a worker thread so GTK stays responsive
// -----------------------------------------------------------------------------
void
on_rebuild_task(GTask *task, gpointer, gpointer, GCancellable *)
{
  try {
    BaseManager::instance().rebuild();
    g_task_return_boolean(task, TRUE);
  } catch (const std::exception &e) {
    g_task_return_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, e.what()));
  }
}

// -----------------------------------------------------------------------------
// Async completion handler: Refresh repositories
// -----------------------------------------------------------------------------
void
on_rebuild_task_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GTask *task = G_TASK(res);
  GError *error = NULL;
  gboolean success = g_task_propagate_boolean(task, &error);

  if (success) {
    set_status(widgets->status_label, "Repositories refreshed.", "green");
  } else {
    set_status(widgets->status_label, error ? error->message : "Repo refresh failed.", "red");
    if (error)
      g_error_free(error);
  }

  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
}

// -----------------------------------------------------------------------------
// Async: Install selected package
// -----------------------------------------------------------------------------
void
on_install_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine selected package
  GtkListView *lv = GTK_LIST_VIEW(gtk_scrolled_window_get_child(widgets->list_scroller));
  if (!lv) {
    return;
  }

  GtkSelectionModel *model = gtk_list_view_get_model(lv);
  if (!model) {
    return;
  }

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  guint index = gtk_single_selection_get_selected(sel);
  if (index == GTK_INVALID_LIST_POSITION) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  GObject *obj = (GObject *)g_list_model_get_item(gtk_single_selection_get_model(sel), index);
  const char *pkg_name = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
  std::string pkg = pkg_name;
  g_object_unref(obj);

  set_status(widgets->status_label, "Installing " + pkg + "...", "blue");

  // Show spinner
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), TRUE);
  gtk_spinner_start(widgets->spinner);

  GTask *task = g_task_new(
      nullptr,
      nullptr,
      +[](GObject *, GAsyncResult *res, gpointer user_data) {
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        GTask *task = G_TASK(res);
        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Stop spinner
        gtk_spinner_stop(widgets->spinner);
        gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);

        if (success) {
          set_status(widgets->status_label, "Installation complete.", "green");

          // Force BaseManager to reload the system state
          BaseManager::instance().rebuild();
          refresh_installed_nevras();

          // Rebind current list items to update "installed" highlight
          GtkListView *lv = GTK_LIST_VIEW(gtk_scrolled_window_get_child(widgets->list_scroller));
          if (lv) {
            GtkSelectionModel *model = gtk_list_view_get_model(lv);
            if (GTK_IS_SINGLE_SELECTION(model)) {
              GtkStringList *store = GTK_STRING_LIST(gtk_single_selection_get_model(GTK_SINGLE_SELECTION(model)));
              std::vector<std::string> current_items;
              guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
              for (guint i = 0; i < n; ++i) {
                GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(store), i));
                const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
                current_items.emplace_back(text);
                g_object_unref(obj);
              }
              fill_listbox_async(widgets, current_items, true);
            }
          }
        } else {
          set_status(widgets->status_label, error ? error->message : "Installation failed.", "red");
          if (error) {
            g_error_free(error);
          }
        }
      },
      widgets);

  g_task_set_task_data(task, g_strdup(pkg.c_str()), g_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        const char *pkg = static_cast<const char *>(task_data);
        std::string err;
        bool ok = install_packages({ pkg }, err);
        if (ok) {
          g_task_return_boolean(t, TRUE);
        } else {
          g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err.c_str()));
        }
      });

  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Async: Remove selected package
// -----------------------------------------------------------------------------
void
on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  GtkListView *lv = GTK_LIST_VIEW(gtk_scrolled_window_get_child(widgets->list_scroller));
  if (!lv) {
    return;
  }

  GtkSelectionModel *model = gtk_list_view_get_model(lv);
  if (!model) {
    return;
  }

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  guint index = gtk_single_selection_get_selected(sel);
  if (index == GTK_INVALID_LIST_POSITION) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  GObject *obj = (GObject *)g_list_model_get_item(gtk_single_selection_get_model(sel), index);
  const char *pkg_name = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
  std::string pkg = pkg_name;
  g_object_unref(obj);

  set_status(widgets->status_label, "Removing " + pkg + "...", "blue");

  // Show spinner
  gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), TRUE);
  gtk_spinner_start(widgets->spinner);

  GTask *task = g_task_new(
      nullptr,
      nullptr,
      +[](GObject *, GAsyncResult *res, gpointer user_data) {
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        GTask *task = G_TASK(res);
        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Stop spinner
        gtk_spinner_stop(widgets->spinner);
        gtk_widget_set_visible(GTK_WIDGET(widgets->spinner), FALSE);

        if (success) {
          set_status(widgets->status_label, "Removal complete.", "green");

          // Force BaseManager to reload the system state
          BaseManager::instance().rebuild();
          refresh_installed_nevras();

          // Rebind current list items to update "installed" highlight
          GtkListView *lv = GTK_LIST_VIEW(gtk_scrolled_window_get_child(widgets->list_scroller));
          if (lv) {
            GtkSelectionModel *model = gtk_list_view_get_model(lv);
            if (GTK_IS_SINGLE_SELECTION(model)) {
              GtkStringList *store = GTK_STRING_LIST(gtk_single_selection_get_model(GTK_SINGLE_SELECTION(model)));
              std::vector<std::string> current_items;
              guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
              for (guint i = 0; i < n; ++i) {
                GObject *obj = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(store), i));
                const char *text = gtk_string_object_get_string(GTK_STRING_OBJECT(obj));
                current_items.emplace_back(text);
                g_object_unref(obj);
              }
              fill_listbox_async(widgets, current_items, true);
            }
          }
        } else {
          set_status(widgets->status_label, error ? error->message : "Removal failed.", "red");
          if (error) {
            g_error_free(error);
          }
        }
      },
      widgets);

  g_task_set_task_data(task, g_strdup(pkg.c_str()), g_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        const char *pkg = static_cast<const char *>(task_data);
        std::string err;
        bool ok = remove_packages({ pkg }, err);
        if (ok) {
          g_task_return_boolean(t, TRUE);
        } else {
          g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err.c_str()));
        }
      });

  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
