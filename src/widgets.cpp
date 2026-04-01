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

#include <sstream>

// Forward declarations
static void add_to_history(SearchWidgets *widgets, const std::string &term);
static void perform_search(SearchWidgets *widgets, const std::string &term);
static void refresh_current_package_view(SearchWidgets *widgets);
static void cancel_active_package_list_request(SearchWidgets *widgets);
static void start_apply_transaction(SearchWidgets *widgets);

// Global cache for previous search results
static std::map<std::string, std::vector<PackageRow>> g_search_cache;
static std::mutex g_cache_mutex; // Protects g_search_cache

// -----------------------------------------------------------------------------
// Task payload & cancellable helpers (snapshot cache key; cancel on widget destroy)
// -----------------------------------------------------------------------------
struct SearchTaskData {
  char *term;
  char *cache_key;
  uint64_t request_id;
  // Snapshot of BaseManager generation at dispatch time.
  // Used to drop stale results if the backend Base is rebuilt while this task runs.
  uint64_t generation;
};

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

static GCancellable *
make_task_cancellable_for(GtkWidget *w)
{
  GCancellable *c = g_cancellable_new();
  if (w) {
    g_signal_connect_object(w, "destroy", G_CALLBACK(g_cancellable_cancel), c, G_CONNECT_SWAPPED);
  }
  return c;
}

struct ApplyTaskData {
  std::vector<std::string> install;
  std::vector<std::string> remove;
  std::vector<std::string> reinstall;
  struct TransactionProgressWindow *progress_window;
};

static void
apply_task_data_free(gpointer p)
{
  ApplyTaskData *d = static_cast<ApplyTaskData *>(p);
  delete d;
}

// Split the pending queue into install, remove, and reinstall transaction specs.
static void
build_pending_transaction_specs(const SearchWidgets *widgets,
                                std::vector<std::string> &install,
                                std::vector<std::string> &remove,
                                std::vector<std::string> &reinstall)
{
  install.clear();
  remove.clear();
  reinstall.clear();

  if (!widgets) {
    return;
  }

  install.reserve(widgets->pending.size());
  remove.reserve(widgets->pending.size());
  reinstall.reserve(widgets->pending.size());

  for (const auto &action : widgets->pending) {
    if (action.type == PendingAction::INSTALL) {
      install.push_back(action.nevra);
    } else if (action.type == PendingAction::REINSTALL) {
      reinstall.push_back(action.nevra);
    } else {
      remove.push_back(action.nevra);
    }
  }
}

// Format the resolved disk-space change for the transaction summary dialog.
static std::string
format_transaction_space_change(long long delta_bytes)
{
  if (delta_bytes == 0) {
    return "Disk space usage will be unchanged.";
  }

  unsigned long long abs_bytes =
      delta_bytes > 0 ? static_cast<unsigned long long>(delta_bytes) : static_cast<unsigned long long>(-delta_bytes);
  char *formatted = g_format_size(abs_bytes);
  std::string line;

  if (delta_bytes > 0) {
    line = std::string(formatted) + " extra disk space will be used.";
  } else {
    line = std::string(formatted) + " of disk space will be freed.";
  }

  g_free(formatted);
  return line;
}

// -----------------------------------------------------------------------------
// FIXME: HACK: Scroll position helpers
// -----------------------------------------------------------------------------

// Saved scroll position used to restore the package list viewport after a refresh.
struct ScrollRestoreData {
  GtkAdjustment *hadj;
  GtkAdjustment *vadj;
  double hvalue;
  double vvalue;
};

// Release the adjustment references kept in the saved scroll-position snapshot.
static void
scroll_restore_data_free(gpointer p)
{
  ScrollRestoreData *d = static_cast<ScrollRestoreData *>(p);
  if (!d) {
    return;
  }

  if (d->hadj) {
    g_object_unref(d->hadj);
  }
  if (d->vadj) {
    g_object_unref(d->vadj);
  }

  delete d;
}

