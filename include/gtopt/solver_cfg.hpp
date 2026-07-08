/**
 * @file      solver_cfg.hpp
 * @brief     Solver tuning-file loader: INI `.cfg` with named enum values
 * @date      2026-07-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Backend plugins accept a per-solver tuning file next to gtopt's
 * solver-agnostic ``solver_options.param_file`` (cases typically pin that to
 * ``solvers/cplex.prm``, so per-solver knobs live in SIBLING files).  This
 * header provides the shared loader so no plugin hand-rolls parsing:
 *
 *   1. ``<dir>/<solver>.cfg`` — preferred.  Standard INI via
 *      ``parse_ini_file`` (same dialect as ``.gtopt.conf``): ``key = value``
 *      pairs, ``#`` / ``;`` comment lines, optional ``[<solver>]`` section
 *      (sectionless keys are accepted too; section entries override them).
 *      Inline trailing comments after the value are stripped.
 *
 *   2. ``<dir>/<solver>.prm`` — legacy fallback (read only when no ``.cfg``
 *      exists).  Whitespace-separated ``<name> <value>`` lines with ``#`` /
 *      ``//`` comments — the historical cuopt.prm dialect.
 *
 * Values from EITHER file go through the same translation so tuning files
 * can say what they mean instead of memorising solver integer codes:
 *
 *   - Numbers pass through verbatim (full backward compatibility).
 *   - Keys registered with an @ref EnumEntry table (the project's named-enum
 *     framework, see enum_option.hpp) accept the entry names
 *     case-insensitively: ``method = concurrent`` → ``method 0``.
 *   - Any other ``true/false/on/off/yes/no`` value maps to ``1`` / ``0``.
 *   - A symbolic value that matches nothing is DROPPED with a warning that
 *     lists the expected names — never forwarded as garbage.
 */

#pragma once

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtopt/config_file.hpp>
#include <gtopt/enum_option.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/// One (key, value) tuning pair in file order — the order matters because
/// plugins apply the pairs sequentially and last-write wins.
using SolverCfgPairs = std::vector<std::pair<std::string, std::string>>;

/// Binds a tuning-file key to the EnumEntry table naming its integer codes.
/// The table's ``E`` is plain ``int`` because solver C APIs take raw integer
/// codes; the names come from the solver's documentation, with
/// ``is_alias`` entries for tolerated alternate spellings.
struct SolverCfgEnumKey
{
  std::string_view key;
  std::span<const EnumEntry<int>> entries;
};

