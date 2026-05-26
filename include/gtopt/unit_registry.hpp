/**
 * @file      unit_registry.hpp
 * @brief     Runtime-loadable dictionary of (class, canonical, dialect) → unit
 * @date      2026-05-21
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Companion to `names_registry.hpp`.  Loads
 * `share/gtopt/unit_dialects.json` (or an embedded fallback) and
 * exposes a `(class, canonical, dialect) → unit` accessor.  The unit
 * strings are free-form physical-unit literals like "MW", "USD/MWh",
 * "m3/s", "Mm3" — they are *not* parsed into a unit algebra; downstream
 * code uses them only for diagnostics (`--list-dialects --units`),
 * mismatch warnings under `--naming-dialect`, and (eventually)
 * auto-conversion by a separate unit engine.
 *
 * File search order mirrors `find_names_file()`:
 *
 *   1. `$GTOPT_UNIT_DIALECTS_FILE` environment variable
 *   2. `${CMAKE_INSTALL_FULL_DATADIR}/gtopt/unit_dialects.json`
 *      (absolute install path baked at configure time)
 *   3. `<source-tree>/share/gtopt/unit_dialects.json`
 *      (development workflow — when running from the build tree)
 *   4. Compiled-in fallback (an embedded copy of the shipped file)
 *   5. Empty registry (graceful degradation; every lookup returns
 *      nullopt and no unit-aware behaviour fires)
 *
 * The registry is loaded once on first access (singleton); subsequent
 * lookups are O(log n) via `gtopt::flat_map`.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gtopt/fmap.hpp>

namespace gtopt
{

class UnitRegistry
{
public:
  /// Process-wide singleton, lazily loaded on first call.  The map is
  /// `const` after construction, so reads are thread-safe.
  [[nodiscard]] static const UnitRegistry& instance();

  /// Construct from an explicit JSON string (tests + the `instance()`
  /// boot path).  Throws `std::runtime_error` on parse error.
  explicit UnitRegistry(std::string_view json_content);

  /// Construct from a JSON file.  Throws on read or parse error.
  explicit UnitRegistry(const std::filesystem::path& json_path);

  /// Look up the unit string for a `(class, canonical, dialect)` tuple.
  /// Returns nullopt when the entry is not registered — callers should
  /// fall through to canonical / no-unit handling, never assume.
  [[nodiscard]] std::optional<std::string_view> unit_for(
      std::string_view class_name,
      std::string_view canonical,
      std::string_view dialect) const noexcept;

  /// Convenience overload assuming the gtopt-native dialect.  Useful at
  /// the LP-construction layer where canonical names are already in
  /// scope and the dialect is implied.
  [[nodiscard]] std::optional<std::string_view> unit_for(
      std::string_view class_name, std::string_view canonical) const noexcept
  {
    return unit_for(class_name, canonical, "gtopt");
  }

  /// Class-agnostic lookup: returns the single unit string if every
  /// registered `(class, canonical, dialect)` entry for the
  /// `(canonical, dialect)` pair agrees on the same unit.  Returns
  /// nullopt when the canonical has no entries in the given dialect,
  /// or when distinct entries disagree on the unit.
  ///
  /// Used by `canonicalize_json_keys` where the alias rewrite is
  /// class-blind — the JSON tokeniser does not know which element type
  /// the key belongs to.  Ambiguity (multiple classes, different units)
  /// is treated as "skip the unit-mismatch warning" rather than a hard
  /// error so the rewrite path stays robust.
  [[nodiscard]] std::optional<std::string_view> class_agnostic_unit_for(
      std::string_view canonical, std::string_view dialect) const noexcept;

  /// Number of (class, canonical, dialect) entries the registry holds.
  [[nodiscard]] std::size_t size() const noexcept { return m_units_.size(); }

  /// Path the registry was loaded from, or nullopt if loaded from an
  /// explicit JSON string or the built-in fallback.
  [[nodiscard]] const std::optional<std::filesystem::path>& source_path()
      const noexcept
  {
    return m_source_path_;
  }

private:
  void build_from_json(std::string_view json_content);

  /// Key shape: `(class_name, canonical, dialect)`.  Vector-backed
  /// `flat_map` because the registry is small (~80–200 entries) and
  /// cache-locality wins over hash overhead.
  using Key = std::tuple<std::string, std::string, std::string>;
  gtopt::flat_map<Key, std::string> m_units_;
  std::optional<std::filesystem::path> m_source_path_;
};

/// Resolve the file path the registry singleton would load from.  Used
/// for diagnostics and tests.  Returns nullopt when only the compiled-
/// in fallback is available.
[[nodiscard]] std::optional<std::filesystem::path> find_units_file();

}  // namespace gtopt
