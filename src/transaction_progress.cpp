// -----------------------------------------------------------------------------
// src/transaction_progress.cpp
// Transaction progress popup and confirmation dialog helpers
// Keeps the apply-flow modal UI separate from the broader widget controller.
// -----------------------------------------------------------------------------
#include "transaction_progress.hpp"

#include "widgets.hpp"

#include <sstream>

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

struct SummaryDialogApplyData {
  SearchWidgets *widgets;
  TransactionApplyCallback on_apply;
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

static void
summary_dialog_apply_data_free(gpointer p)
{
  SummaryDialogApplyData *data = static_cast<SummaryDialogApplyData *>(p);
  delete data;
}

// -----------------------------------------------------------------------------
// Build the transaction popup used for streaming package install output
// -----------------------------------------------------------------------------
TransactionProgressWindow *
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
void
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
void
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
void
show_transaction_summary_dialog(SearchWidgets *widgets,
                                const TransactionPreview &preview,
                                TransactionApplyCallback on_apply)
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

  auto *apply_data = new SummaryDialogApplyData { widgets, on_apply };

  g_object_set_data_full(G_OBJECT(dialog), "summary-dialog-apply-data", apply_data, summary_dialog_apply_data_free);

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
                     SummaryDialogApplyData *data = static_cast<SummaryDialogApplyData *>(user_data);
                     SearchWidgets *widgets = data ? data->widgets : nullptr;
                     TransactionApplyCallback on_apply = data ? data->on_apply : nullptr;
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                     if (widgets && on_apply) {
                       on_apply(widgets);
                     }
                   }),
                   apply_data);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