// Restore the saved scroll position once the refreshed view is back in place.
static gboolean
restore_scroll_position_idle(gpointer user_data)
{
  ScrollRestoreData *d = static_cast<ScrollRestoreData *>(user_data);
  if (!d) {
    return G_SOURCE_REMOVE;
  }

  if (d->hadj) {
    gtk_adjustment_set_value(d->hadj, d->hvalue);
  }
  if (d->vadj) {
    gtk_adjustment_set_value(d->vadj, d->vvalue);
  }

  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Transaction progress popup state
// -----------------------------------------------------------------------------
struct TransactionProgressWindow {
  GtkWindow *window;
  GtkLabel *title_label;
  GtkLabel *stage_label;
  GtkTextBuffer *buffer;
  GtkTextView *view;
  GtkSpinner *spinner;
  GtkButton *close_button;
  bool finished;
};

struct ProgressAppendData {
  GtkLabel *stage_label;
  GtkTextBuffer *buffer;
  GtkTextView *view;
  char *message;
};

static void
progress_append_data_free(ProgressAppendData *data)
{
  if (!data) {
    return;
  }

  g_object_unref(data->stage_label);
  g_object_unref(data->buffer);
  g_object_unref(data->view);
  g_free(data->message);
  delete data;
}

// -----------------------------------------------------------------------------
// Build the transaction popup used for streaming package install output
// -----------------------------------------------------------------------------
static TransactionProgressWindow *
create_transaction_progress_window(SearchWidgets *widgets, size_t pending_count)
{
  auto *progress = new TransactionProgressWindow();
  progress->finished = false;

  progress->window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(progress->window, "Transaction Progress");
  gtk_window_set_default_size(progress->window, 760, 420);
  gtk_window_set_modal(progress->window, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(progress->window, app);
    }
    gtk_window_set_transient_for(progress->window, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(progress->window, outer);

  progress->title_label = GTK_LABEL(gtk_label_new(nullptr));
  gtk_label_set_markup(progress->title_label,
                       ("<b>Applying " + std::to_string(pending_count) + " pending package change" +
                        (pending_count == 1 ? "</b>" : "s</b>"))
                           .c_str());
  gtk_label_set_xalign(progress->title_label, 0.0f);
  gtk_box_append(GTK_BOX(outer), GTK_WIDGET(progress->title_label));

  GtkWidget *stage_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(outer), stage_box);

  progress->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_spinner_start(progress->spinner);
  gtk_box_append(GTK_BOX(stage_box), GTK_WIDGET(progress->spinner));

  progress->stage_label = GTK_LABEL(gtk_label_new("Resolving dependency changes..."));
  gtk_label_set_xalign(progress->stage_label, 0.0f);
  gtk_widget_set_hexpand(GTK_WIDGET(progress->stage_label), TRUE);
  gtk_box_append(GTK_BOX(stage_box), GTK_WIDGET(progress->stage_label));

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  progress->view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(progress->view, FALSE);
  gtk_text_view_set_cursor_visible(progress->view, FALSE);
  gtk_text_view_set_monospace(progress->view, TRUE);
  gtk_text_view_set_wrap_mode(progress->view, GTK_WRAP_WORD_CHAR);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(progress->view));
  progress->buffer = gtk_text_view_get_buffer(progress->view);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  progress->close_button = GTK_BUTTON(gtk_button_new_with_label("Close"));
  gtk_widget_set_sensitive(GTK_WIDGET(progress->close_button), FALSE);
  gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(progress->close_button));

  g_signal_connect(progress->close_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) {
                     auto *progress = static_cast<TransactionProgressWindow *>(user_data);
                     gtk_window_destroy(progress->window);
                   }),
                   progress);

  g_signal_connect(progress->window,
                   "close-request",
                   G_CALLBACK(+[](GtkWindow *, gpointer user_data) -> gboolean {
                     auto *progress = static_cast<TransactionProgressWindow *>(user_data);
                     return progress->finished ? FALSE : TRUE;
                   }),
                   progress);

  g_signal_connect(
      progress->window,
      "destroy",
      G_CALLBACK(+[](GtkWidget *, gpointer user_data) { delete static_cast<TransactionProgressWindow *>(user_data); }),
      progress);

  gtk_window_present(progress->window);

  return progress;
}

