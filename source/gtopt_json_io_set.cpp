/**
 * @file      gtopt_json_io_set.cpp
 * @brief     --set key=value overlay for Planning (split from
 * gtopt_json_io.cpp)
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains apply_set_options() and its helpers.  Instantiates
 * from_json<Planning> for JSON overlay merging.
 */

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/solver_options.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

constexpr auto FastParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::no,
    daw::json::options::ExcludeSpecialEscapes::no,
    daw::json::options::UseExactMappingsByDefault::no>;

// ── --set key=value support ───────────────────────────────────────────

/// Auto-detect the JSON type of a value string.
/// Returns the value ready for embedding in a JSON document:
///  - "true"/"false"/"null" → bare keyword
///  - integer string        → bare number
///  - decimal / scientific  → bare number
///  - anything else         → JSON-quoted string
[[nodiscard]] std::string to_json_value(const std::string& v)
{
  if (v == "true" || v == "false" || v == "null") {
    return v;
  }
  // Try integer
  {
    char* end = nullptr;
    (void)std::strtoll(v.c_str(), &end, /*base=*/10);
    if (end != v.c_str() && *end == '\0') {
      return v;
    }
  }
  // Try floating-point
  {
    char* end = nullptr;
    (void)std::strtod(v.c_str(), &end);
    if (end != v.c_str() && *end == '\0') {
      return v;
    }
  }
  // String: escape backslashes and double-quotes
  std::string escaped;
  escaped.reserve(v.size() + 2);
  escaped += '"';
  for (const char c : v) {
    if (c == '"' || c == '\\') {
      escaped += '\\';
    }
    escaped += c;
  }
  escaped += '"';
  return escaped;
}

/// Build a minimal Planning JSON overlay from a dotted key and a JSON
/// value. The key is a dotted path relative to `planning.options`, e.g.
/// `"sddp_options.forward_solver_options.threads"`.  The result is a
/// valid JSON string like:
/// `{"options":{"sddp_options":{"forward_solver_options":{"threads":8}}}}`
[[nodiscard]] std::string build_set_option_json(const std::string& dotted_key,
                                                const std::string& json_val)
{
  // Split the key on '.'
  std::vector<std::string_view> parts;
  std::string_view key_sv {dotted_key};
  while (true) {
    const auto dot = key_sv.find('.');
    if (dot == std::string_view::npos) {
      parts.push_back(key_sv);
      break;
    }
    parts.push_back(key_sv.substr(0, dot));
    key_sv.remove_prefix(dot + 1);
  }

  // Build nested JSON under "options"
  std::string json = "{\"options\":{";
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    json += '"';
    json += parts[i];
    json += "\":{";
  }
  json += '"';
  json += parts.back();
  json += "\":";
  json += json_val;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    json += '}';
  }
  json += "}}";
  return json;
}

/// Set a single field on a SolverOptions struct.
/// @return true if the field was recognised and set.
bool try_set_solver_field(SolverOptions& so,
                          std::string_view field,
                          const std::string& value)
{
  if (field == "threads") {
    so.threads = std::stoi(value);
    return true;
  }
  if (field == "algorithm") {
    if (const auto algo = enum_from_name<LPAlgo>(value)) {
      so.algorithm = *algo;
    } else {
      const auto v = std::stoi(value);
      if (v < 0 || v >= static_cast<int>(LPAlgo::last_algo)) {
        spdlog::error("algorithm value {} out of range (0–{})",
                      v,
                      static_cast<int>(LPAlgo::last_algo) - 1);
        return false;
      }
      so.algorithm = static_cast<LPAlgo>(v);
    }
    return true;
  }
  if (field == "presolve") {
    so.presolve = (value == "true" || value == "1");
    return true;
  }
  if (field == "log_level") {
    so.log_level = std::stoi(value);
    return true;
  }
  if (field == "optimal_eps") {
    so.optimal_eps = std::stod(value);
    return true;
  }
  if (field == "feasible_eps") {
    so.feasible_eps = std::stod(value);
    return true;
  }
  if (field == "barrier_eps") {
    so.barrier_eps = std::stod(value);
    return true;
  }
  if (field == "time_limit") {
    so.time_limit = std::stod(value);
    return true;
  }
  if (field == "reuse_basis") {
    so.reuse_basis = (value == "true" || value == "1");
    return true;
  }
  if (field == "log_mode") {
    if (const auto mode = enum_from_name<SolverLogMode>(value)) {
      so.log_mode = mode;
    } else {
      spdlog::error("Invalid log_mode '{}' (expected nolog or detailed)",
                    value);
      return false;
    }
    return true;
  }
  return false;
}

