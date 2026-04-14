// -----------------------------------------------------------------------------
// src/config.cpp
// Config helpers for saving/restoring user settings
// Handles persistent UI state (window size, pane divider positions, etc.)
// The configuration is stored as key=value pairs in:
//   ~/.config/dnf_ui.conf
//
// TODO: Consider switching to GKeyFile or JSON for more structured data
// -----------------------------------------------------------------------------
#include "config.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

#include <glib.h>

namespace {
constexpr int DEFAULT_PANED_POSITION = 300;
constexpr int DEFAULT_WINDOW_WIDTH = 1200;
constexpr int DEFAULT_WINDOW_HEIGHT = 820;
constexpr int MIN_WINDOW_WIDTH = 600;
constexpr int MIN_WINDOW_HEIGHT = 400;

std::filesystem::path
config_file_path()
{
  const char *config_dir = g_get_user_config_dir();
  if (!config_dir || !*config_dir) {
    return {};
  }

  return std::filesystem::path(config_dir) / "dnf_ui.conf";
}

bool
config_try_parse_int(const std::map<std::string, std::string> &config, const char *key, int &value_out)
{
  auto it = config.find(key);
  if (it == config.end()) {
    return false;
  }

  try {
    size_t parsed_chars = 0;
    int parsed = std::stoi(it->second, &parsed_chars);
    if (parsed_chars != it->second.size()) {
      return false;
    }

    value_out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}
} // namespace

std::map<std::string, std::string>
config_load_map()
{
  std::map<std::string, std::string> config;
  std::filesystem::path path = config_file_path();
  if (path.empty()) {
    return config;
  }

  std::ifstream file(path);
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
  std::filesystem::path path = config_file_path();
  if (path.empty()) {
    return;
  }

  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  if (!file.good()) {
    return;
  }

  for (auto &[k, v] : config) {
    file << k << "=" << v << "\n";
  }
}

int
config_load_paned_position()
{
  auto config = config_load_map();
  int position = DEFAULT_PANED_POSITION;
  if (config_try_parse_int(config, "paned_position", position)) {
    return position;
  }

  return DEFAULT_PANED_POSITION;
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
  int w = DEFAULT_WINDOW_WIDTH;
  int h = DEFAULT_WINDOW_HEIGHT;

  config_try_parse_int(config, "window_width", w);
  config_try_parse_int(config, "window_height", h);

  if (w < MIN_WINDOW_WIDTH) {
    w = DEFAULT_WINDOW_WIDTH;
  }
  if (h < MIN_WINDOW_HEIGHT) {
    h = DEFAULT_WINDOW_HEIGHT;
  }

  gtk_window_set_default_size(window, w, h);
}

void
config_save_window_geometry(GtkWindow *window)
{
  auto config = config_load_map();
  int w = 0;
  int h = 0;

  // In GTK4 the default size tracks user-driven resize changes and preserves
  // the last non-maximized size, which makes it the right value to persist.
  gtk_window_get_default_size(window, &w, &h);
  if (w <= 0 || h <= 0) {
    return;
  }

  w = std::max(w, MIN_WINDOW_WIDTH);
  h = std::max(h, MIN_WINDOW_HEIGHT);
  config["window_width"] = std::to_string(w);
  config["window_height"] = std::to_string(h);
  config_save_map(config);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
