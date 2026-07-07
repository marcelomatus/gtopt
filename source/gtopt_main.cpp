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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <utility>

#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/gtopt_lp_runner.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/resolve_planning_args.hpp>
#include <gtopt/validate_planning.hpp>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
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

/// Name of the async default logger we register; reused as the idempotent
/// guard so a second call to ``ensure_default_logger_async`` is a no-op.
constexpr std::string_view kAsyncLoggerName = "gtopt_async";

/// Initialise spdlog's async thread pool exactly once per process.
///
/// All sinks attached to async loggers afterwards dispatch their writes
/// through a background thread pool + bounded message queue, so the
/// application's hot threads never block on disk I/O nor on the sink
/// mutex.  Queue size = 65536 messages, 2 worker threads.
///
/// **Overflow policy** is set to ``overrun_oldest`` in
/// ``ensure_default_logger_async`` (NOT ``block``): under the
/// burst-y log pattern of cascade level transitions (every parallel
/// scene-build emits LP-element log lines simultaneously) a
/// ``block`` policy would freeze producer threads — including the
/// main thread — when the queue fills, deadlocking the solve.
/// Observed on juan/IPLP L1→L2 with the original 8192/1/block
/// config: main thread parked in ``hrtimer_nanosleep`` indefinitely
/// while 71 worker threads waited on the queue futex.  Losing a
/// few oldest log lines under sustained backpressure is strictly
/// preferable to halting the solve.
///
/// The pool must be initialised before any async logger is created;
/// guarded with a function-local flag so subsequent calls are no-ops.
void ensure_async_thread_pool()
{
  static const bool initialised = []
  {
    constexpr std::size_t queue_size = 65536;
    constexpr std::size_t worker_threads = 2;
    spdlog::init_thread_pool(queue_size, worker_threads);
    return true;
  }();
  (void)initialised;
}

/// Replace spdlog's default logger with an `async_logger` wrapping the
/// current sinks.  All subsequent ``spdlog::info(...)`` /
/// ``spdlog::warn(...)`` / etc. calls — including the stdout console
/// stream — dispatch via the background thread pool.  Idempotent: a
/// second call is a no-op (detected by the registered name).
///
/// Must be called early in ``gtopt_main`` so every later sink-attaching
/// helper (``setup_file_logging``, ``setup_trace_log``) just pushes onto
/// the already-async default logger.
void ensure_default_logger_async()
{
  auto current = spdlog::default_logger();
  if (current && current->name() == kAsyncLoggerName) {
    return;  // already async
  }
  ensure_async_thread_pool();
  const auto previous_level = current ? current->level() : spdlog::level::info;
  std::vector<spdlog::sink_ptr> sinks;
  if (current) {
    const auto& current_sinks = current->sinks();
    sinks.assign(current_sinks.begin(), current_sinks.end());
  }
  // ``overrun_oldest``: when the bounded queue is full, the new
  // message overwrites the oldest one rather than blocking the
  // producer thread.  This trades occasional log-line loss under
  // sustained backpressure for guaranteed solver forward
  // progress — see the ``ensure_async_thread_pool`` docstring for
  // the juan/IPLP deadlock that motivated the switch from
  // ``block``.  The 65536-slot queue + 2 workers means the
  // overrun window only opens during truly sustained bursts
  // (level transitions × 16 scenes × many LP-element log lines);
  // normal iteration logging fits comfortably.
  auto async = std::make_shared<spdlog::async_logger>(
      std::string {kAsyncLoggerName},
      sinks.begin(),
      sinks.end(),
      spdlog::thread_pool(),
      spdlog::async_overflow_policy::overrun_oldest);
  async->set_level(previous_level);
  spdlog::set_default_logger(async);
}

