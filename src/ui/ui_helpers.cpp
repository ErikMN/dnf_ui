// -----------------------------------------------------------------------------
// src/ui/ui_helpers.cpp
// Generic UI utility helpers
// Provides small shared helpers for status feedback and transaction action label
// updates used across the split widget controller modules.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"

#include "widgets.hpp"

namespace {

constexpr const char *ICON_BUTTON_IMAGE_KEY = "dnfui-icon-button-image";
constexpr const char *ICON_BUTTON_LABEL_KEY = "dnfui-icon-button-label";

} // namespace

GtkWidget *
ui_helpers_create_icon_button(const char *icon_name, const char *label)
{
  GtkWidget *button = gtk_button_new();
  ui_helpers_set_icon_button(GTK_BUTTON(button), icon_name, label);

  return button;
}

void
ui_helpers_set_icon_button(GtkButton *button, const char *icon_name, const char *label)
{
  if (!button) {
    return;
  }

  GtkWidget *image = GTK_WIDGET(g_object_get_data(G_OBJECT(button), ICON_BUTTON_IMAGE_KEY));
  GtkWidget *label_widget = GTK_WIDGET(g_object_get_data(G_OBJECT(button), ICON_BUTTON_LABEL_KEY));

  if (!image || !label_widget) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    image = gtk_image_new();
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), image);

    label_widget = gtk_label_new(nullptr);
    gtk_widget_set_valign(label_widget, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), label_widget);

    gtk_button_set_child(button, box);
    g_object_set_data(G_OBJECT(button), ICON_BUTTON_IMAGE_KEY, image);
    g_object_set_data(G_OBJECT(button), ICON_BUTTON_LABEL_KEY, label_widget);
  }

  const bool has_icon = icon_name && icon_name[0] != '\0';
  gtk_image_set_from_icon_name(GTK_IMAGE(image), has_icon ? icon_name : nullptr);
  gtk_widget_set_visible(image, has_icon);
  gtk_label_set_text(GTK_LABEL(label_widget), label ? label : "");
}

// -----------------------------------------------------------------------------
// Helper: Update status label with color
// -----------------------------------------------------------------------------
void
ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color)
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

  char *escaped = g_markup_escape_text(text.c_str(), -1);
  std::string markup = "<span background=\"" + bg + "\" foreground=\"black\">" + escaped + "</span>";
  g_free(escaped);

  gtk_label_set_markup(label, markup.c_str());
}

// -----------------------------------------------------------------------------
// Helper: Update transaction action button labels based on pending actions
// -----------------------------------------------------------------------------
void
ui_helpers_update_action_button_labels(SearchWidgets *widgets, const std::string &pkg)
{
  bool pending_install = false;
  bool pending_remove = false;
  bool pending_reinstall = false;

  for (const auto &a : widgets->transaction.actions) {
    if (a.nevra == pkg) {
      pending_install = (a.type == PendingAction::INSTALL);
      pending_remove = (a.type == PendingAction::REMOVE);
      pending_reinstall = (a.type == PendingAction::REINSTALL);
      break;
    }
  }

  if (pending_install) {
    ui_helpers_set_icon_button(widgets->transaction.install_button, "edit-clear-symbolic", "Unmark Install");
    ui_helpers_set_icon_button(widgets->transaction.remove_button, "list-remove-symbolic", "Mark for Removal");
    ui_helpers_set_icon_button(widgets->transaction.reinstall_button, "view-refresh-symbolic", "Mark for Reinstall");
  } else if (pending_reinstall) {
    ui_helpers_set_icon_button(widgets->transaction.install_button, "list-add-symbolic", "Mark for Install");
    ui_helpers_set_icon_button(widgets->transaction.remove_button, "list-remove-symbolic", "Mark for Removal");
    ui_helpers_set_icon_button(widgets->transaction.reinstall_button, "edit-clear-symbolic", "Unmark Reinstall");
  } else if (pending_remove) {
    ui_helpers_set_icon_button(widgets->transaction.install_button, "list-add-symbolic", "Mark for Install");
    ui_helpers_set_icon_button(widgets->transaction.remove_button, "edit-clear-symbolic", "Unmark Removal");
    ui_helpers_set_icon_button(widgets->transaction.reinstall_button, "view-refresh-symbolic", "Mark for Reinstall");
  } else {
    ui_helpers_set_icon_button(widgets->transaction.install_button, "list-add-symbolic", "Mark for Install");
    ui_helpers_set_icon_button(widgets->transaction.remove_button, "list-remove-symbolic", "Mark for Removal");
    ui_helpers_set_icon_button(widgets->transaction.reinstall_button, "view-refresh-symbolic", "Mark for Reinstall");
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