// -----------------------------------------------------------------------------
// Queue one transaction log line onto the GTK main loop
// -----------------------------------------------------------------------------
static void
append_transaction_progress_line(TransactionProgressWindow *progress, const std::string &message)
{
  if (!progress || message.empty()) {
    return;
  }

  auto *data = new ProgressAppendData();
  data->stage_label = GTK_LABEL(g_object_ref(progress->stage_label));
  data->buffer = GTK_TEXT_BUFFER(g_object_ref(progress->buffer));
  data->view = GTK_TEXT_VIEW(g_object_ref(progress->view));
  data->message = g_strdup(message.c_str());

  g_main_context_invoke(
      nullptr,
      +[](gpointer user_data) -> gboolean {
        auto *data = static_cast<ProgressAppendData *>(user_data);

        gtk_label_set_text(data->stage_label, data->message);

        GtkTextIter end;
        gtk_text_buffer_get_end_iter(data->buffer, &end);
        gtk_text_buffer_insert(data->buffer, &end, data->message, -1);
        gtk_text_buffer_insert(data->buffer, &end, "\n", 1);

        gtk_text_buffer_get_end_iter(data->buffer, &end);
        GtkTextMark *mark = gtk_text_buffer_create_mark(data->buffer, nullptr, &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(data->view, mark);
        gtk_text_buffer_delete_mark(data->buffer, mark);

        progress_append_data_free(data);
        return G_SOURCE_REMOVE;
      },
      data);
}

// -----------------------------------------------------------------------------
// Queue one or more transaction log lines onto the GTK main loop
// -----------------------------------------------------------------------------
static void
append_transaction_progress(TransactionProgressWindow *progress, const std::string &message)
{
  if (!progress || message.empty()) {
    return;
  }

  std::istringstream stream(message);
  std::string line;

  while (std::getline(stream, line)) {
    if (!line.empty()) {
      append_transaction_progress_line(progress, line);
    }
  }
}

// -----------------------------------------------------------------------------
// Update the popup when the package transaction finishes
// -----------------------------------------------------------------------------
static void
finish_transaction_progress(TransactionProgressWindow *progress, bool success, const std::string &summary)
{
  if (!progress) {
    return;
  }

  if (!summary.empty()) {
    append_transaction_progress(progress, summary);
  }

  progress->finished = true;
  gtk_spinner_stop(progress->spinner);
  gtk_widget_set_visible(GTK_WIDGET(progress->spinner), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(progress->close_button), TRUE);
  gtk_label_set_text(progress->stage_label,
                     success ? "Transaction finished successfully." : "Transaction finished with errors.");
  gtk_window_set_title(progress->window, success ? "Transaction Complete" : "Transaction Failed");
}

// Append one resolved transaction section to the confirmation dialog.
static void
append_transaction_summary_section(GtkBox *parent, const char *title, const std::vector<std::string> &items)
{
  if (!parent || !title || items.empty()) {
    return;
  }

  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(parent, section);

  GtkWidget *heading = gtk_label_new(nullptr);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(heading), markup);
  g_free(markup);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(section), heading);

  for (const auto &item : items) {
    GtkWidget *label = gtk_label_new(item.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(section), label);
  }
}

// Show the final confirmation dialog before starting the package transaction.
static void
show_transaction_summary_dialog(SearchWidgets *widgets, const TransactionPreview &preview)
{
  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, "Summary");
  gtk_window_set_default_size(dialog, 760, 520);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(dialog, app);
    }
    gtk_window_set_transient_for(dialog, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *title = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(title), "<b>Summary</b>");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *question = gtk_label_new("Apply the following changes?");
  gtk_label_set_xalign(GTK_LABEL(question), 0.0f);
  gtk_box_append(GTK_BOX(outer), question);

  GtkWidget *intro = gtk_label_new(
      "This is your last opportunity to look through the list of marked changes before they are applied.");
  gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
  gtk_box_append(GTK_BOX(outer), intro);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(contents, 6);
  gtk_widget_set_margin_end(contents, 6);
  gtk_widget_set_margin_top(contents, 6);
  gtk_widget_set_margin_bottom(contents, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), contents);

  // Show the resolved backend changes, not only the packages the user marked manually.
  append_transaction_summary_section(GTK_BOX(contents), "To be installed", preview.install);
  append_transaction_summary_section(GTK_BOX(contents), "To be upgraded", preview.upgrade);
  append_transaction_summary_section(GTK_BOX(contents), "To be downgraded", preview.downgrade);
  append_transaction_summary_section(GTK_BOX(contents), "To be reinstalled", preview.reinstall);
  append_transaction_summary_section(GTK_BOX(contents), "To be removed", preview.remove);

  GtkWidget *summary_heading = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(summary_heading), "<b>Summary</b>");
  gtk_label_set_xalign(GTK_LABEL(summary_heading), 0.0f);
  gtk_box_append(GTK_BOX(outer), summary_heading);

  GtkWidget *summary_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(GTK_BOX(outer), summary_box);

  auto append_summary_line = [&](const std::string &line) {
    GtkWidget *label = gtk_label_new(line.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(summary_box), label);
  };

  auto append_count_line = [&](size_t count, const char *verb) {
    if (count == 0) {
      return;
    }

    char line[256];
    snprintf(line, sizeof(line), "%zu package%s will be %s.", count, count == 1 ? "" : "s", verb);
    append_summary_line(line);
  };

  append_count_line(preview.install.size(), "installed");
  append_count_line(preview.upgrade.size(), "upgraded");
  append_count_line(preview.downgrade.size(), "downgraded");
  append_count_line(preview.reinstall.size(), "reinstalled");
  append_count_line(preview.remove.size(), "removed");
  append_summary_line(format_transaction_space_change(preview.disk_space_delta));

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
  gtk_box_append(GTK_BOX(button_box), cancel_button);

  GtkWidget *apply_button = gtk_button_new_with_label("Apply");
  gtk_widget_add_css_class(apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(button_box), apply_button);

  g_signal_connect(cancel_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  g_signal_connect(apply_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                     start_apply_transaction(widgets);
                   }),
                   widgets);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Update Apply button enabled state based on pending actions