/// Set up trace-level file sink for detailed SPDLOG_TRACE output.
///
/// **Opt-in only**: requires the user to pass `--trace-log` / `-T`
/// (with or without a path).  When `trace_log` is unset this function
/// is a no-op — the global spdlog level stays at INFO and no trace
/// file is created.  This avoids the ~MB-scale auto-trace file +
/// global level=trace overhead that previously fired on every run.
///
/// Path resolution:
///   - `-T PATH`  → write to PATH verbatim
///   - `-T`       → write to auto-numbered `<log_dir>/trace_<N>.log`
///                 (same `N` as the matching `gtopt_<N>.log`)
///
/// Assumes the default logger has already been upgraded to async via
/// ``ensure_default_logger_async`` (called at the top of
/// ``gtopt_main``).  Just appends the new trace sink to the existing
/// default logger and raises its level to ``trace``.
void setup_trace_log(const MainOptions& opts)
{
  if (!opts.trace_log.has_value()) {
    return;  // ← gated; keep global level at INFO
  }
  // Empty string means "user passed -T without an argument" → auto-name.
  const std::string trace_path = opts.trace_log.value().empty()
      ? numbered_log_path(resolve_log_dir(opts), "trace")
      : opts.trace_log.value();

  try {
    const auto parent = std::filesystem::path(trace_path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    // NOTE: do NOT call `ensure_default_logger_async()` here.  The
    // gtopt_main entry point already installs the async wrapper; if
    // the caller subsequently asked for sync mode (via
    // `--no-async-logger` or `-T`), `gtopt_main` invoked
    // `switch_to_sync_default_logger()` immediately afterwards.
    // Re-asserting async here would silently undo that switch — and
    // under `-T` it would re-enable the very `overrun_oldest` policy
    // that drops trace lines under load.  Just attach the sink to
    // whichever default logger is currently registered.
    auto trace_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        trace_path, /*truncate=*/true);
    trace_sink->set_level(spdlog::level::trace);
    // Explicit pattern: drop the `[file.cpp:NNN]` source-location field
    // that spdlog's default pattern (`%+`) renders for SPDLOG_INFO /
    // SPDLOG_WARN macros.  End-users don't care about source location;
    // a uniform `[date time] [level] message` is easier to scan and
    // matches the stdout sink's terse pattern (`[%H:%M:%S.%e] %v`).
    // TRACE-level lines emitted via `spdlog::trace(...)` (function form)
    // never carried source-location anyway, so this is a no-op for them.
    trace_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    auto logger = spdlog::default_logger();
    logger->sinks().push_back(std::move(trace_sink));
    logger->set_level(spdlog::level::trace);
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

  // Warn when there is no demand-shedding penalty anywhere in the
  // model.  See `has_no_shedding_penalty` for the full rule.
  if (has_no_shedding_penalty(planning)) {
    spdlog::warn(
        "demand_fail_cost is 0 and no demand carries a `fcost` "
        "or `ecost` field: unserved load has no penalty.  Set "
        "the global `demand_fail_cost > 0` OR a per-demand "
        "`fcost` to penalize load shedding.");
  }

  // Validate planning (referential integrity, ranges, completeness)
  {
    const auto vresult = validate_planning(planning);
    if (!vresult.ok()) {
      return std::unexpected(
          std::format("Planning validation failed with {} error(s)",
                      vresult.errors.size()));
    }
  }

  return 0;
}

}  // namespace

bool has_no_shedding_penalty(const Planning& planning) noexcept
{
  // Global shedding penalty?
  const auto dfc =
      planning.options.model_options.demand_fail_cost.value_or(0.0);
  if (dfc != 0.0) {
    return false;
  }
  // Any per-demand fcost / ecost?
  const auto& demands = planning.system.demand_array;
  const bool any_per_demand_penalty = std::ranges::any_of(
      demands,
      [](const auto& d) { return d.fcost.has_value() || d.ecost.has_value(); });
  return !any_per_demand_penalty;
}

void install_async_default_logger()
{
  ensure_default_logger_async();
}