namespace detail
{

/// True when `value` is fully consumed as a (possibly signed / fractional /
/// exponent) number — such values pass through untranslated.
[[nodiscard]] inline bool cfg_value_is_number(std::string_view value) noexcept
{
  if (value.empty()) {
    return false;
  }
  double parsed {};
  const auto* first = value.data();
  const auto* last =
      std::next(first, static_cast<std::ptrdiff_t>(value.size()));
  const auto [ptr, ec] = std::from_chars(first, last, parsed);
  return ec == std::errc {} && ptr == last;
}

/// Strip an inline trailing comment (``value  # note`` / ``; note`` /
/// ``// note``) plus surrounding whitespace.  Only a WHITESPACE-preceded
/// marker starts a comment, so values themselves can never be split (all
/// solver parameter values are single tokens).
[[nodiscard]] inline std::string_view cfg_strip_inline_comment(
    std::string_view value) noexcept
{
  for (std::size_t i = 0; i < value.size(); ++i) {
    const bool at_marker = value[i] == '#' || value[i] == ';'
        || (value[i] == '/' && i + 1 < value.size() && value[i + 1] == '/');
    if (at_marker && (i == 0 || value[i - 1] == ' ' || value[i - 1] == '\t')) {
      value = value.substr(0, i);
      break;
    }
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  return value;
}

/// Generic boolean spellings accepted for any key without an enum table.
inline constexpr auto cfg_bool_entries = std::to_array<EnumEntry<int>>({
    {.name = "false", .value = 0},
    {.name = "true", .value = 1},
    {.name = "off", .value = 0, .is_alias = true},
    {.name = "on", .value = 1, .is_alias = true},
    {.name = "no", .value = 0, .is_alias = true},
    {.name = "yes", .value = 1, .is_alias = true},
});

/// Comma-joined non-alias names of `entries` for warning messages.
[[nodiscard]] inline std::string cfg_expected_names(
    std::span<const EnumEntry<int>> entries)
{
  std::string out;
  for (const auto& e : entries) {
    if (e.is_alias) {
      continue;
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += e.name;
  }
  return out;
}

}  // namespace detail

/**
 * @brief Translate one tuning value into the solver's numeric code.
 *
 * Numbers pass through verbatim.  A key present in @p enum_keys accepts its
 * table's names (case-insensitive, aliases included); any other key accepts
 * the generic boolean spellings.  Returns @c std::nullopt — and logs a
 * warning naming the expected values — when the value is symbolic but
 * matches nothing; callers drop the pair.
 */
[[nodiscard]] inline std::optional<std::string> translate_solver_cfg_value(
    std::span<const SolverCfgEnumKey> enum_keys,
    std::string_view key,
    std::string_view value)
{
  if (detail::cfg_value_is_number(value)) {
    return std::string {value};
  }
  for (const auto& ek : enum_keys) {
    if (detail::ascii_iequals(ek.key, key)) {
      if (const auto found = enum_from_name<int>(ek.entries, value)) {
        return std::to_string(*found);
      }
      spdlog::warn(
          "solver cfg: invalid value '{}' for key '{}' (expected: {}) — "
          "entry dropped",
          value,
          key,
          detail::cfg_expected_names(ek.entries));
      return std::nullopt;
    }
  }
  if (const auto found =
          enum_from_name<int>(std::span {detail::cfg_bool_entries}, value))
  {
    return std::to_string(*found);
  }
  spdlog::warn(
      "solver cfg: non-numeric value '{}' for key '{}' is not a known "
      "name — entry dropped",
      value,
      key);
  return std::nullopt;
}

/**
 * @brief Load and translate a solver tuning file.
 *
 * Resolves ``<solver>.cfg`` (INI, preferred) then ``<solver>.prm`` (legacy)
 * as siblings of @p param_file — see the file header for both dialects.
 * Every value is routed through @ref translate_solver_cfg_value with
 * @p enum_keys, so ``.cfg`` and legacy ``.prm`` files both accept named
 * values.  Order is preserved: sectionless INI entries first, then the
 * ``[<solver>]`` section (so section entries win when a plugin applies the
 * pairs sequentially).
 *
 * @param param_file  gtopt's solver-agnostic parameter-file path (only its
 *                    parent directory is used); disengaged → empty result.
 * @param solver_name Basename stem: ``cuopt`` → ``cuopt.cfg`` / ``cuopt.prm``.
 * @param enum_keys   Per-key named-value tables (may be empty).
 */
[[nodiscard]] inline SolverCfgPairs load_solver_cfg(
    const std::optional<std::string>& param_file,
    std::string_view solver_name,
    std::span<const SolverCfgEnumKey> enum_keys)
{
  SolverCfgPairs pairs;
  if (!param_file.has_value() || param_file->empty()) {
    return pairs;
  }
  const auto dir = std::filesystem::path {*param_file}.parent_path();

  const auto push_translated =
      [&](const std::string& key, std::string_view raw_value)
  {
    const auto value = detail::cfg_strip_inline_comment(raw_value);
    if (value.empty()) {
      return;
    }
    if (auto translated = translate_solver_cfg_value(enum_keys, key, value)) {
      pairs.emplace_back(key, std::move(*translated));
    }
  };

  std::error_code ec;
  if (const auto cfg_path = dir / (std::string {solver_name} + ".cfg");
      std::filesystem::exists(cfg_path, ec) && !ec)
  {
    const auto ini = parse_ini_file(cfg_path);
    // Sectionless entries first, the solver's own section second — the
    // plugin applies pairs in order, so section entries override.
    for (const auto& section : {std::string {}, std::string {solver_name}}) {
      if (const auto it = ini.find(section); it != ini.end()) {
        for (const auto& [key, value] : it->second) {
          push_translated(key, value);
        }
      }
    }
    return pairs;
  }

  const auto prm_path = dir / (std::string {solver_name} + ".prm");
  if (!std::filesystem::exists(prm_path, ec) || ec) {
    return pairs;
  }
  std::ifstream prm {prm_path};
  std::string line;
  while (std::getline(prm, line)) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '#' || line[first] == ';'
        || line.compare(first, 2, "//") == 0)
    {
      continue;
    }
    // Legacy dialect: ``<name> <value>``; tolerate ``<name> = <value>`` too
    // so files can migrate content before being renamed to ``.cfg``.
    std::istringstream iss {line.substr(first)};
    std::string name;
    std::string value;
    if (iss >> name >> value) {
      if (value == "=" && !(iss >> value)) {
        continue;
      }
      push_translated(name, value);
    }
  }
  return pairs;
}

}  // namespace gtopt