// -----------------------------------------------------------------------------
static void
update_apply_button(SearchWidgets *widgets)
{
  if (!widgets || !widgets->apply_button) {
    return;
  }

  size_t pending_count = widgets->pending.size();
  bool has_pending = pending_count > 0;
  std::string apply_label = "Apply Transactions";
  if (has_pending) {
    apply_label += " (" + std::to_string(pending_count) + ")";
  }

  gtk_button_set_label(widgets->apply_button, apply_label.c_str());
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->apply_button), has_pending);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->clear_pending_button), has_pending);
}

// -----------------------------------------------------------------------------
// Refresh Pending Actions tab
// -----------------------------------------------------------------------------
static void
refresh_pending_tab(SearchWidgets *widgets)
{
  // Clear existing rows
  while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(widgets->pending_list, 0)) {
    gtk_list_box_remove(widgets->pending_list, GTK_WIDGET(row));
  }

  // Re-add actions
  for (const auto &a : widgets->pending) {
    std::string prefix;
    switch (a.type) {
    case PendingAction::INSTALL:
      prefix = "Install: ";
      break;
    case PendingAction::REINSTALL:
      prefix = "Reinstall: ";
      break;
    case PendingAction::REMOVE:
      prefix = "Remove: ";
      break;
    }
    std::string line = prefix + a.nevra;
    GtkWidget *row = gtk_label_new(line.c_str());
    gtk_label_set_xalign(GTK_LABEL(row), 0.0);
    gtk_list_box_append(widgets->pending_list, row);
  }
  update_apply_button(widgets);
}

// -----------------------------------------------------------------------------
// Remove a pending action
// -----------------------------------------------------------------------------
static bool
remove_pending_action(SearchWidgets *widgets, const std::string &nevra)
{
  for (size_t i = 0; i < widgets->pending.size(); ++i) {
    if (widgets->pending[i].nevra == nevra) {
      widgets->pending.erase(widgets->pending.begin() + i);
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Find pending action type for a package
// -----------------------------------------------------------------------------
static bool
get_pending_action_type(SearchWidgets *widgets, const std::string &nevra, PendingAction::Type &out_type)
{
  for (const auto &a : widgets->pending) {
    if (a.nevra == nevra) {
      out_type = a.type;
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Spinner ref-count helpers (prevents one task from hiding spinner used by another)
// -----------------------------------------------------------------------------
static GQuark
spinner_quark()
{
  static GQuark q = 0;
  if (G_UNLIKELY(q == 0)) {
    q = g_quark_from_static_string("dnfui-spinner-count");
  }

  return q;
}

static void
spinner_acquire(GtkSpinner *spinner)
{
  if (!spinner) {
    return;
  }

  GQuark q = spinner_quark();
  int count = GPOINTER_TO_INT(g_object_get_qdata(G_OBJECT(spinner), q));
  count++;
  g_object_set_qdata(G_OBJECT(spinner), q, GINT_TO_POINTER(count));

  if (count == 1) {
    gtk_widget_set_visible(GTK_WIDGET(spinner), TRUE);
    gtk_spinner_start(spinner);
  }
}

static void
spinner_release(GtkSpinner *spinner)
{
  if (!spinner) {
    return;
  }

  GQuark q = spinner_quark();
  int count = GPOINTER_TO_INT(g_object_get_qdata(G_OBJECT(spinner), q));
  if (count > 0) {
    count--;
    g_object_set_qdata(G_OBJECT(spinner), q, GINT_TO_POINTER(count));
  }

  if (count == 0) {
    gtk_spinner_stop(spinner);
    gtk_widget_set_visible(GTK_WIDGET(spinner), FALSE);
    g_object_set_qdata(G_OBJECT(spinner), q, nullptr);
  }
}

// Return true when the shared package-list request state currently owns a running task.
static bool
has_active_package_list_request(const SearchWidgets *widgets)
{
  return widgets && widgets->package_list_cancellable && !g_cancellable_is_cancelled(widgets->package_list_cancellable);
}

// Return the button that owns the Stop state for the active package list request.
static GtkButton *
package_list_stop_button(SearchWidgets *widgets, PackageListRequestKind kind)
{
  if (!widgets) {
    return nullptr;
  }

  return kind == PackageListRequestKind::LIST_INSTALLED ? widgets->list_button : widgets->search_button;
}

// Human-readable cancel status for the current background package list request.
static const char *
package_list_cancelled_status(PackageListRequestKind kind)
{
  switch (kind) {
  case PackageListRequestKind::SEARCH:
    return "Search cancelled.";
  case PackageListRequestKind::LIST_INSTALLED:
    return "Listing installed packages cancelled.";
  case PackageListRequestKind::NONE:
  default:
    return "Operation cancelled.";
  }
}

// Track the active background package list request and switch the owning button to Stop.
static void
begin_package_list_request(SearchWidgets *widgets, GCancellable *c, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || !c) {
    return;
  }

  if (widgets->package_list_cancellable) {
    g_object_unref(widgets->package_list_cancellable);
  }

  widgets->package_list_cancellable = G_CANCELLABLE(g_object_ref(c));
  widgets->current_package_list_request_id = request_id;
  widgets->current_package_list_request_kind = kind;
  GtkButton *stop_button = package_list_stop_button(widgets, kind);
  GtkButton *other_button =
      kind == PackageListRequestKind::LIST_INSTALLED ? widgets->search_button : widgets->list_button;

  gtk_button_set_label(widgets->search_button, "Search");
  gtk_button_set_label(widgets->list_button, "List Installed");
  gtk_button_set_label(stop_button, "Stop");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->desc_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->exact_checkbox), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->history_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(other_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(stop_button), TRUE);
}

// Restore the normal search and list controls after a package query stops or finishes.
static void
restore_package_list_controls(SearchWidgets *widgets)
{
  if (!widgets) {
    return;
  }

  gtk_button_set_label(widgets->search_button, "Search");
  gtk_button_set_label(widgets->list_button, "List Installed");
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->entry), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->desc_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->exact_checkbox), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->history_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->list_button), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(widgets->search_button), TRUE);
}

