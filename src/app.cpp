// -----------------------------------------------------------------------------
// src/app.cpp
// GTK Application setup and activation
// -----------------------------------------------------------------------------
#include "app.hpp"

#include "base_manager.hpp"
#include "debug_trace.hpp"
#include "dnf_backend/dnf_backend.hpp"
#include "ui/main_window.hpp"
#include "ui/widgets.hpp"

#include <gtk/gtk.h>

// -----------------------------------------------------------------------------
// Function forward declarations
// -----------------------------------------------------------------------------
static void activate(GtkApplication *app, gpointer user_data);
static void setup_periodic_tasks(void);
static void startup_warmup_data_free(gpointer data);
static gboolean start_backend_warmup_idle(gpointer user_data);
static void start_backend_warmup_task(SearchWidgets *widgets);
static void on_backend_warmup_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void on_backend_warmup_task_finished(GObject *source_object, GAsyncResult *result, gpointer user_data);

struct StartupWarmupData {
  SearchWidgets *widgets = nullptr;
  GCancellable *startup_cancellable = nullptr;
};

// -----------------------------------------------------------------------------
// Run GTK application and return process exit status
// -----------------------------------------------------------------------------
int
app_run_dnfui(int argc, char **argv)
{
  GtkApplication *app = gtk_application_new("com.fedora.dnfui", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}

// -----------------------------------------------------------------------------
// Setup periodic background tasks
// -----------------------------------------------------------------------------
static void
setup_periodic_tasks(void)
{
  // --- Periodic refresh of installed package names every 5 minutes ---
  g_timeout_add_seconds(
      300, // 5 minutes
      [](gpointer) -> gboolean {
        dnf_backend_refresh_installed_nevras();
        return TRUE; // keep repeating
      },
      nullptr);
}

// -----------------------------------------------------------------------------
// Start backend warm up after the first window show is out of the way.
// -----------------------------------------------------------------------------
static void
startup_warmup_data_free(gpointer data)
{
  StartupWarmupData *warmup = static_cast<StartupWarmupData *>(data);
  if (!warmup) {
    return;
  }
  if (warmup->startup_cancellable) {
    g_object_unref(warmup->startup_cancellable);
  }
  delete warmup;
}

static gboolean
start_backend_warmup_idle(gpointer user_data)
{
  StartupWarmupData *warmup = static_cast<StartupWarmupData *>(user_data);
  if (!warmup || !warmup->widgets || !warmup->startup_cancellable ||
      g_cancellable_is_cancelled(warmup->startup_cancellable)) {
    return G_SOURCE_REMOVE;
  }
  start_backend_warmup_task(warmup->widgets);

  return G_SOURCE_REMOVE;
}

// -----------------------------------------------------------------------------
// Start a quiet background task that warms up the shared DNF base.
// -----------------------------------------------------------------------------
static void
start_backend_warmup_task(SearchWidgets *widgets)
{
  if (!widgets || !widgets->window_state.backend_warmup_label) {
    return;
  }

  DNFUI_TRACE("Backend warm up task start");
  gtk_widget_set_visible(GTK_WIDGET(widgets->window_state.backend_warmup_label), TRUE);

  widgets->window_state.backend_warmup_cancellable = g_cancellable_new();

  GTask *task = g_task_new(G_OBJECT(widgets->window_state.backend_warmup_label),
                           widgets->window_state.backend_warmup_cancellable,
                           on_backend_warmup_task_finished,
                           nullptr);
  g_task_run_in_thread(task, on_backend_warmup_task);
  g_object_unref(task);
}

// -----------------------------------------------------------------------------
// Warm up BaseManager in the background so the first package query is faster.
// -----------------------------------------------------------------------------
static void
on_backend_warmup_task(GTask *task, gpointer, gpointer, GCancellable *cancellable)
{
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Backend warm up was cancelled.");
    return;
  }

  try {
    BaseManager::instance().acquire_read();
    if (g_cancellable_is_cancelled(cancellable)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Backend warm up was cancelled.");
      return;
    }
    g_task_return_boolean(task, TRUE);
  } catch (const std::exception &e) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", e.what());
  } catch (...) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Backend warm up failed.");
  }
}

// -----------------------------------------------------------------------------
// Ignore warm up errors so startup stays quiet and normal queries handle them.
// -----------------------------------------------------------------------------
static void
on_backend_warmup_task_finished(GObject *source_object, GAsyncResult *result, gpointer)
{
  GtkLabel *label = GTK_LABEL(source_object);
  GError *error = nullptr;
  g_task_propagate_boolean(G_TASK(result), &error);

  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    DNFUI_TRACE("Backend warm up task cancelled");
    g_clear_error(&error);
    return;
  }

  if (error) {
    DNFUI_TRACE("Backend warm up task failed: %s", error->message);
  } else {
    DNFUI_TRACE("Backend warm up task done");
  }

  g_clear_error(&error);

  if (label) {
    gtk_widget_set_visible(GTK_WIDGET(label), FALSE);
  }
}

// -----------------------------------------------------------------------------
// GTK app setup (start here)
// -----------------------------------------------------------------------------
static void
activate(GtkApplication *app, gpointer)
{
  MainWindow main_window = main_window_create(app);

  setup_periodic_tasks();

  // Show the fully initialized window
  gtk_window_present(GTK_WINDOW(main_window.window));

  // Warm up the shared backend after the window is on screen
  StartupWarmupData *warmup = new StartupWarmupData();
  warmup->widgets = main_window.widgets;
  warmup->startup_cancellable = G_CANCELLABLE(g_object_ref(main_window.startup_cancellable));
  g_idle_add_full(G_PRIORITY_LOW, start_backend_warmup_idle, warmup, startup_warmup_data_free);
  g_object_unref(main_window.startup_cancellable);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
