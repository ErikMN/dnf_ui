// -----------------------------------------------------------------------------
// src/ui/common/ui_helpers.cpp
// Shared UI helpers
// Creates icon buttons, updates status text, and formats common labels.
// -----------------------------------------------------------------------------
#include "ui/common/ui_helpers.hpp"

#include "i18n.hpp"

namespace {

constexpr const char *ICON_BUTTON_IMAGE_KEY = "dnfui-icon-button-image";
constexpr const char *ICON_BUTTON_LABEL_KEY = "dnfui-icon-button-label";

} // namespace

// -----------------------------------------------------------------------------
// Create a button containing an icon and label.
// -----------------------------------------------------------------------------
GtkWidget *
ui_helpers_create_icon_button(const char *icon_name, const char *label)
{
  GtkWidget *button = gtk_button_new();
  ui_helpers_set_icon_button(GTK_BUTTON(button), icon_name, label);

  return button;
}

// -----------------------------------------------------------------------------
// Update the icon and label shown in an existing action button.
// -----------------------------------------------------------------------------
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
// Update the status label with a background color.
// -----------------------------------------------------------------------------
void
ui_helpers_set_status(GtkLabel *label, const std::string &text, const std::string &color)
{
  std::string bg;
  if (color == "green")
    bg = "#e6f4ea";
  else if (color == "red")
    bg = "#fce8e6";
  else if (color == "blue")
    bg = "#e8f0fe";
  else if (color == "gray")
    bg = "#f3f4f6";
  else
    bg = "#ffffff";

  char *escaped = g_markup_escape_text(text.c_str(), -1);
  std::string markup = "<span background=\"" + bg + "\" foreground=\"black\">" + escaped + "</span>";
  g_free(escaped);

  gtk_label_set_markup(label, markup.c_str());
}

// -----------------------------------------------------------------------------
// Hide a timing label.
// -----------------------------------------------------------------------------
void
ui_helpers_clear_duration_label(GtkLabel *label)
{
  if (!label) {
    return;
  }

  gtk_label_set_text(label, "");
  gtk_widget_set_visible(GTK_WIDGET(label), FALSE);
}

// -----------------------------------------------------------------------------
// Show elapsed time in a timing label.
// -----------------------------------------------------------------------------
void
ui_helpers_show_duration_label(GtkLabel *label, const char *title, const char *fallback_title, gint64 started_at_us)
{
  if (!label || started_at_us <= 0) {
    return;
  }

  gint64 elapsed_us = g_get_monotonic_time() - started_at_us;
  if (elapsed_us < 0) {
    elapsed_us = 0;
  }

  const double elapsed_seconds = static_cast<double>(elapsed_us) / 1000000.0;
  const char *display_title = title;
  if (!display_title || display_title[0] == '\0') {
    display_title = fallback_title ? fallback_title : "";
  }
  std::string text = dnfui_i18n_format(_("%s: %.1f s"), display_title, elapsed_seconds);
  gtk_label_set_text(label, text.c_str());
  gtk_widget_set_visible(GTK_WIDGET(label), TRUE);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
