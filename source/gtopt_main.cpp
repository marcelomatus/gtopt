/**
 * @file      gtopt_main.cpp
 * @brief     Thin orchestrator for the gtopt optimizer
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
 *  - `lp_only`: build all scene/phase LP matrices but skip solving;
 *    validating input without running the solver.
 *  - `json_file`: write the merged Planning to a JSON file before solving.
 *  - `lp_file`: write the flat LP model to a `.lp` file before solving.
 *  - `print_stats`: log pre- and post-solve system statistics.
 *  - `lp_presolve`: enable/disable LP presolve in the solver.
 *  - `lp_debug`: when true, save LP debug files (one per scene/phase for
 *    the monolithic solver; one per iteration/scene/phase for SDDP) to the
 *    `log_directory`.  If `output_compression` is set (e.g. `"gzip"`), the
 *    files are compressed automatically and the originals removed.
 *
 * The heavy DAW JSON and Arrow/Parquet compilation is delegated to
 * gtopt_json_io.cpp and gtopt_lp_runner.cpp respectively, enabling
 * parallel compilation of these three translation units.
 */

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>

#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/gtopt_lp_runner.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/resolve_planning_args.hpp>
#include <gtopt/validate_planning.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

namespace
{

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

  // Propagate lp_only so the SDDP solver also sees it.
  if (opts.lp_only) {
    planning.options.lp_only = opts.lp_only;
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

    // Build, solve, and write output
    return build_solve_and_output(std::move(my_planning), opts);

  } catch (const std::exception& ex) {
    return std::unexpected(
        std::format("Unexpected critical error: {}", ex.what()));
  } catch (...) {
    return std::unexpected("Unknown critical error occurred");
  }
}

}  // namespace gtopt
