/**
 * @file      gtopt_main.cpp
 * @brief     Core application entry point for the gtopt optimizer
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implementation of the gtopt_main() function, which ties together JSON
 * parsing, option application, LP construction, solving, and output writing.
 *
 * Key options handled here:
 *  - `planning_files`: list of JSON case file stems to load and merge.
 *  - `fast_parsing`: use lenient (non-strict) JSON parsing.
 *  - `lp_build`: build all scene/phase LP matrices but skip solving;
 *    validating input without running the solver.
 *  - `json_file`: write the merged Planning to a JSON file before solving.
 *  - `lp_file`: write the flat LP model to a `.lp` file before solving.
 *  - `print_stats`: log pre- and post-solve system statistics.
 *  - `lp_presolve`: enable/disable LP presolve in the solver.
 *  - `lp_debug`: when true, save LP debug files (one per scene/phase for
 *    the monolithic solver; one per iteration/scene/phase for SDDP) to the
 *    `log_directory`.  If `output_compression` is set (e.g. `"gzip"`), the
 *    files are compressed automatically and the originals removed.
 */

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <daw/daw_read_file.h>
#include <gtopt/error.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/lp_stats.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/pampl_parser.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/resolve_planning_args.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/validate_planning.hpp>
#include <spdlog/sinks/basic_file_sink.h>
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

/// Same as StrictParsePolicy but rejects any JSON key not listed in the schema.
/// Used by the --check-json pass to surface typos / unknown fields.
constexpr auto ExactParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::yes>;

// ── --set key=value support ─────────────────────────────────────────────────

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

/// Build a minimal Planning JSON overlay from a dotted key and a JSON value.
/// The key is a dotted path relative to `planning.options`, e.g.
/// `"sddp_options.forward_solver_options.threads"`.  The result is a valid
/// JSON string like:
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

/// Apply all `--set key=value` overrides to a Planning object.
/// Each entry in @p set_options is `"dotted.path=value"`.  The function
/// first tries a direct setter for SolverOptions paths (which have required
/// non-optional fields), then falls back to a JSON overlay approach for all
/// other paths.
/// @return true if all options were applied successfully, false on any error.
[[nodiscard]] bool apply_set_options(
    Planning& planning, const std::vector<std::string>& set_options)
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

// ── end --set support ───────────────────────────────────────────────────────

/**
 * @brief Parse planning JSON files into a Planning object.
 *
 * Iterates over each path in @p planning_files, reads the JSON, and merges
 * it into a single Planning.  Returns an error string on the first failure.
 *
 * @param planning_files Paths to input JSON files (without extension).
 * @param strict_parsing Use strict parse policy when true.
 * @param check_json     Warn (via spdlog) about unrecognised JSON fields.
 * @return The merged Planning or an error message.
 */
[[nodiscard]] std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files,
    bool strict_parsing,
    bool check_json,
    const std::optional<std::string>& input_directory = {})
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

      // Optional pre-pass: parse with exact-mapping policy to surface any
      // JSON key not listed in the schema.  This parses the file a second
      // time (performance trade-off), but only when --check-json is
      // explicitly requested.  Warnings only — parsing always proceeds with
      // the normal policy below.
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

/**
 * @brief Write the merged Planning to a JSON file.
 *
 * @param planning  The planning object to serialise.
 * @param json_file Output path stem (extension is replaced with ".json").
 * @return void on success or an error message.
 */
[[nodiscard]] std::expected<void, std::string> write_json_output(
    const Planning& planning, const std::string& json_file)
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

/**
 * @brief Log pre-solve system statistics.
 *
 * Delegates to `gtopt_check_json --info` via posix_spawn (no shell, timeout-
 * protected, output captured).  Each output line is forwarded through the
 * spdlog INFO stream so that stats always appear in the same log channel as
 * the rest of the solver output.  Falls back to the built-in C++
 * implementation when the script is not installed or exits non-zero.
 *
 * @param planning_files The list of planning JSON file stems/paths.
 * @param planning        The parsed planning (used for the built-in fallback).
 */