// Restore the shared package list UI when the active background request is done.
static void
end_package_list_request(SearchWidgets *widgets, uint64_t request_id, PackageListRequestKind kind)
{
  if (!widgets || widgets->current_package_list_request_id != request_id ||
      widgets->current_package_list_request_kind != kind) {
    return;
  }

  if (widgets->package_list_cancellable) {
    g_object_unref(widgets->package_list_cancellable);
    widgets->package_list_cancellable = nullptr;
  }
  widgets->current_package_list_request_id = 0;
  widgets->current_package_list_request_kind = PackageListRequestKind::NONE;
  restore_package_list_controls(widgets);
}

// Cancel the active package list request and immediately unlock the shared controls.
static void
cancel_active_package_list_request(SearchWidgets *widgets)
{
  if (!widgets || !widgets->package_list_cancellable) {
    return;
  }

  PackageListRequestKind kind = widgets->current_package_list_request_kind;
  GCancellable *c = widgets->package_list_cancellable;
  if (!g_cancellable_is_cancelled(c)) {
    g_cancellable_cancel(c);
  }

  // Release only the spinner slot owned by this request so other running tasks
  // can keep their progress indication visible.
  spinner_release(widgets->spinner);
  restore_package_list_controls(widgets);
  set_status(widgets->status_label, package_list_cancelled_status(kind), "gray");
}

// -----------------------------------------------------------------------------
// FIXME: HACK: Refresh the visible package rows after pending-action state changes
// Rebuilds the current package list presentation so status badges stay in sync
// with the pending transaction state.
// -----------------------------------------------------------------------------
static void
refresh_current_package_view(SearchWidgets *widgets)
{
  ScrollRestoreData *scroll = new ScrollRestoreData { nullptr, nullptr, 0.0, 0.0 };

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(widgets->list_scroller);
  if (hadj) {
    scroll->hadj = GTK_ADJUSTMENT(g_object_ref(hadj));
    scroll->hvalue = gtk_adjustment_get_value(hadj);
  }

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(widgets->list_scroller);
  if (vadj) {
    scroll->vadj = GTK_ADJUSTMENT(g_object_ref(vadj));
    scroll->vvalue = gtk_adjustment_get_value(vadj);
  }

  fill_package_view(widgets, widgets->current_packages);

  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, restore_scroll_position_idle, scroll, scroll_restore_data_free);
}

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
  std::string key = (g_search_in_description.load() ? "desc:" : "name:");
  key += (g_exact_match.load() ? "exact:" : "contains:");
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
// Returns a std::vector<PackageRow> containing structured package metadata.
// -----------------------------------------------------------------------------

// Task data for list-installed operation.
// We snapshot the BaseManager generation at dispatch time so the UI can ignore
// results produced against an older Base after a rebuild or transaction.
// request_id keeps the active Stop button state matched to the task that
// currently owns it.
struct ListTaskData {
  uint64_t request_id;
  uint64_t generation;
};

