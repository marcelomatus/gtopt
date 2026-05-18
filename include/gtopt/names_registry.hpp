/**
 * @file      names_registry.hpp
 * @brief     Runtime-loadable dictionary of gtopt JSON key aliases
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Loads `share/gtopt/naming_dialects.json` (or an embedded fallback)
 * and exposes a flat `alias -> canonical` map used by
 * `json_canonicalize.hpp` to rewrite alternative JSON key names to
 * the canonical gtopt names accepted by `from_json<Planning>` under
 * `StrictParsePolicy`.
 *
 * File search order:
 *
 *   1. `$GTOPT_NAMING_DIALECTS_FILE` environment variable
 *   2. `${CMAKE_INSTALL_FULL_DATADIR}/gtopt/naming_dialects.json`
 *      (absolute install path baked at configure time)
 *   3. `<source-tree>/share/gtopt/naming_dialects.json`
 *      (development workflow — when running from the build tree)
 *   4. Compiled-in fallback (an embedded copy of the shipped file)
 *   5. Empty registry (graceful degradation)
 *
 * The registry is loaded once on first access (singleton); subsequent
 * lookups are O(log n) via `gtopt::flat_map` (vector-backed; the
 * dictionary is small — ~50-200 entries — so cache-friendly linear
 * memory wins over hash-table indirection).
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/fmap.hpp>

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

  /// Resolve a **global** alias to its canonical name.  Returns
  /// `nullopt` if `alias` is not a registered global alias (which
  /// includes the case where `alias` is itself already a canonical
  /// name).  Class-scoped aliases are not consulted by this overload
  /// — use the (class, alias) overload below from element-typed
  /// resolution paths.  This overload is used by `canonicalize_json_keys`
  /// (which has no element-type context).
  [[nodiscard]] std::optional<std::string_view> canonical_for(
      std::string_view alias) const noexcept;

  /// Resolve an alias scoped to a specific element class
  /// (`"generator"`, `"flow_right"`, …).  Looks up the class-scoped
  /// table first (`class_aliases[]` in naming_dialects.json), then
  /// falls back to the global table.  Used by `resolve_single_param`
  /// where the element class is known and aliases like `discharge`
  /// (canonical for `flow`, legacy for `flow_right.target`) need
  /// class-aware disambiguation.
  [[nodiscard]] std::optional<std::string_view> canonical_for(
      std::string_view class_name, std::string_view alias) const noexcept;

  /// Number of (alias, canonical) entries. Useful for diagnostics
  /// and tests.
  [[nodiscard]] std::size_t size() const noexcept
  {
    return m_alias_to_canonical_.size();
  }

  /// Returns the (canonical -> {aliases}) inverse view for global
  /// aliases.  Used for diagnostics (e.g. `--list-dialects`).
  /// Class-scoped aliases are not included.
  [[nodiscard]] const gtopt::flat_map<std::string, std::vector<std::string>>&
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

  gtopt::flat_map<std::string, std::string> m_alias_to_canonical_;
  gtopt::flat_map<std::string, std::vector<std::string>>
      m_canonical_to_aliases_;
  /// Class-scoped lookup table.  Key is `(class_name, alias)`; value
  /// is the canonical attribute name within that class.  Populated
  /// from `class_aliases[]` in naming_dialects.json.  Looked up only
  /// via the (class, alias) overload of `canonical_for`.
  gtopt::flat_map<std::pair<std::string, std::string>, std::string>
      m_class_alias_to_canonical_;
  std::optional<std::filesystem::path> m_source_path_;
};

/// Resolve the file path the registry singleton would load from. Used
/// for diagnostics (`--list-dialects`) and tests. Returns `nullopt`
/// when only the compiled-in fallback is available.
[[nodiscard]] std::optional<std::filesystem::path> find_names_file();

}  // namespace gtopt