void log_pre_solve_stats(
    [[maybe_unused]] const std::vector<std::string>& planning_files,
    const Planning& planning)
{
  // Pre-solve stats are now printed by the built-in C++ code.
  // The external gtopt_check_json tool (richer output with global indicators)
  // is invoked by the run_gtopt wrapper script as a pre-step.

  const auto& sys = planning.system;
  const auto& sim = planning.simulation;
  const auto& plan_opts = planning.options;

  spdlog::info("=== System statistics ===");
  spdlog::info(std::format("  System name     : {}", sys.name));
  spdlog::info(std::format("  System version  : {}", sys.version));
  spdlog::info("=== System elements  ===");
  spdlog::info(std::format("  Buses           : {}", sys.bus_array.size()));
  spdlog::info(
      std::format("  Generators      : {}", sys.generator_array.size()));
  spdlog::info(std::format("  Generator profs : {}",
                           sys.generator_profile_array.size()));
  spdlog::info(std::format("  Demands         : {}", sys.demand_array.size()));
  spdlog::info(
      std::format("  Demand profs    : {}", sys.demand_profile_array.size()));
  spdlog::info(std::format("  Lines           : {}", sys.line_array.size()));
  spdlog::info(std::format("  Batteries       : {}", sys.battery_array.size()));
  spdlog::info(
      std::format("  Converters      : {}", sys.converter_array.size()));
  spdlog::info(
      std::format("  Reserve zones   : {}", sys.reserve_zone_array.size()));
  spdlog::info(std::format("  Reserve provisions   : {}",
                           sys.reserve_provision_array.size()));
  spdlog::info(
      std::format("  Junctions       : {}", sys.junction_array.size()));
  spdlog::info(
      std::format("  Waterways       : {}", sys.waterway_array.size()));
  spdlog::info(std::format("  Flows           : {}", sys.flow_array.size()));
  spdlog::info(
      std::format("  Reservoirs      : {}", sys.reservoir_array.size()));
  spdlog::info(std::format("  ReservoirSeepages     : {}",
                           sys.reservoir_seepage_array.size()));
  spdlog::info(std::format("  Turbines        : {}", sys.turbine_array.size()));
  if (!sys.flow_right_array.empty()) {
    spdlog::info(
        std::format("  Flow rights     : {}", sys.flow_right_array.size()));
  }
  if (!sys.volume_right_array.empty()) {
    spdlog::info(
        std::format("  Volume rights   : {}", sys.volume_right_array.size()));
  }
  if (!sys.user_constraint_array.empty()) {
    spdlog::info(std::format("  User constraints: {}",
                             sys.user_constraint_array.size()));
  }
  if (!sys.user_param_array.empty()) {
    spdlog::info(
        std::format("  User params     : {}", sys.user_param_array.size()));
  }
  spdlog::info("=== Simulation statistics ===");
  spdlog::info(std::format("  Blocks          : {}", sim.block_array.size()));
  spdlog::info(std::format("  Stages          : {}", sim.stage_array.size()));
  spdlog::info(
      std::format("  Scenarios       : {}", sim.scenario_array.size()));
  spdlog::info("=== Key options ===");
  const auto& mo = plan_opts.model_options;
  spdlog::info(
      std::format("  use_kirchhoff   : {}",
                  mo.use_kirchhoff.value_or(false) ? "true" : "false"));
  spdlog::info(
      std::format("  use_single_bus  : {}",
                  mo.use_single_bus.value_or(false) ? "true" : "false"));
  spdlog::info(std::format("  scale_objective : {}",
                           mo.scale_objective.value_or(1'000.0)));
  spdlog::info(
      std::format("  scale_theta     : {}",
                  mo.scale_theta.has_value()
                      ? std::format("{:.6g}", *mo.scale_theta)
                      : std::format("{:.6g} (auto)",
                                    PlanningOptionsLP::default_scale_theta)));
  spdlog::info(std::format(
      "  equilibration   : {}",
      enum_name(plan_opts.lp_build_options.equilibration_method.value_or(
          LpEquilibrationMethod::none))));
  spdlog::info(
      std::format("  demand_fail_cost: {}", mo.demand_fail_cost.value_or(0.0)));
  spdlog::info(std::format("  input_directory : {}",
                           plan_opts.input_directory.value_or("(default)")));
  spdlog::info(std::format("  output_directory: {}",
                           plan_opts.output_directory.value_or("(default)")));
  spdlog::info(std::format("  output_format   : {}",
                           plan_opts.output_format
                               ? enum_name(*plan_opts.output_format)
                               : "(default)"));
}

/**
 * @brief Log post-solve solution statistics.
 *
 * Solve time is already printed unconditionally as "planning {:.3f}s" and
 * scale_objective is already printed in log_pre_solve_stats, so neither is
 * repeated here.
 *
 * @param planning_lp  The solved PlanningLP model.
 * @param optimal      True when the solver returned an optimal solution.
 */
void log_post_solve_stats(const PlanningLP& planning_lp, bool optimal)
{
  spdlog::info("=== Solution statistics ===");
  spdlog::info(std::format("  Status          : {}",
                           optimal ? "optimal" : "non-optimal"));

  if (optimal && !planning_lp.systems().empty()
      && !planning_lp.systems().front().empty())
  {
    const auto& lp_if =
        planning_lp.systems().front().front().linear_interface();
    const auto& plp_opts = planning_lp.options();
    const double scale = plp_opts.scale_objective();
    const double obj_scaled = lp_if.get_obj_value();
    const double obj_unscaled = obj_scaled * scale;
    spdlog::info(std::format("  LP variables    : {}", lp_if.get_numcols()));
    spdlog::info(std::format("  LP constraints  : {}", lp_if.get_numrows()));
    spdlog::info(std::format("  Obj (scaled)    : {:.6g}", obj_scaled));
    spdlog::info(std::format("  Obj (unscaled)  : {:.6g}", obj_unscaled));
    spdlog::info(std::format("  Solver kappa    : {:.6g}", lp_if.get_kappa()));
  }
}

/// Set up trace-level file sink for detailed SPDLOG_TRACE output.
///
/// When --trace-log is given explicitly, uses that path; otherwise
/// auto-creates a numbered file inside the log directory
/// (e.g. logs/trace_1.log, logs/trace_2.log).
void setup_trace_log(const MainOptions& opts)
{
  const auto log_dir = std::filesystem::path(
      opts.log_directory.has_value()
          ? opts.log_directory.value()
          : (std::filesystem::path(opts.output_directory.value_or("output"))
             / "logs")
                .string());
  std::string trace_path;

  if (opts.trace_log.has_value()) {
    trace_path = opts.trace_log.value();
  } else {
    // Find the next available trace_N.log in log_dir
    try {
      std::filesystem::create_directories(log_dir);
    } catch (const std::filesystem::filesystem_error& fe) {
      spdlog::warn("could not create log directory '{}': {}",
                   log_dir.string(),
                   fe.what());
    }
    int n = 1;
    constexpr int max_trace_files = 10000;
    for (; n <= max_trace_files; ++n) {
      auto candidate = log_dir / std::format("trace_{}.log", n);
      if (!std::filesystem::exists(candidate)) {
        trace_path = candidate.string();
        break;
      }
    }
    if (trace_path.empty()) {
      // All slots used — fall back to timestamp-based name
      const auto now = std::chrono::system_clock::now();
      const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                          now.time_since_epoch())
                          .count();
      trace_path = (log_dir / std::format("trace_{}.log", ts)).string();
    }
  }

  try {
    const auto parent = std::filesystem::path(trace_path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    auto trace_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        trace_path, /*truncate=*/true);
    trace_sink->set_level(spdlog::level::trace);
    // Keep the first sink (assumed to be the console/stdout sink) at its
    // current level so trace messages only go to the file, not terminal.
    auto& sinks = spdlog::default_logger()->sinks();
    if (!sinks.empty()) {
      sinks.front()->set_level(spdlog::default_logger()->level());
    }
    sinks.push_back(std::move(trace_sink));
    spdlog::set_level(spdlog::level::trace);
    spdlog::info("trace log file: {}", trace_path);
  } catch (const spdlog::spdlog_ex& ex) {
    spdlog::warn("could not open trace log file: {}", ex.what());
  }
}

/// Load user constraints from external file(s) referenced in planning.
///
/// Supports both .pampl and JSON array formats.  Resolves relative paths
/// against input_directory and assigns UIDs to avoid collisions.
[[nodiscard]] std::expected<void, std::string> load_user_constraints(
    Planning& planning)
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

/// Apply --set overrides, CLI flags, codec probing, user constraint
/// loading, demand_fail_cost warning, and planning validation.
///
/// Returns 0 on success, EXIT_FAILURE when --set fails (matching the
/// original behaviour that returns a valid int exit code, not an error
/// string), or an error string for other failures.
[[nodiscard]] std::expected<int, std::string> configure_planning(
    Planning& planning, const MainOptions& opts)
{
  // Apply --set key=value overrides (before specific CLI flags)
  if (!opts.set_options.empty()) {
    if (!apply_set_options(planning, opts.set_options)) {
      return EXIT_FAILURE;
    }
  }

  // Specific CLI flags override --set
  apply_cli_options(planning, opts);

  // Probe the Arrow/Parquet runtime once for the best available codec.
  planning.options.output_compression =
      enum_from_name<CompressionCodec>(probe_parquet_codec(
          PlanningOptionsLP(planning.options).output_compression()));

  // Propagate lp_build so the SDDP solver also sees it.
  if (opts.lp_build) {
    planning.options.lp_build = opts.lp_build;
  }

  // Load user constraints from external file(s)
  if (auto uc_result = load_user_constraints(planning); !uc_result) {
    return std::unexpected(std::move(uc_result.error()));
  }

  // Warn when demand_fail_cost is 0 or not set
  {
    const auto dfc =
        planning.options.model_options.demand_fail_cost.value_or(0.0);
    if (dfc == 0.0) {
      spdlog::warn(
          "demand_fail_cost is 0: unserved load has no penalty. "
          "Set demand_fail_cost > 0 to penalize load shedding.");
    }
  }

  // Validate planning (referential integrity, ranges, completeness)
  {
    const auto vresult = validate_planning(planning);
    if (!vresult.ok()) {
      return std::unexpected(
          std::format("Planning validation failed with {} error(s)",  // NOLINT
                      vresult.errors.size()));
    }
  }

  return 0;
}

/// Prepare LP build options and apply per-solver config.
///
/// Logs pre-build statistics if @p do_stats is true.  The returned
/// LpBuildOptions should be passed to the PlanningLP constructor.
[[nodiscard]] LpBuildOptions prepare_lp_build(Planning& planning,
                                              const MainOptions& opts,
                                              bool do_stats)
{
  // CLI --lp-names-level overrides --set; fall back to merged planning.
  const auto eff_names_level = opts.lp_names_level
      ? opts.lp_names_level
      : planning.options.lp_build_options.names_level;
  auto flat_opts = make_lp_build_options(
      eff_names_level,
      opts.matrix_eps,
      do_stats,
      opts.solver,
      planning.options.lp_build_options.row_equilibration);

  if (do_stats) {
    log_pre_solve_stats(opts.planning_files, planning);
  }

  // Apply per-solver config from .gtopt.conf [solver.<name>] sections.
  if (const auto& solver_key = flat_opts.solver_name; !solver_key.empty()) {
    if (const auto it = opts.solver_configs.find(solver_key);
        it != opts.solver_configs.end())
    {
      auto& so = planning.options.solver_options;
      auto conf = it->second;
      conf.overlay(so);
      so = conf;
    }
  }

  return flat_opts;
}

/// Log LP coefficient statistics for all scene x phase LPs.
void log_lp_coefficient_stats(const PlanningLP& planning_lp)
{
  std::vector<ScenePhaseLPStats> lp_entries;
  for (auto&& [si, phase_systems] :
       std::views::enumerate(planning_lp.systems()))
  {
    for (auto&& [pi, system_lp] : std::views::enumerate(phase_systems)) {
      const auto& li = system_lp.linear_interface();
      lp_entries.push_back({
          .scene_uid = static_cast<int>(system_lp.scene().uid()),
          .phase_uid = static_cast<int>(system_lp.phase().uid()),
          .num_vars = static_cast<int>(li.get_numcols()),
          .num_constraints = static_cast<int>(li.get_numrows()),
          .stats_nnz = li.lp_stats_nnz(),
          .stats_zeroed = li.lp_stats_zeroed(),
          .stats_max_abs = li.lp_stats_max_abs(),
          .stats_min_abs = li.lp_stats_min_abs(),
          .stats_max_col = static_cast<int>(li.lp_stats_max_col()),
          .stats_min_col = static_cast<int>(li.lp_stats_min_col()),
          .stats_max_col_name = std::string(li.lp_stats_max_col_name()),
          .stats_min_col_name = std::string(li.lp_stats_min_col_name()),
          .row_type_stats =
              [&]
          {
            std::vector<RowTypeStats> rts;
            for (const auto& e : li.lp_row_type_stats()) {
              rts.push_back({
                  .type = e.type,
                  .count = e.count,
                  .nnz = e.nnz,
                  .max_abs = e.max_abs,
                  .min_abs = e.min_abs,
              });
            }
            return rts;
          }(),
      });
    }
  }
  log_lp_stats_summary(lp_entries,
                       planning_lp.options().lp_coeff_ratio_threshold());
}

/// Run the solver and return whether an optimal solution was found.
[[nodiscard]] bool run_solver(PlanningLP& planning_lp)
{
  const spdlog::stopwatch solve_sw;

  const auto& plp_opts_ref = planning_lp.options();
  const auto method = plp_opts_ref.method_type_enum();
  const SolverOptions solver_opts =
      (method == MethodType::sddp || method == MethodType::cascade)
      ? plp_opts_ref.sddp_forward_solver_options()
      : plp_opts_ref.monolithic_solver_options();
  const auto result = planning_lp.resolve(solver_opts);
  const auto solve_elapsed =
      std::chrono::duration<double>(solve_sw.elapsed()).count();
  spdlog::info(std::format("  Optimization time {:.3f}s", solve_elapsed));

  if (result.has_value()) {
    return true;
  }

  const auto& err = result.error();
  // Use warn for non-optimal (e.g. time-limit with feasible incumbent),
  // error only for hard failures such as infeasibility.
  const auto msg = std::format(
      "Solver did not find an optimal solution: "
      "{} (code={})",
      err.message,
      static_cast<int>(err.code));
  if (err.code == ErrorCode::SolverError) {
    spdlog::warn(msg);
  } else {
    spdlog::error(msg);
  }
  // Format optional tolerances: show "(default)" when not set.
  const auto eps_str = [](std::optional<double> v) -> std::string
  { return v ? std::format("{}", *v) : "(default)"; };
  spdlog::error(
      std::format("  Solver options used:"
                  " algorithm={}, threads={}, presolve={},"
                  " optimal_eps={}, feasible_eps={}, barrier_eps={},"
                  " log_level={}",
                  solver_opts.algorithm,
                  solver_opts.threads,
                  solver_opts.presolve,
                  eps_str(solver_opts.optimal_eps),
                  eps_str(solver_opts.feasible_eps),
                  eps_str(solver_opts.barrier_eps),
                  solver_opts.log_level));
  return false;
}

/// Write solution output and save planning JSON to the output directory.
[[nodiscard]] std::expected<void, std::string> write_solution_output(
    PlanningLP& planning_lp)
{
  spdlog::info("=== Output writing ===");
  const spdlog::stopwatch out_sw;
  try {
    planning_lp.write_out();
  } catch (const std::exception& ex) {
    return std::unexpected(std::format("Error writing output: {}", ex.what()));
  }

  // Save the merged planning JSON into the output directory so that
  // post-processing tools have a self-contained reference.
  {
    const auto out_dir = planning_lp.options().output_directory();
    const auto planning_json =
        (std::filesystem::path(out_dir) / "planning.json").string();
    auto pj_result = write_json_output(planning_lp.planning(), planning_json);
    if (pj_result) {
      spdlog::info("  planning JSON saved to {}", planning_json);
    } else {
      spdlog::warn("  failed to save planning JSON: {}", pj_result.error());
    }
  }

  spdlog::info(
      std::format("  Write output time {:.3f}s", out_sw.elapsed().count()));
  return {};
}

}  // namespace

[[nodiscard]] std::expected<int, std::string> gtopt_main(
    const MainOptions& raw_opts)
{
  // Resolve directory arguments and auto-detect CWD context
  auto resolved = resolve_planning_args(raw_opts);
  if (!resolved) {
    return std::unexpected(std::move(resolved.error()));
  }
  const auto& opts = *resolved;

  setup_trace_log(opts);

  const auto strict_parsing = !opts.fast_parsing.value_or(false);
  if (!strict_parsing) {  // NOLINT
    spdlog::info("using fast json parsing");
  }

  try {
    // Parse planning JSON files
    auto parse_result = parse_planning_files(opts.planning_files,
                                             strict_parsing,
                                             opts.check_json.value_or(false),
                                             opts.input_directory);
    if (!parse_result) {
      return std::unexpected(std::move(parse_result.error()));
    }
    Planning my_planning = std::move(*parse_result);

    // Apply options, load user constraints, and validate
    auto cfg = configure_planning(my_planning, opts);
    if (!cfg) {
      return std::unexpected(std::move(cfg.error()));
    }
    if (*cfg != 0) {
      return *cfg;
    }

    // Write JSON output if requested
    if (opts.json_file) {
      auto wr = write_json_output(my_planning, opts.json_file.value());
      if (!wr) {
        return std::unexpected(std::move(wr.error()));
      }
    }

    // Build the LP model
    try {
      const bool do_stats = opts.print_stats.value_or(true)
          || my_planning.options.lp_build_options.compute_stats.value_or(false);
      // CLI --lp-names-level overrides --set; fall back to merged planning
      // value.
      const auto eff_names_level = opts.lp_names_level
          ? opts.lp_names_level
          : my_planning.options.lp_build_options.names_level;
      const auto flat_opts = make_lp_build_options(
          eff_names_level,
          opts.matrix_eps,
          do_stats,
          opts.solver,
          my_planning.options.lp_build_options.equilibration_method);

      const auto flat_opts = prepare_lp_build(my_planning, opts, do_stats);

      spdlog::info("=== Building LP model ===");
      const spdlog::stopwatch build_sw;
      PlanningLP planning_lp {std::move(my_planning),  // NOLINT
                              flat_opts};
      spdlog::info(
          std::format("  Build lp time {:.3f}s", build_sw.elapsed().count()));

      if (opts.lp_file) {
        planning_lp.write_lp(opts.lp_file.value());
      }

      if (do_stats) {
        log_lp_coefficient_stats(planning_lp);
      }

      // lp_build: skip solving if only LP assembly was requested
      if (opts.lp_build.value_or(false) || planning_lp.options().lp_build()) {
        spdlog::info("lp_build: all LP matrices built, skipping solve");
        return 0;
      }

      // Solve the LP
      spdlog::info("=== System optimization ===");
      const bool optimal = run_solver(planning_lp);

      if (do_stats) {
        log_post_solve_stats(planning_lp, optimal);
      }

      if (!optimal) {
        return 1;
      }

      // Write solution output
      if (auto wr = write_solution_output(planning_lp); !wr) {
        return std::unexpected(std::move(wr.error()));
      }

      return 0;

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Error during LP creation or solving: {}", ex.what()));
    }
  } catch (const std::exception& ex) {
    return std::unexpected(
        std::format("Unexpected critical error: {}", ex.what()));
  } catch (...) {
    return std::unexpected("Unknown critical error occurred");
  }
}

}  // namespace gtopt
