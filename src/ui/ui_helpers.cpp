// -----------------------------------------------------------------------------
// src/ui/ui_helpers.cpp
// Generic UI utility helpers
// Provides small shared helpers for status feedback and transaction action label
// updates used across the split widget controller modules.
// -----------------------------------------------------------------------------
#include "ui_helpers.hpp"

#include "widgets.hpp"

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
    gtk_button_set_label(widgets->transaction.install_button, "Unmark Install");
    gtk_button_set_label(widgets->transaction.remove_button, "Mark for Removal");
    gtk_button_set_label(widgets->transaction.reinstall_button, "Mark for Reinstall");
  } else if (pending_reinstall) {
    gtk_button_set_label(widgets->transaction.install_button, "Mark for Install");
    gtk_button_set_label(widgets->transaction.remove_button, "Mark for Removal");
    gtk_button_set_label(widgets->transaction.reinstall_button, "Unmark Reinstall");
  } else if (pending_remove) {
    gtk_button_set_label(widgets->transaction.install_button, "Mark for Install");
    gtk_button_set_label(widgets->transaction.remove_button, "Unmark Removal");
    gtk_button_set_label(widgets->transaction.reinstall_button, "Mark for Reinstall");
  } else {
    gtk_button_set_label(widgets->transaction.install_button, "Mark for Install");
    gtk_button_set_label(widgets->transaction.remove_button, "Mark for Removal");
    gtk_button_set_label(widgets->transaction.reinstall_button, "Mark for Reinstall");
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
