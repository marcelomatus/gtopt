/**
 * @file      names_registry.hpp
 * @brief     Runtime-loadable dictionary of gtopt JSON key aliases
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Loads `share/gtopt/names.json` (or an embedded fallback) and exposes
 * a flat `alias -> canonical` map used by `json_canonicalize.hpp` to
 * rewrite alternative JSON key names to the canonical gtopt names
 * accepted by `from_json<Planning>` under `StrictParsePolicy`.
 *
 * File search order:
 *
 *   1. `$GTOPT_NAMES_FILE` environment variable
 *   2. `<exe-dir>/../share/gtopt/names.json` (standard relative install)
 *   3. `<exe-dir>/share/gtopt/names.json`    (flat install)
 *   4. `${CMAKE_INSTALL_FULL_DATADIR}/gtopt/names.json`
 *      (absolute install path baked at configure time)
 *   5. `<source-tree>/share/gtopt/names.json`
 *      (development workflow — when running from the build tree)
 *   6. Compiled-in fallback (an embedded copy of the shipped file)
 *
 * The registry is loaded once on first access (singleton); subsequent
 * lookups are O(log n) via `boost::container::flat_map`.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/container/flat_map.hpp>

namespace gtopt
{

class NamesRegistry
{
public:
  /// Get the process-wide singleton, lazily loaded on first call.
  /// Lookups against the returned reference are thread-safe (the map
  /// is `const` after construction).
  [[nodiscard]] static const NamesRegistry& instance();

  /// Construct from an explicit JSON string (used by tests and for
  /// the `instance()` boot path). Throws on parse error.
  explicit NamesRegistry(std::string_view json_content);

  /// Construct from a JSON file. Throws on read or parse error.
  explicit NamesRegistry(const std::filesystem::path& json_path);

  /// Resolve an alias to its canonical name. Returns `nullopt` if
  /// `alias` is not a registered alias (which includes the case
  /// where `alias` is itself already a canonical name).
  [[nodiscard]] std::optional<std::string_view> canonical_for(
      std::string_view alias) const noexcept;

  /// Number of (alias, canonical) entries. Useful for diagnostics
  /// and tests.
  [[nodiscard]] std::size_t size() const noexcept
  {
    return m_alias_to_canonical_.size();
  }

  /// Returns the (canonical -> {aliases}) inverse view for
  /// diagnostics (e.g. `--list-dialects`).
  [[nodiscard]] const boost::container::flat_map<std::string,
                                                 std::vector<std::string>>&
  canonical_to_aliases() const noexcept
  {
    return m_canonical_to_aliases_;
  }

  /// Path the registry was loaded from, or `nullopt` if loaded from
  /// the built-in fallback or an explicit JSON string.
  [[nodiscard]] const std::optional<std::filesystem::path>& source_path()
      const noexcept
  {
    return m_source_path_;
  }

private:
  /// Build the flat maps from a parsed JSON document text.
  /// Throws `std::runtime_error` if the document is malformed.
  void build_from_json(std::string_view json_content);

  /// Detect alias / canonical collisions while building. An alias
  /// must not be registered as the canonical of another field, and
  /// no alias may appear under more than one canonical (per the
  /// uniqueness invariant of §10.4).
  void check_alias_uniqueness(std::string_view alias,
                              std::string_view canonical) const;

  boost::container::flat_map<std::string, std::string> m_alias_to_canonical_;
  boost::container::flat_map<std::string, std::vector<std::string>>
      m_canonical_to_aliases_;
  std::optional<std::filesystem::path> m_source_path_;
};

/// Resolve the file path the registry singleton would load from. Used
/// for diagnostics (`--list-dialects`) and tests. Returns `nullopt`
/// when only the compiled-in fallback is available.
[[nodiscard]] std::optional<std::filesystem::path> find_names_file();

}  // namespace gtopt