static void
on_list_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  try {
    // Query all installed packages
    auto *results = new std::vector<PackageRow>(get_installed_package_rows_interruptible(cancellable));
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
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
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  const ListTaskData *td = static_cast<const ListTaskData *>(g_task_get_task_data(task));

  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      if (td) {
        end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
      }
      return;
    }
  }

  // Drop stale results if the backend Base changed while the worker was running.
  // This prevents rendering a list that no longer matches the current repo/system state.
  if (td && td->generation != BaseManager::instance().current_generation()) {
    spinner_release(widgets->spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = (std::vector<PackageRow> *)g_task_propagate_pointer(task, &error);

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  }

  if (packages) {
    // Populate the package table and update status
    widgets->selected_nevra.clear();
    fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu installed packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    gtk_label_set_text(widgets->details_label, "Select a package for details.");
    gtk_label_set_text(widgets->files_label, "Select an installed package to view its file list.");
    gtk_label_set_text(widgets->deps_label, "Select a package to view dependencies.");
    gtk_label_set_text(widgets->changelog_label, "Select a package to view its changelog.");
    delete packages;
  } else {
    set_status(widgets->status_label, error ? error->message : "Error listing packages.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Async: Search available packages (non-blocking)
// Executes a background libdnf5 query to find packages matching the search term.
// Runs off the GTK main thread via GTask to keep the UI responsive.
// Returns a std::vector<PackageRow> of matching package metadata.
// -----------------------------------------------------------------------------
static void
on_search_task(GTask *task, gpointer, gpointer task_data, GCancellable *cancellable)
{
  const SearchTaskData *td = static_cast<const SearchTaskData *>(task_data);
  const char *pattern = td ? td->term : "";
  try {
    auto *results = new std::vector<PackageRow>(search_available_package_rows_interruptible(pattern, cancellable));
    // Ensure results are freed if never propagated (stale/cancel path).
    g_task_return_pointer(task, results, [](gpointer p) { delete static_cast<std::vector<PackageRow> *>(p); });
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
  GTask *task = G_TASK(res);
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  GCancellable *c = g_task_get_cancellable(task);
  const SearchTaskData *td = static_cast<const SearchTaskData *>(g_task_get_task_data(task));

  if (c && g_cancellable_is_cancelled(c)) {
    if (td) {
      end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    }
    return;
  }

  if (td && td->generation != BaseManager::instance().current_generation()) {
    spinner_release(widgets->spinner);
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
    return;
  }

  GError *error = nullptr;
  std::vector<PackageRow> *packages = (std::vector<PackageRow> *)g_task_propagate_pointer(task, &error);

  // Stop spinner (ref-counted)
  spinner_release(widgets->spinner);

  if (td) {
    end_package_list_request(widgets, td->request_id, PackageListRequestKind::SEARCH);
  }

  if (packages) {
    // Cache results for faster re-display next time (use dispatch-time key)
    if (td && td->cache_key) {
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      g_search_cache[td->cache_key] = *packages;
    }

    refresh_installed_nevras();

    // Fill the package table and display result count
    widgets->selected_nevra.clear();
    fill_package_view(widgets, *packages);
    char msg[256];
    snprintf(msg, sizeof(msg), "Found %zu packages.", packages->size());
    set_status(widgets->status_label, msg, "green");
    delete packages;
  } else {
    set_status(widgets->status_label, error ? error->message : "Error or no results.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// UI callback: List Installed button
// Starts async listing of all installed packages
// The same button changes to Stop while the worker task is running.
// -----------------------------------------------------------------------------
void
on_list_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  if (has_active_package_list_request(widgets)) {
    if (widgets->current_package_list_request_kind == PackageListRequestKind::LIST_INSTALLED) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  set_status(widgets->status_label, "Listing installed packages...", "blue");

  // Show spinner (ref-counted)
  spinner_acquire(widgets->spinner);

  // Run query asynchronously
  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  // Store generation snapshot so completion can reject stale results.
  ListTaskData *td = new ListTaskData;
  td->request_id = widgets->next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  // The shared request helper owns disabling the entry and flipping the
  // initiating button from List Installed to Stop.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::LIST_INSTALLED);
  GTask *task = g_task_new(nullptr, c, on_list_task_finished, widgets);
  g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<ListTaskData *>(p); });

  g_task_run_in_thread(task, on_list_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// UI callback: Search button (or pressing Enter in entry field)
// Reads options, caches query, and triggers background search
// The same button acts as Stop while a search worker task is running.
// -----------------------------------------------------------------------------
void
on_search_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = (SearchWidgets *)user_data;
  if (has_active_package_list_request(widgets)) {
    if (widgets->current_package_list_request_kind == PackageListRequestKind::SEARCH) {
      cancel_active_package_list_request(widgets);
    }
    return;
  }

  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->exact_checkbox));

  const char *txt = gtk_editable_get_text(GTK_EDITABLE(widgets->entry));
  std::string pattern = txt ? txt : "";

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
  widgets->current_packages.clear();
  widgets->selected_nevra.clear();
  fill_package_view(widgets, {});

  // Reset UI labels and actions
  set_status(widgets->status_label, "Ready.", "gray");
  gtk_label_set_text(widgets->details_label, "Select a package for details.");
  gtk_label_set_text(widgets->files_label, "Select an installed package to view its file list.");
  gtk_label_set_text(widgets->deps_label, "Select a package to view dependencies.");
  gtk_label_set_text(widgets->changelog_label, "Select a package to view its changelog.");
  update_action_button_labels(widgets, "");
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

  // Ensure cache key reflects current checkboxes even when triggered from history
  g_search_in_description = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->desc_checkbox));
  g_exact_match = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets->exact_checkbox));

  gtk_editable_set_text(GTK_EDITABLE(widgets->entry), term.c_str());
  set_status(widgets->status_label, ("Searching for '" + term + "'...").c_str(), "blue");
  widgets->selected_nevra.clear();

  // Check cache first
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_search_cache.find(cache_key_for(term));
    if (it != g_search_cache.end()) {
      // Use cached results and skip background thread
      fill_package_view(widgets, it->second);

      char msg[256];
      snprintf(msg, sizeof(msg), "Loaded %zu cached results.", it->second.size());
      set_status(widgets->status_label, msg, "gray");

      return;
    }
  }

  // Otherwise perform real background search
  spinner_acquire(widgets->spinner);

  const std::string key = cache_key_for(term);
  SearchTaskData *td = static_cast<SearchTaskData *>(g_malloc0(sizeof *td));
  td->term = g_strdup(term.c_str());
  td->cache_key = g_strdup(key.c_str());
  td->request_id = widgets->next_package_list_request_id++;
  td->generation = BaseManager::instance().current_generation();

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  // The shared request helper owns disabling the search controls and flipping
  // the initiating Search button to Stop.
  begin_package_list_request(widgets, c, td->request_id, PackageListRequestKind::SEARCH);
  GTask *task = g_task_new(nullptr, c, on_search_task_finished, widgets);
  g_task_set_task_data(task, td, search_task_data_free);
  g_task_run_in_thread(task, on_search_task);
  g_object_unref(task);
  g_object_unref(c);
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
  GTask *task = G_TASK(res);
  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      return;
    }
  }
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GError *error = nullptr;
  gboolean success = g_task_propagate_boolean(task, &error);

  if (success) {
    set_status(widgets->status_label, "Repositories refreshed.", "green");
  } else {
    set_status(widgets->status_label, error ? error->message : "Repo refresh failed.", "red");
    if (error) {
      g_error_free(error);
    }
  }
}

