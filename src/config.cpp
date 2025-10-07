// -----------------------------------------------------------------------------
// Config helpers for saving/restoring user settings
// Handles persistent UI state (window size, pane divider positions, etc.)
// The configuration is stored as key=value pairs in:
//   ~/.config/dnf_ui.conf
//
// TODO: FIXME: Consider switching to GKeyFile or JSON for more structured data
// -----------------------------------------------------------------------------
#include "config.hpp"
#include <fstream>
#include <filesystem>

#include <glib.h>

// -----------------------------------------------------------------------------
// Config helpers for saving/restoring user settings
// -----------------------------------------------------------------------------
std::map<std::string, std::string>
load_config_map()
{
  std::map<std::string, std::string> config;
  const char *home = g_get_home_dir();
  std::string config_path = std::string(home ? home : "") + "/.config/dnf_ui.conf";
  std::ifstream file(config_path);
  if (!file.good()) {
    return config;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    config[key] = value;
  }

  return config;
}

void
save_config_map(const std::map<std::string, std::string> &config)
{
  const char *home = g_get_home_dir();
  std::string config_dir = std::string(home ? home : "") + "/.config";
  std::filesystem::create_directories(config_dir);
  std::ofstream file(config_dir + "/dnf_ui.conf");
  for (auto &[k, v] : config) {
    file << k << "=" << v << "\n";
  }
}

int
load_paned_position()
{
  auto config = load_config_map();
  if (config.count("paned_position")) {
    return std::stoi(config["paned_position"]);
  }

  return 300;
}

void
save_paned_position(GtkPaned *paned)
{
  auto config = load_config_map();
  config["paned_position"] = std::to_string(gtk_paned_get_position(paned));
  save_config_map(config);
}

void
load_window_geometry(GtkWindow *window)
{
  auto config = load_config_map();
  int w = 900, h = 700;
  if (config.count("window_width"))
    w = std::stoi(config["window_width"]);
  if (config.count("window_height"))
    h = std::stoi(config["window_height"]);
  if (w < 600)
    w = 900;
  if (h < 400)
    h = 700;
  gtk_window_set_default_size(window, w, h);
}

void
save_window_geometry(GtkWindow *window)
{
  auto config = load_config_map();
  int w = 900, h = 700;

#if GTK_CHECK_VERSION(4, 10, 0)
  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds(GTK_WIDGET(window), nullptr, &bounds)) {
    w = static_cast<int>(bounds.size.width);
    h = static_cast<int>(bounds.size.height);
  }
#else
  GtkAllocation alloc;
  gtk_widget_get_allocation(GTK_WIDGET(window), &alloc);
  w = alloc.width;
  h = alloc.height;
#endif

  config["window_width"] = std::to_string(w);
  config["window_height"] = std::to_string(h);
  save_config_map(config);
}
