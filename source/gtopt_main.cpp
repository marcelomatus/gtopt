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
#include <cstdint>
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

/// Resolve the directory used for log/trace files.
///
/// Prefers `opts.log_directory` when set; otherwise falls back to
/// `<output_directory>/logs` (with `output_directory` itself defaulting
/// to "output").  Does *not* create the directory — callers do that.
[[nodiscard]] std::filesystem::path resolve_log_dir(const MainOptions& opts)
{
  if (opts.log_directory.has_value()) {
    return {opts.log_directory.value()};
  }
  return std::filesystem::path(opts.output_directory.value_or("output"))
      / "logs";
}

/// Pick the run number `N` shared by every log file produced this run.
///
/// Scans @p log_dir for the smallest `N >= 1` such that neither
/// `gtopt_N.log` nor `trace_N.log` already exists, so a single run
/// pairs `gtopt_3.log` with `trace_3.log` (matching N — the user
/// shouldn't have to cross-reference two different counters).
///
/// The result is cached in a function-local static so the second
/// caller (typically `setup_trace_log` after `setup_file_logging`)
/// reuses the same N even if the first caller has already created
/// `gtopt_N.log` on disk.  Within one process @p log_dir is constant,
/// so the cache is safe.
///
/// When every slot is taken, falls back to a timestamp-derived number.
/// Failures creating the directory only emit a warning.
[[nodiscard]] int pick_run_number(const std::filesystem::path& log_dir)
{
  static int cached = 0;
  if (cached != 0) {
    return cached;
  }
  try {
    std::filesystem::create_directories(log_dir);
  } catch (const std::filesystem::filesystem_error& fe) {
    spdlog::warn(
        "could not create log directory '{}': {}", log_dir.string(), fe.what());
  }
  constexpr int max_files = 10000;
  for (int n = 1; n <= max_files; ++n) {
    const auto gtopt_path = log_dir / std::format("gtopt_{}.log", n);
    const auto trace_path = log_dir / std::format("trace_{}.log", n);
    if (!std::filesystem::exists(gtopt_path)
        && !std::filesystem::exists(trace_path))
    {
      cached = n;
      return cached;
    }
  }
  // All slots used — fall back to timestamp-derived run number.
  const auto now = std::chrono::system_clock::now();
  const auto ts = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count());
  cached = static_cast<int>(ts & 0x7fffffffULL);
  return cached;
}

/// Build the path `<log_dir>/<stem>_<N>.log` for the current run.
[[nodiscard]] std::string numbered_log_path(
    const std::filesystem::path& log_dir, std::string_view stem)
{
  return (log_dir / std::format("{}_{}.log", stem, pick_run_number(log_dir)))
      .string();
}

/// Set up trace-level file sink for detailed SPDLOG_TRACE output.
///
/// When --trace-log is given explicitly, uses that path; otherwise
/// auto-creates a `trace_N.log` next to the matching `gtopt_N.log`
/// (same `N`, picked once per run by `pick_run_number()`).
void setup_trace_log(const MainOptions& opts)
{
  const auto log_dir = resolve_log_dir(opts);
  const std::string trace_path = opts.trace_log.has_value()
      ? opts.trace_log.value()
      : numbered_log_path(log_dir, "trace");

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

void setup_file_logging(const MainOptions& opts, bool suppress_stdout)
{
  const auto log_dir = resolve_log_dir(opts);

  // Pick `gtopt_N.log` with the same `N` that `setup_trace_log` will
  // use for `trace_N.log` — so the user can pair the two by suffix.
  const std::string log_path = numbered_log_path(log_dir, "gtopt");

  try {
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        log_path, /*truncate=*/true);
    // Inherit the default logger's level so --verbose / --quiet behave
    // the same on the file as they did on stdout.
    file_sink->set_level(spdlog::default_logger()->level());

    auto& sinks = spdlog::default_logger()->sinks();
    if (suppress_stdout) {
      // Drop every existing sink (typically the auto-installed stdout
      // colour sink) so log output only goes to the file.
      sinks.clear();
    }
    sinks.push_back(std::move(file_sink));
  } catch (const spdlog::spdlog_ex& ex) {
    spdlog::warn("could not open log file '{}': {}", log_path, ex.what());
  }
}

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

  try {
    // Parse planning JSON files
    auto parse_result =
        parse_planning_files(opts.planning_files, opts.input_directory);
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

    // ── Resolve relative input_directory against JSON file location ──
    //
    // When the first planning file resides in a subdirectory (e.g.
    // "subdir/case.json" or "/abs/path/case.json"), relative paths in
    // the JSON—such as input_directory="." or input_directory="data"—
    // must be resolved relative to the JSON file's parent, not CWD.
    // This handles the webservice scenario where a zip is extracted to
    // an input directory and the JSON is in a nested subdirectory.
    //
    // Only applied when the CLI did not explicitly set these directories
    // (opts.*_directory is nullopt), so user CLI overrides are respected.
    if (!opts.planning_files.empty()) {
      namespace fs = std::filesystem;
      const auto json_dir = fs::path(opts.planning_files.front()).parent_path();
      if (!json_dir.empty() && json_dir != ".") {
        auto resolve_if_relative =
            [&](std::optional<std::string>& json_dir_opt,
                const std::optional<std::string>& cli_opt,
                const char* default_val)
        {
          if (!cli_opt.has_value()) {
            const auto effective = json_dir_opt.value_or(default_val);
            if (fs::path(effective).is_relative()) {
              json_dir_opt = (json_dir / effective).string();
            }
          }
        };
        resolve_if_relative(
            my_planning.options.input_directory, opts.input_directory, "input");
        resolve_if_relative(my_planning.options.output_directory,
                            opts.output_directory,
                            "output");
      }
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
