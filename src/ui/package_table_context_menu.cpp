// -----------------------------------------------------------------------------
// src/ui/package_table_context_menu.cpp
// Package table context menu helpers
// Keeps right-click package actions separate from package table construction.
// -----------------------------------------------------------------------------
#include "package_table_context_menu.hpp"

#include "pending_transaction_controller.hpp"
#include "ui/pending_transaction_state.hpp"
#include "ui/widgets.hpp"

// Find the pending action for the clicked package row, if one exists.
static bool
get_context_menu_pending_action(SearchWidgets *widgets, const std::string &nevra, PendingAction::Type &out_type)
{
  for (const auto &action : widgets->transaction.actions) {
    if (action.nevra == nevra) {
      out_type = action.type;
      return true;
    }
  }

  return false;
}

// Add one transaction action to the package context menu.
static void
append_context_menu_action(GtkBox *box,
                           const char *label,
                           gboolean sensitive,
                           GCallback callback,
                           SearchWidgets *widgets)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  gtk_widget_set_sensitive(button, sensitive);
  g_signal_connect(button, "clicked", callback, widgets);
  gtk_box_append(box, button);
}

void
package_table_show_context_menu(GtkWidget *anchor,
                                SearchWidgets *widgets,
                                const PackageRow &row,
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

  if (!select_row(row.nevra)) {
    return;
  }

  GtkWidget *popover = gtk_popover_new();
  gtk_widget_set_parent(popover, anchor);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

  GdkRectangle rect = { static_cast<int>(x), static_cast<int>(y), 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_popover_set_child(GTK_POPOVER(popover), box);

  // Match the main action buttons: remove/reinstall are only valid for the
  // exact installed NEVRA represented by this row.
  bool installed_exact = dnf_backend_is_package_installed_exact(row);
  // Keep the running app visible in the table, but block context-menu actions
  // that would modify the package currently owning this executable.
  bool self_protected = installed_exact && dnf_backend_is_package_self_protected(row);
  bool can_reinstall = installed_exact && !self_protected && dnf_backend_can_reinstall_package(row);

  PendingAction::Type pending_type;
  bool has_pending = get_context_menu_pending_action(widgets, row.nevra, pending_type);

  // Keep context menu actions aligned with the normal package action buttons.
  const char *install_label =
      has_pending && pending_type == PendingAction::INSTALL ? "Unmark Install" : "Mark for Install";
  const char *remove_label =
      has_pending && pending_type == PendingAction::REMOVE ? "Unmark Removal" : "Mark for Removal";
  const char *reinstall_label =
      has_pending && pending_type == PendingAction::REINSTALL ? "Unmark Reinstall" : "Mark for Reinstall";

  append_context_menu_action(GTK_BOX(box),
                             install_label,
                             !installed_exact,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_install_button_clicked(button, user_data);
                             }),
                             widgets);

  append_context_menu_action(GTK_BOX(box),
                             remove_label,
                             installed_exact && !self_protected,
                             G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                               if (GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
                                 gtk_popover_popdown(GTK_POPOVER(popover));
                               }
                               pending_transaction_on_remove_button_clicked(button, user_data);
                             }),
                             widgets);

  append_context_menu_action(GTK_BOX(box),
                             reinstall_label,
                             can_reinstall,
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
