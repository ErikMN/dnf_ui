// src/ui/pending_transaction_controller.hpp
// Public pending transaction controller entry points
//
// Owns the GTK callbacks for marking packages, clearing pending actions, and
// applying the prepared transaction through the transaction service.
#pragma once

#include <gtk/gtk.h>

void pending_transaction_on_install_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_remove_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_reinstall_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_apply_button_clicked(GtkButton *, gpointer user_data);
void pending_transaction_on_clear_pending_button_clicked(GtkButton *, gpointer user_data);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
