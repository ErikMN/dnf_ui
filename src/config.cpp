// -----------------------------------------------------------------------------
// src/config.cpp
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
config_load_map()
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
config_save_map(const std::map<std::string, std::string> &config)
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
config_load_paned_position()
{
  auto config = config_load_map();
  if (config.count("paned_position")) {
    return std::stoi(config["paned_position"]);
  }

  return 300;
}

void
config_save_paned_position(GtkPaned *paned)
{
  auto config = config_load_map();
  config["paned_position"] = std::to_string(gtk_paned_get_position(paned));
  config_save_map(config);
}

void
config_load_window_geometry(GtkWindow *window)
{
  auto config = config_load_map();
  int w = 1200, h = 820;
  if (config.count("window_width"))
    w = std::stoi(config["window_width"]);
  if (config.count("window_height"))
    h = std::stoi(config["window_height"]);
  if (w < 600)
    w = 1200;
  if (h < 400)
    h = 820;
  gtk_window_set_default_size(window, w, h);
}

void
config_save_window_geometry(GtkWindow *window)
{
  auto config = config_load_map();
  int w = 1200, h = 820;

  // GTK4 build: use gtk_window_get_default_size() and gtk_widget_compute_bounds()
  int default_w = 0, default_h = 0;
  gtk_window_get_default_size(window, &default_w, &default_h);

  if (default_w > 0 && default_h > 0) {
    w = default_w;
    h = default_h;
  } else {
    // fallback to compute bounds if no default size set
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(GTK_WIDGET(window), nullptr, &bounds)) {
      w = static_cast<int>(bounds.size.width);
      h = static_cast<int>(bounds.size.height);
    }
  }

  config["window_width"] = std::to_string(w);
  config["window_height"] = std::to_string(h);
  config_save_map(config);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
