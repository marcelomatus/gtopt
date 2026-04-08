/**
 * @file      gtopt_json_io.cpp
 * @brief     JSON parsing, serialization, and --set overlay for Planning
 * @date      2026-04-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Separated from gtopt_main.cpp so that the heavy DAW JSON headers
 * compile in their own translation unit, enabling parallel compilation.
 */

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <daw/daw_read_file.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/pampl_parser.hpp>
#include <gtopt/solver_options.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

namespace
{

constexpr auto FastParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::no,
    daw::json::options::ExcludeSpecialEscapes::no,
    daw::json::options::UseExactMappingsByDefault::no>;

constexpr auto StrictParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::no>;

/// Same as StrictParsePolicy but rejects any JSON key not listed in the
/// schema. Used by the --check-json pass to surface typos / unknown fields.
constexpr auto ExactParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::yes>;

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
      so.algorithm = static_cast<LPAlgo>(std::stoi(value));
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
      so.log_mode = static_cast<SolverLogMode>(std::stoi(value));
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

// ── Public API ────────────────────────────────────────────────────────

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

std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files,
    bool strict_parsing,
    bool check_json,
    const std::optional<std::string>& input_directory)
{
  const spdlog::stopwatch sw;
  Planning my_planning;

  for (const auto& planning_file : planning_files) {
    try {
      std::filesystem::path fpath(planning_file);
      fpath.replace_extension(".json");

      // Check existence before calling daw::read_file, which is noexcept
      // and would call std::terminate via std::filesystem::file_size if
      // the file is missing.  If the file is not found in the current
      // directory, try the input_directory as a fallback.
      if (!std::filesystem::exists(fpath) && input_directory.has_value()) {
        auto alt =
            std::filesystem::path(input_directory.value()) / fpath.filename();
        if (std::filesystem::exists(alt)) {
          spdlog::info("  Found '{}' in input_directory '{}'",
                       fpath.filename().string(),
                       input_directory.value());
          fpath = std::move(alt);
        }
      }
      if (!std::filesystem::exists(fpath)) {
        return std::unexpected(
            std::format("Input file '{}' does not exist", fpath.string()));
      }

      // NOLINTNEXTLINE(clang-analyzer-unix.Stream) - stream leak false
      // positive in daw::read_file
      const auto json_result = daw::read_file(fpath.string());

      if (!json_result) {
        return std::unexpected(
            std::format("Failed to read input file '{}'", planning_file));
      }

      spdlog::info(std::format("  Parsing input file {}", fpath.string()));

      // Optional pre-pass: parse with exact-mapping policy to surface
      // any JSON key not listed in the schema.  This parses the file a
      // second time (performance trade-off), but only when --check-json
      // is explicitly requested.  Warnings only — parsing always
      // proceeds with the normal policy below.
      if (check_json) {
        try {
          (void)daw::json::from_json<Planning>(json_result.value(),
                                               ExactParsePolicy);
        } catch (const daw::json::json_exception& jex) {
          spdlog::warn("Unknown JSON field in '{}': {}",
                       fpath.string(),
                       to_formatted_string(jex, json_result.value().c_str()));
        }
      }

      try {
        if (strict_parsing) {
          auto plan = daw::json::from_json<Planning>(json_result.value(),
                                                     StrictParsePolicy);
          my_planning.merge(std::move(plan));
        } else {
          auto plan = daw::json::from_json<Planning>(json_result.value(),
                                                     FastParsePolicy);
          my_planning.merge(std::move(plan));
        }
      } catch (const daw::json::json_exception& jex) {
        return std::unexpected(
            std::format("JSON parsing error in file '{}': {}",
                        fpath.string(),
                        to_formatted_string(jex, json_result.value().c_str())));
      }

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Unexpected error processing file '{}': {}",
                      planning_file,
                      ex.what()));
    }
  }

  spdlog::info(std::format("  Parse all input files time {:.3f}s",
                           sw.elapsed().count()));
  return my_planning;
}

std::expected<void, std::string> write_json_output(const Planning& planning,
                                                   const std::string& json_file)
{
  const spdlog::stopwatch sw;
  std::filesystem::path jpath(json_file);

  try {
    jpath.replace_extension(".json");
  } catch (const std::filesystem::filesystem_error& ex) {
    return std::unexpected(
        std::format("Filesystem error processing JSON output path '{}': {}",
                    json_file,
                    ex.what()));
  }

  std::ofstream jfile(jpath);
  if (!jfile) {
    return std::unexpected(
        std::format("Failed to create JSON output file '{}'", jpath.string()));
  }

  try {
    jfile << daw::json::to_json(planning) << '\n';
  } catch (const daw::json::json_exception& ex) {
    return std::unexpected(
        std::format("JSON serialization error for file '{}': {}",
                    jpath.string(),
                    ex.what()));
  }

  spdlog::info(std::format("  Write system json file time {:.3f}s",
                           sw.elapsed().count()));
  return {};
}

std::expected<void, std::string> load_user_constraints(Planning& planning)
{
  std::vector<std::string> uc_paths;
  if (const auto& uc_file = planning.system.user_constraint_file) {
    uc_paths.push_back(*uc_file);
  }
  for (const auto& f : planning.system.user_constraint_files) {
    uc_paths.push_back(f);
  }

  for (const auto& uc_name : uc_paths) {
    auto filepath = std::filesystem::path {uc_name};
    // Resolve relative paths against input_directory
    if (filepath.is_relative() && planning.options.input_directory) {
      auto alt =
          std::filesystem::path {*planning.options.input_directory} / filepath;
      if (std::filesystem::exists(alt)) {
        filepath = std::move(alt);
      }
    }
    const auto ext = filepath.extension().string();

    try {
      // Determine next UID to avoid collisions with inline constraints
      const auto& existing = planning.system.user_constraint_array;
      Uid next_uid = Uid {1};
      for (const auto& uc : existing) {
        if (uc.uid >= next_uid) {
          next_uid = uc.uid + Uid {1};
        }
      }

      if (ext == ".pampl") {
        auto pampl_result =
            PamplParser::parse_file(filepath.string(), next_uid);
        spdlog::info(
            std::format("Loaded {} constraint(s) and {} param(s) from PAMPL"
                        " file '{}'",
                        pampl_result.constraints.size(),
                        pampl_result.params.size(),
                        filepath.string()));

        auto& arr = planning.system.user_constraint_array;
        arr.insert(arr.end(),
                   std::make_move_iterator(pampl_result.constraints.begin()),
                   std::make_move_iterator(pampl_result.constraints.end()));

        auto& parr = planning.system.user_param_array;
        parr.insert(parr.end(),
                    std::make_move_iterator(pampl_result.params.begin()),
                    std::make_move_iterator(pampl_result.params.end()));
      } else {
        // Default: treat as JSON array of UserConstraint
        auto file_content = daw::read_file(filepath.string());
        if (!file_content) {
          return std::unexpected(std::format(
              "Cannot read user_constraint_file '{}'", filepath.string()));
        }
        auto loaded =
            daw::json::from_json<std::vector<UserConstraint>>(*file_content);
        spdlog::info(
            std::format("Loaded {} user constraint(s) from JSON file '{}'",
                        loaded.size(),
                        filepath.string()));

        auto& arr = planning.system.user_constraint_array;
        arr.insert(arr.end(),
                   std::make_move_iterator(loaded.begin()),
                   std::make_move_iterator(loaded.end()));
      }

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Error loading user_constraint_file '{}': {}",
                      filepath.string(),
                      ex.what()));
    }
  }
  return {};
}

}  // namespace gtopt