void switch_to_sync_default_logger()
{
  auto current = spdlog::default_logger();
  if (!current) {
    return;
  }
  if (current->name() != kAsyncLoggerName) {
    return;  // already synchronous
  }
  // Drain any messages already enqueued on the async wrapper before we
  // detach its sinks.  Otherwise messages "logged" up to this point
  // might never reach disk because we're about to drop the only owner
  // of the async_logger handle.
  try {
    current->flush();
  } catch (...) {
  }
  const auto previous_level = current->level();
  std::vector<spdlog::sink_ptr> sinks;
  {
    const auto& current_sinks = current->sinks();
    sinks.assign(current_sinks.begin(), current_sinks.end());
  }
  auto sync = std::make_shared<spdlog::logger>(
      "gtopt_sync", sinks.begin(), sinks.end());
  sync->set_level(previous_level);
  spdlog::set_default_logger(std::move(sync));
}

void flush_default_logger_best_effort() noexcept
{
  try {
    if (auto logger = spdlog::default_logger()) {
      logger->flush();
    }
  } catch (...) {
  }
}

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
    // Match the trace sink: drop source-location, keep date + level.
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    // Assemble the new sink set, then atomically install a FRESH async
    // logger — never mutate the live default logger's `sinks()` vector
    // in place.  Under async logging the backend thread iterates that
    // same vector inside `spdlog::async_logger::backend_sink_it_` while
    // it drains the queue; an in-place `clear()`/`push_back()` from this
    // (producer) thread is an unsynchronised write racing that read and
    // can free a sink mid-`log()`, crashing the backend thread (observed
    // as an intermittent SIGSEGV when many tests share one process,
    // backtrace `thread_pool::process_next_msg_` → `backend_sink_it_` →
    // `base_sink::log` → null vtable).  Swapping in a new logger is the
    // race-free pattern already used by `ensure_default_logger_async` /
    // `switch_to_sync_default_logger`: the outgoing logger and its sinks
    // stay alive via the queued async messages' `shared_ptr` until the
    // backend drains them, while the new logger owns a freshly-built
    // sink vector that no other thread touches yet.  Forces async, as
    // the previous `ensure_default_logger_async()` call did (this is a
    // public API that may run standalone, e.g. from the webservice).
    ensure_async_thread_pool();
    auto current = spdlog::default_logger();
    const auto level = current ? current->level() : spdlog::level::info;

    std::vector<spdlog::sink_ptr> sinks;
    if (suppress_stdout) {
      // Drop the stdout console sink, but keep an err-gated stderr sink
      // so an early failure (apply_set_options rejecting an unknown
      // --set key, missing input file, etc.) still surfaces on the
      // terminal without the user having to find the gtopt_N.log.
      auto stderr_sink =
          std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
      stderr_sink->set_level(spdlog::level::err);
      stderr_sink->set_pattern("gtopt: [%l] %v");
      sinks.push_back(std::move(stderr_sink));
    } else if (current) {
      // Preserve the existing sinks (stdout console, any trace sink, …).
      const auto& current_sinks = current->sinks();
      sinks.assign(current_sinks.begin(), current_sinks.end());
    }
    sinks.push_back(std::move(file_sink));

    // Drain the outgoing logger so its queued tail lands before it is
    // replaced (its sinks live on, via the queued messages, until done).
    if (current) {
      try {
        current->flush();
      } catch (...) {
      }
    }

    auto async = std::make_shared<spdlog::async_logger>(
        std::string {kAsyncLoggerName},
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    async->set_level(level);
    spdlog::set_default_logger(std::move(async));
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

  // Upgrade the default logger to async once, before any sink-attaching
  // helper runs.  All subsequent spdlog::info / warn / trace calls
  // (including the stdout colour sink) dispatch via the background
  // thread pool, so SDDP workers don't block on the sink mutex.
  ensure_default_logger_async();

  // Two cases force the default logger back to synchronous:
  //   1. The user explicitly disabled async via `--no-async-logger` /
  //      `--async-logger=false`.
  //   2. Trace mode is enabled (`--trace-log` / `-T`).  The async
  //      wrapper's bounded queue + `overrun_oldest` policy silently
  //      drops trace bursts, which defeats the purpose of trace mode
  //      (we want EVERY trace line on disk for post-mortem analysis).
  //      Sync mode takes a sink mutex per log call — slower under
  //      load but deterministic.
  // Idempotent: no-op when the default logger is already sync.
  const bool async_disabled = !opts.async_logger.value_or(true);
  const bool trace_enabled = opts.trace_log.has_value();
  if (async_disabled || trace_enabled) {
    switch_to_sync_default_logger();
  }

  setup_trace_log(opts);

  try {
    // Parse planning JSON files.  `opts.naming_dialect` arrives from the
    // CLI flag `--naming-dialect <name>`; when set, the parse-time
    // alias canonicalization emits a once-per-alias warning for any
    // input key whose dialect tag differs.  Empty preserves the silent
    // legacy behaviour.  A dialect set only via JSON (not CLI) is
    // honoured at output rename time but does not trigger this input
    // warn — surfacing that case would require a two-pass parse.
    const std::string_view enforce_dialect = opts.naming_dialect.has_value()
        ? std::string_view {*opts.naming_dialect}
        : std::string_view {};
    auto parse_result = parse_planning_files(
        opts.planning_files, opts.input_directory, enforce_dialect);
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

    // Save pre-solve state snapshot:
    // ``<output_directory>/gtopt_state.json``.
    //
    // The ``<tool>_state.json`` naming convention is shared with the
    // Python converters (``plexos2gtopt_state.json``,
    // ``plp2gtopt_state.json``) so the originating tool is obvious
    // from the filename across all output directories.
    //
    // Captures the FULLY-MERGED Planning (all input ``-s`` files
    // unified + every CLI override applied via ``--set``, ``--no-mip``,
    // ``--solver``, ``--no-scale``, ``--method``, etc.) as a single
    // self-contained JSON the solver is about to consume.
    //
    // Reproducibility contract: re-running ``gtopt -s
    // <output_directory>/gtopt_state.json`` with no other CLI flags
    // produces a byte-identical LP and (modulo solver non-determinism)
    // a byte-identical solution.  The snapshot is the canonical
    // "this is what was solved" record — drop it into a bug report,
    // an audit trail, or a regression-test fixture.
    //
    // Always-on; distinct from the user-facing ``--json-file PATH``
    // (which writes the same JSON to an explicit path the operator
    // chooses).  Distinct from the POST-solve ``planning.json`` sidecar
    // written by ``gtopt_lp_runner`` after solving: that one carries
    // the final state as gtopt left it (which may differ from the
    // pre-solve inputs if any auto-defaults were resolved during
    // build).  For replay use the pre-solve snapshot here.
    //
    // Silent on missing output directory: when ``output_directory`` is
    // unset and no default applies (e.g. ``--lp-only`` smoke tests),
    // skip the snapshot — the LP build is the only meaningful artefact.
    {
      // Resolution order — matches what ``PlanningOptionsLP`` does later
      // when actually writing per-class output, so the snapshot lands
      // next to the LP outputs regardless of how the operator
      // configured the directory:
      //   1. ``my_planning.options.output_directory`` (JSON-side)
      //   2. ``opts.output_directory`` (CLI ``--output-directory``)
      //   3. ``"output"`` (built-in default, same as
      //      ``PlanningOptionsLP::output_directory``)
      const std::string out_dir = my_planning.options.output_directory.value_or(
          opts.output_directory.value_or(std::string {"output"}));
      const auto snapshot_path =
          (std::filesystem::path(out_dir) / "gtopt_state.json").string();
      // Best-effort: ensure the directory exists (the post-solve
      // write_out will recreate it anyway, but we land HERE first).
      std::error_code ec;
      std::filesystem::create_directories(out_dir, ec);
      // Snapshot is SELF-CONTAINED: the PAMPL files were already
      // parsed and merged into ``user_constraint_array`` (+
      // ``user_param_array``) before we got here.  Strip the
      // ``user_constraint_file`` / ``user_constraint_files``
      // references so a reload doesn't re-parse them on top of the
      // already-merged constraints (which would trigger gtopt's
      // unique-name guard, e.g. ``non-unique name Campiche_starting
      // or uid 2018``).  Make a copy so the in-memory planning we
      // pass on to ``build_solve_and_output`` keeps the original
      // file refs intact.
      Planning snapshot_planning = my_planning;
      snapshot_planning.system.user_constraint_file.reset();
      snapshot_planning.system.user_constraint_files.clear();
      auto snap = write_json_output(snapshot_planning, snapshot_path);
      if (snap) {
        spdlog::info("  pre-solve state snapshot: {}", snapshot_path);
      } else {
        spdlog::warn("  failed to save pre-solve state snapshot {}: {}",
                     snapshot_path,
                     snap.error());
      }

      // README.md — self-documenting output-directory guide.  Same
      // ``<tool>_state.json`` convention + reproducibility narrative
      // as the Python converters' READMEs (see
      // ``scripts/gtopt_shared/state_snapshot.py`` ::
      // ``write_gtopt_readme``).  Best-effort; failure to write is a
      // warning, not a fatal.
      const auto readme_path =
          (std::filesystem::path(out_dir) / "README.md").string();
      try {
        std::ofstream f(readme_path, std::ios::trunc);
        f << R"MD(# gtopt output directory

This directory contains the output of a `gtopt` run.

| File / dir | Purpose |
|---|---|
| `planning.json` | Post-solve planning sidecar (Planning as gtopt left it after solving — useful for downstream tooling). |
| `gtopt_state.json` | **Pre-solve state snapshot** — fully-merged Planning (all `-s` inputs unified + every CLI override applied) written immediately BEFORE solve.  See *Reproducing this run* below. |
| `solver_status.json` | One-line solver outcome (status, method, elapsed time, scenes done). |
| `<Class>/` | Per-LP-class output Parquet streams (`Generator/generation_sol.parquet`, `Bus/balance_dual.parquet`, `Line/flowp_sol.parquet`, …).  Which fields appear depends on `--write-out`. |
| `logs/` | Per-(scene, phase) solver logs (`cplex_sc0_ph0.log`, `gtopt_1.log`). |
| `README.md` | This file. |

## Reproducing this run

`gtopt` writes a **pre-solve state snapshot** to `gtopt_state.json` at the
START of every run, capturing the fully-merged Planning (all `-s` input
files unified + every CLI override applied — `--set`, `--no-mip`,
`--solver`, `--no-scale`, `--method`, etc.).

### Re-run with the same options

```bash
gtopt -s gtopt_state.json
```

The snapshot is self-contained; PAMPL `user_constraint_file` references
are stripped before write (constraints are inlined into
`user_constraint_array`), so a reload doesn't re-parse them on top of
already-merged constraints.

### Reproducibility contract

Re-running with no other CLI flags produces a **stable** LP across
iterations: snapshot N=2 vs snapshot N=3 written from a snapshot reload
are byte-identical, including the per-block LP coefficients.  Use the
snapshot for bug reports, audit trails, and regression-test fixtures.

### Inspect the snapshot

```bash
jq '.options' gtopt_state.json
jq '.simulation' gtopt_state.json
jq '.system | keys' gtopt_state.json
```
)MD";
        if (f) {
          spdlog::info("  output README: {}", readme_path);
        }
      } catch (const std::exception& ex) {
        spdlog::warn("  failed to write README {}: {}", readme_path, ex.what());
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