/// Try to handle a --set key=value as a direct SolverOptions field set.
/// Returns true if the key matched a solver_options path and was applied.
bool try_set_solver_options_path(Planning& planning,
                                 const std::string& key,
                                 const std::string& value)
{
  // solver_options.<field>
  constexpr std::string_view pfx_so = "solver_options.";
  if (key.starts_with(pfx_so)) {
    return try_set_solver_field(
        planning.options.solver_options, key.substr(pfx_so.size()), value);
  }

  // sddp_options.forward_solver_options.<field>
  constexpr std::string_view pfx_fwd = "sddp_options.forward_solver_options.";
  if (key.starts_with(pfx_fwd)) {
    auto& fso = planning.options.sddp_options.forward_solver_options;
    if (!fso) {
      fso = SolverOptions {};
    }
    return try_set_solver_field(*fso, key.substr(pfx_fwd.size()), value);
  }

  // sddp_options.backward_solver_options.<field>
  constexpr std::string_view pfx_bwd = "sddp_options.backward_solver_options.";
  if (key.starts_with(pfx_bwd)) {
    auto& bso = planning.options.sddp_options.backward_solver_options;
    if (!bso) {
      bso = SolverOptions {};
    }
    return try_set_solver_field(*bso, key.substr(pfx_bwd.size()), value);
  }

  // monolithic_options.solver_options.<field>
  constexpr std::string_view pfx_mono = "monolithic_options.solver_options.";
  if (key.starts_with(pfx_mono)) {
    auto& mso = planning.options.monolithic_options.solver_options;
    if (!mso) {
      mso = SolverOptions {};
    }
    return try_set_solver_field(*mso, key.substr(pfx_mono.size()), value);
  }

  return false;
}

// ── end --set support ─────────────────────────────────────────────────

}  // namespace

bool apply_set_options(Planning& planning,
                       const std::vector<std::string>& set_options)
{
  for (const auto& opt : set_options) {
    const auto eq_pos = opt.find('=');
    if (eq_pos == std::string::npos || eq_pos == 0) {
      spdlog::error("--set: invalid format '{}' (expected key=value)", opt);
      return false;
    }
    const auto key = opt.substr(0, eq_pos);
    const auto value = opt.substr(eq_pos + 1);

    // Direct setter for SolverOptions paths (non-optional struct fields)
    try {
      if (try_set_solver_options_path(planning, key, value)) {
        spdlog::info("--set {}={} applied", key, value);
        continue;
      }
    } catch (const std::exception& ex) {
      spdlog::error("--set {}={}: {}", key, value, ex.what());
      return false;
    }

    // JSON overlay approach for all other paths
    const auto json_val = to_json_value(value);
    auto json = build_set_option_json(key, json_val);

    try {
      auto overlay = daw::json::from_json<Planning>(json, FastParsePolicy);
      planning.merge(std::move(overlay));
      spdlog::info("--set {}={} applied", key, value);
    } catch (const daw::json::json_exception& ex) {
      // If auto-typed value failed (e.g. number where string expected),
      // retry with the value as a quoted string
      if (json_val.front() != '"') {
        auto str_json = build_set_option_json(key, "\"" + value + "\"");
        try {
          auto overlay =
              daw::json::from_json<Planning>(str_json, FastParsePolicy);
          planning.merge(std::move(overlay));
          spdlog::info("--set {}={} applied (as string)", key, value);
          continue;
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      }
      spdlog::error("--set {}={}: unknown option or invalid value", key, value);
      return false;
    }
  }
  return true;
}

}  // namespace gtopt