// -----------------------------------------------------------------------------
// Helper: Rebuild base asynchronously and refresh installed highlights afterwards
// -----------------------------------------------------------------------------
static void
rebuild_after_tx_finished(GObject *, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(res);
  if (GCancellable *c = g_task_get_cancellable(task)) {
    if (g_cancellable_is_cancelled(c)) {
      return;
    }
  }
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
  GError *error = nullptr;
  gboolean ok = g_task_propagate_boolean(task, &error);

  if (!ok && error) {
    set_status(widgets->status_label, error->message, "red");
    g_error_free(error);
    return;
  }

  // Refresh installed state and rebind the current package rows.
  refresh_installed_nevras();

  if (!widgets->current_packages.empty()) {
    refresh_current_package_view(widgets);
  }
}

static void
rebuild_after_tx_async(SearchWidgets *widgets)
{
  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  GTask *task = g_task_new(nullptr, c, rebuild_after_tx_finished, widgets);
  g_task_run_in_thread(task, on_rebuild_task);
  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Async: Install selected package
// -----------------------------------------------------------------------------
void
on_install_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!get_selected_package_row(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending install
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::INSTALL) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    // If it was pending REMOVE (or anything else), replace it with INSTALL
    remove_pending_action(widgets, pkg.nevra);
    widgets->pending.push_back({ PendingAction::INSTALL, pkg.nevra });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for install: " + pkg.name).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg.nevra);

  // Refresh the package table to apply pending-state badges.
  refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Async: Remove selected package
// -----------------------------------------------------------------------------
void
on_remove_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  // Determine the selected package from the current package table.
  PackageRow pkg;
  if (!get_selected_package_row(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  // Toggle pending remove
  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::REMOVE) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    // If it was pending INSTALL (or anything else), replace it with REMOVE
    remove_pending_action(widgets, pkg.nevra);
    widgets->pending.push_back({ PendingAction::REMOVE, pkg.nevra });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for removal: " + pkg.name).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg.nevra);

  // Refresh the package table to apply pending-state badges.
  refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Async: Reinstall selected package
// -----------------------------------------------------------------------------
void
on_reinstall_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  PackageRow pkg;
  if (!get_selected_package_row(widgets, pkg)) {
    set_status(widgets->status_label, "No package selected.", "gray");
    return;
  }

  PendingAction::Type existing_type;
  bool has_existing = get_pending_action_type(widgets, pkg.nevra, existing_type);
  if (has_existing && existing_type == PendingAction::REINSTALL) {
    remove_pending_action(widgets, pkg.nevra);
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Unmarked: " + pkg.name).c_str(), "gray");
  } else {
    remove_pending_action(widgets, pkg.nevra);
    widgets->pending.push_back({ PendingAction::REINSTALL, pkg.nevra });
    refresh_pending_tab(widgets);
    set_status(widgets->status_label, ("Marked for reinstall: " + pkg.name).c_str(), "blue");
  }
  update_action_button_labels(widgets, pkg.nevra);

  refresh_current_package_view(widgets);
}

// -----------------------------------------------------------------------------
// Clears all pending install, remove, and reinstall actions without applying them
// -----------------------------------------------------------------------------
void
on_clear_pending_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets->pending.empty()) {
    set_status(widgets->status_label, "No pending actions to clear.", "blue");
    return;
  }

  size_t count = widgets->pending.size();
  widgets->pending.clear();
  refresh_pending_tab(widgets);

  // Refresh the package table to remove pending-state badges.
  refresh_current_package_view(widgets);

  char msg[256];
  snprintf(msg, sizeof(msg), "Cleared %zu pending action%s.", count, count == 1 ? "" : "s");
  set_status(widgets->status_label, msg, "green");
}

