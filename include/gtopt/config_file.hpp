/**
 * @file      config_file.hpp
 * @brief     Lightweight INI config file reader for .gtopt.conf
 * @date      2026-03-25
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Reads the standard `~/.gtopt.conf` INI file used by all gtopt scripts.
 * The `[gtopt]` section provides default values for the C++ binary's
 * command-line options.  CLI flags always take precedence over config values.
 *
 * ### Search order
 * 1. `$GTOPT_CONFIG` environment variable (exact path)
 * 2. `./.gtopt.conf` (current working directory)
 * 3. `~/.gtopt.conf` (home directory)
 *
 * ### Format
 * ```ini
 * [gtopt]
 * solver          = highs
 * algorithm       = barrier
 * threads         = 4
 * output-format   = parquet
 * output-compression = zstd
 * sddp-max-iterations = 200
 * ```
 */

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace gtopt
{

/// Parsed INI sections: section name → {key → value}.
using IniData = std::map<std::string, std::map<std::string, std::string>>;

/**
 * @brief Parse a simple INI file.
 *
 * Handles `[section]` headers, `key = value` pairs, `#` and `;` comments,
 * and blank lines.  Keys are trimmed; values are trimmed.  Keys outside any
 * section are placed under the empty-string section.
 *
 * @param path Path to the INI file.
 * @return Parsed sections, or empty if the file cannot be opened.
 */
[[nodiscard]] inline IniData parse_ini_file(const std::filesystem::path& path)
{
  IniData data;
  std::ifstream file(path);
  if (!file.is_open()) {
    return data;
  }

  std::string current_section;
  std::string line;
  while (std::getline(file, line)) {
    // Trim leading whitespace
    const auto start = line.find_first_not_of(" \t");
    if (start == std::string::npos) {
      continue;  // blank line
    }
    line = line.substr(start);

    // Skip comments
    if (line.front() == '#' || line.front() == ';') {
      continue;
    }

    // Section header
    if (line.front() == '[') {
      const auto end = line.find(']');
      if (end != std::string::npos) {
        current_section = line.substr(1, end - 1);
        // Trim section name
        const auto s = current_section.find_first_not_of(" \t");
        const auto e = current_section.find_last_not_of(" \t");
        if (s != std::string::npos) {
          current_section = current_section.substr(s, e - s + 1);
        }
      }
      continue;
    }

    // Key = value
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;  // skip malformed lines
    }

    auto key = line.substr(0, eq);
    auto val = line.substr(eq + 1);

    // Trim key
    if (const auto e = key.find_last_not_of(" \t"); e != std::string::npos) {
      key = key.substr(0, e + 1);
    }

    // Trim value
    if (const auto s = val.find_first_not_of(" \t"); s != std::string::npos) {
      val = val.substr(s);
      if (const auto e = val.find_last_not_of(" \t\r\n");
          e != std::string::npos)
      {
        val = val.substr(0, e + 1);
      }
    } else {
      val.clear();
    }

    data[current_section][key] = val;
  }

  return data;
}

/**
 * @brief Find the `.gtopt.conf` file following the standard search order.
 *
 * 1. `$GTOPT_CONFIG` (if set and file exists)
 * 2. `./.gtopt.conf` (current directory)
 * 3. `~/.gtopt.conf` (home directory)
 *
 * @return Path to the config file, or empty path if none found.
 */
[[nodiscard]] inline std::filesystem::path find_config_file()
{
  // 1. Environment variable
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* env = std::getenv("GTOPT_CONFIG")) {
    std::filesystem::path p(env);
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  // 2. Current directory
  {
    std::filesystem::path p = ".gtopt.conf";
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  // 3. Home directory
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* home = std::getenv("HOME")) {
    std::filesystem::path p = std::filesystem::path(home) / ".gtopt.conf";
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  return {};
}

}  // namespace gtopt
