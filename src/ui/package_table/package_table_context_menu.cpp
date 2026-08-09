// -----------------------------------------------------------------------------
// src/ui/package_table/package_table_context_menu.cpp
// Package table context menu helpers
// Keeps right-click package actions separate from package table construction.
// -----------------------------------------------------------------------------
#include "ui/package_table/package_table_context_menu.hpp"

#include "i18n.hpp"
#include "ui/transaction/pending_transaction_action_rows.hpp"
#include "ui/transaction/pending_transaction_controller.hpp"
#include "ui/common/widgets.hpp"

// -----------------------------------------------------------------------------
// Add one transaction action to the package context menu.
// -----------------------------------------------------------------------------
static void
append_context_menu_action(GtkBox *box,
                           const char *label,
                           gboolean sensitive,
                           GCallback callback,
                           MainWindowUiState *widgets)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  gtk_widget_set_sensitive(button, sensitive);
  g_signal_connect(button, "clicked", callback, widgets);
  gtk_box_append(box, button);
}

// -----------------------------------------------------------------------------
// Show right-click actions for one package table row.
// -----------------------------------------------------------------------------
void
package_table_show_context_menu(GtkWidget *anchor,
                                MainWindowUiState *widgets,
                                const PackageTableRow &row,
                                double x,
                                double y,
                                const std::function<bool(const std::string &)> &select_row)
{
  if (!anchor || !widgets) {
    return;
  }

  GtkWidget *view = gtk_widget_get_ancestor(anchor, GTK_TYPE_COLUMN_VIEW);
  if (!view || !GTK_IS_COLUMN_VIEW(view)) {
    return;
  }

  if (!select_row(row.row.nevra)) {
    return;
  }

  GtkWidget *popover = gtk_popover_new();
  gtk_widget_set_parent(popover, anchor);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  GdkRectangle rect = { static_cast<int>(x), static_cast<int>(y), 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_popover_set_child(GTK_POPOVER(popover), box);

  PendingTransactionSelectionActions actions =
      pending_transaction_actions_for_selection(row.row,
                                                row.upgrade_target(),
                                                row.upgrade_generation(),
                                                widgets->query_state.displayed_query,
                                                widgets->transaction_state.actions);

  // Match the main action buttons: install and upgrade use the available row,
  // while remove and reinstall use the installed row.
  // Keep the running app visible in the table, but block context-menu actions
  // that would modify the package currently owning this executable.
  const char *install_label = nullptr;
  if (actions.install_is_pending) {
    if (actions.rows.install_is_upgrade) {
      install_label = _("Unmark Upgrade");
    } else if (actions.rows.install_is_downgrade) {
      install_label = _("Unmark Downgrade");
    } else {
      install_label = _("Unmark Install");
    }
  } else {
    if (actions.rows.install_is_upgrade) {
      install_label = _("Mark for Upgrade");
    } else if (actions.rows.install_is_downgrade) {
      install_label = _("Mark for Downgrade");
    } else {
      install_label = _("Mark for Install");
    }
  }
  const char *remove_label = actions.remove_is_pending ? _("Unmark Removal") : _("Mark for Removal");
  const char *reinstall_label = actions.reinstall_is_pending ? _("Unmark Reinstall") : _("Mark for Reinstall");

  append_context_menu_action(GTK_BOX(box),
                             install_label,
                             actions.can_install,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_install_button_clicked(button, user_data);
                             }),
                             widgets);

  append_context_menu_action(GTK_BOX(box),
                             remove_label,
                             actions.can_remove,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_remove_button_clicked(button, user_data);
                             }),
                             widgets);

  append_context_menu_action(GTK_BOX(box),
                             reinstall_label,
                             actions.can_reinstall,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_reinstall_button_clicked(button, user_data);
                             }),
                             widgets);

  g_signal_connect(popover,
                   "closed",
                   G_CALLBACK(+[](GtkPopover *popover, gpointer) { gtk_widget_unparent(GTK_WIDGET(popover)); }),
                   nullptr);

  gtk_popover_popup(GTK_POPOVER(popover));
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