// -----------------------------------------------------------------------------
// Start the async apply flow after the user confirms the transaction summary.
static void
start_apply_transaction(SearchWidgets *widgets)
{
  ApplyTaskData *td = new ApplyTaskData;
  build_pending_transaction_specs(widgets, td->install, td->remove, td->reinstall);
  td->progress_window = create_transaction_progress_window(widgets, widgets->pending.size());

  append_transaction_progress(td->progress_window, "Queued transaction request.");
  set_status(widgets->status_label, "Applying pending changes. See transaction window for details.", "blue");
  spinner_acquire(widgets->spinner);

  GCancellable *c = make_task_cancellable_for(GTK_WIDGET(widgets->entry));
  GTask *task = g_task_new(
      nullptr,
      c,
      +[](GObject *, GAsyncResult *res, gpointer user_data) {
        GTask *task = G_TASK(res);
        ApplyTaskData *td = static_cast<ApplyTaskData *>(g_task_get_task_data(task));
        if (GCancellable *c = g_task_get_cancellable(task)) {
          if (g_cancellable_is_cancelled(c)) {
            return;
          }
        }
        SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);
        GError *error = nullptr;
        gboolean success = g_task_propagate_boolean(task, &error);

        // Stop spinner (ref-counted)
        spinner_release(widgets->spinner);

        finish_transaction_progress(td ? td->progress_window : nullptr, success, "");

        if (success) {
          // Clear pending queue and refresh tab
          widgets->pending.clear();
          refresh_pending_tab(widgets);

          set_status(widgets->status_label, "Transaction successful.", "green");

          // Rebuild base and refresh installed highlighting asynchronously
          rebuild_after_tx_async(widgets);
        } else {
          set_status(widgets->status_label, error ? error->message : "Transaction failed.", "red");
          if (error) {
            g_error_free(error);
          }
        }
      },
      widgets);

  g_task_set_task_data(task, td, apply_task_data_free);

  g_task_run_in_thread(
      task, +[](GTask *t, gpointer, gpointer task_data, GCancellable *) {
        ApplyTaskData *td = static_cast<ApplyTaskData *>(task_data);
        std::string err;
        bool ok = apply_transaction(td->install, td->remove, td->reinstall, err, [td](const std::string &message) {
          append_transaction_progress(td->progress_window, message);
        });
        if (ok) {
          g_task_return_boolean(t, TRUE);
        } else {
          g_task_return_error(t, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err.c_str()));
        }
      });

  g_object_unref(task);
  g_object_unref(c);
}

// -----------------------------------------------------------------------------
// Apply pending actions in a single libdnf5 transaction (async via backend)
// -----------------------------------------------------------------------------
void
on_apply_button_clicked(GtkButton *, gpointer user_data)
{
  SearchWidgets *widgets = static_cast<SearchWidgets *>(user_data);

  if (widgets->pending.empty()) {
    set_status(widgets->status_label, "No pending changes.", "gray");
    return;
  }

  std::vector<std::string> install;
  std::vector<std::string> remove;
  std::vector<std::string> reinstall;
  build_pending_transaction_specs(widgets, install, remove, reinstall);

  // Resolve the full transaction first so the summary includes dependency-driven changes too.
  TransactionPreview preview;
  std::string error;
  if (!preview_transaction(install, remove, reinstall, preview, error)) {
    set_status(widgets->status_label, error.empty() ? "Unable to prepare transaction preview." : error.c_str(), "red");
    return;
  }

  show_transaction_summary_dialog(widgets, preview);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
