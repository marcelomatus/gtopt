#include <format>
#include <iostream>
#include <set>
#include <string>

#include <gtopt/check_solvers.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json_schema.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/names_registry.hpp>
#include <gtopt/resolve_planning_args.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/unit_registry.hpp>
#include <gtopt/version.hpp>
#include <unistd.h>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#endif

#include <spdlog/cfg/argv.h>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

using namespace gtopt;

// ---- jemalloc runtime configuration ----
// When jemalloc is linked (GTOPT_HAVE_JEMALLOC, set by CMake), bake a good
// default into the binary via jemalloc's `malloc_conf` symbol.
// `background_thread:true` runs a dedicated purge thread so freed pages are
// returned to the OS OFF the allocating hot path.  Measured on the 2-year
// Maule/Laja SDDP parity case (CPLEX, one iteration): peak RSS 35.6 -> 25.5 GB
// (-28%) and wall 19:47 -> 16:43 (-15%).  Without it, jemalloc's stock default
// (background_thread:false, dirty_decay_ms:10000) purges inline and bloats RSS
// to glibc-like levels — i.e. merely *linking* jemalloc buys little; the
// background thread is what delivers the win.  Reducing narenas / percpu_arena
// / shorter decay added nothing (or hurt wall) in the sweep, so we ship only
// this one knob.
//
// The `MALLOC_CONF` environment variable still overrides this (higher priority
// than the symbol), so operators can tune further at runtime.
// extern "C" gives the exact unmangled name jemalloc looks up in the dynamic
// symbol table; [[gnu::used, gnu::retain]] keeps it under --gc-sections.
#if defined(GTOPT_HAVE_JEMALLOC)
extern "C" [[gnu::used, gnu::retain]] const char* malloc_conf =
    "background_thread:true";
#endif

int main(int argc, char** argv)
{
  // Install the async default-logger wrapper at the very first
  // opportunity — BEFORE any `spdlog::set_level` / `spdlog::set_pattern` /
  // env-level load.  Otherwise the short window between `main()` entry
  // and `gtopt_main()` installing the wrapper runs through the
  // synchronous default logger, which serialises every log call on the
  // sink mutex.  If a background monitor thread (CPU sampler, signal
  // handler) fires during that window it would contend with the main
  // thread's setup-time `spdlog::info(...)` calls.
  //
  // Idempotent: a second call from inside `gtopt_main` is a no-op.
  //
  // Note: this unconditionally installs async even when the user passes
  // `--no-async-logger` / `--trace-log`, because CLI parsing has not
  // happened yet.  `gtopt_main` will switch the default logger back to
  // sync once the options are parsed.
  gtopt::install_async_default_logger();

  //
  // process the command options
  //

  try {
    auto desc = make_options_description();

    po::positional_options_description pos_desc;
    pos_desc.add("system-file", -1);

    po::variables_map vm;
    try {
      auto parser = po::command_line_parser(argc, argv)
                        .options(desc)
                        .positional(pos_desc);
      po::store(parser, vm);
      po::notify(vm);
    } catch (po::parse_error& e) {
      std::cout << "ERROR: " << e.what() << "\n";
      std::cout << desc << "\n";
      return 1;
    }

    if (vm.contains("verbose")) {
      spdlog::set_level(spdlog::level::trace);
    }

    if (vm.contains("help")) {
      std::cout << desc << '\n';
      std::cout << R"(Examples
========

  Run a planning JSON file (default: monolithic, multi-bus, all auto):
    gtopt case.json

  Run on a directory ('case_dir/case_dir.json' is auto-detected):
    gtopt case_dir/

  SDDP with the compressed memory mode (releases solver between phases,
  keeps a compressed flat-LP snapshot for fast reconstruct):
    gtopt case.json --memory-saving compress

  Single-bus (copper-plate) mode — ignores all line limits:
    gtopt case.json --set model_options.use_single_bus=true

  Skip Kirchhoff voltage-law constraints (DC OPF only):
    gtopt case.json --set model_options.use_kirchhoff=false

  Pick a specific LP/MIP backend:
    gtopt case.json --solver cplex
    gtopt case.json --solver highs

  Validate input + build the LP without solving (fast schema /
  feasibility check; emits 'Validation: …' warnings + errors):
    gtopt case.json --lp-only

  Suppress all log output to stdout (logs still land in the per-run
  '<output>/logs/gtopt_N.log' file when stdout is non-interactive):
    gtopt case.json --quiet

  Override scalar fields via --set (dotted path, repeatable; values
  are auto-typed bool/int/double/string).  Example: pin SDDP threads
  and disable Kirchhoff in one shot:
    gtopt case.json --set sddp_options.forward_solver_options.threads=8 \
                    --set model_options.use_kirchhoff=false

  Save the merged planning JSON for inspection:
    gtopt case.json --json-file merged_case.json

  Save the assembled LP for solver-side inspection:
    gtopt case.json --lp-file model

  Tighten / loosen SDDP convergence:
    gtopt case.json --set sddp_options.max_iterations=200 \
                    --set sddp_options.convergence_tol=1e-5

  MIP-aware solve with a 1 % gap target and 10-minute wall-clock cap:
    gtopt case.json --mip-gap 0.01 --time-limit 600

  Quick LP-only smoke test on a MIP case (relaxes commitment binaries
  to continuous; gives a lower bound on the true MIP optimum):
    gtopt case.json --no-mip

  Enforce a naming dialect on input + output JSON (warns on
  cross-dialect aliases at parse time, renames canonical keys to the
  dialect's aliases at write time):
    gtopt case.json --naming-dialect plp

  Discover what aliases / units each dialect uses (no run, just
  prints the registry table — pipe to grep / awk to filter):
    gtopt --list-dialects
    gtopt --list-dialects plexos

  Dump the JSON Schema of the input options model (no run; pipe to a
  schema validator or save for editor autocompletion):
    gtopt --json-schema
    gtopt --json-schema options

Outputs
=======

By default gtopt writes its results, logs, and traces under the
configured output directory (CLI: --set output_directory=<dir>;
default: 'output' relative to the case file's parent).  The layout
is:

  <output_directory>/                 results: per-element parquet/csv
                    /solution.csv     scalar solve summary (status,
                                      objective, kappa, per-(scene,
                                      phase) breakdown)
                    /Generator/       generation_sol.parquet, etc.
                    /Bus/             balance_dual.parquet (LMPs), etc.
                    /Reservoir/       efin_sol, …
                    /cuts/            saved Benders cuts (SDDP only)
                    /logs/            per-run log files (see below)

  <output_directory>/logs/gtopt_N.log     INFO/WARN log of run #N
                        /trace_N.log      trace-level log of run #N
                        /error_*.lp[.zst] LP debug files when a
                                          solve goes infeasible

The 'N' suffix is shared between gtopt_N.log and trace_N.log of the
same run (next free integer not present in the directory), so you
can pair the two by suffix when investigating an issue across runs.

When stdout is interactive (a TTY), spdlog also prints INFO+ to the
terminal in addition to writing the log file.  When stdout is
non-interactive (piped, redirected, or run via run_gtopt / CI),
stdout is suppressed entirely and the log file is the canonical
record — `--quiet` further silences the log file too.
)";
      return 0;
    }

    if (vm.contains("version")) {
      std::cout << GTOPT_VERSION << '\n';
      return 0;
    }

    if (vm.contains("solvers")) {
      auto& registry = gtopt::SolverRegistry::instance();
      registry.load_all_plugins();
      const auto solvers = registry.available_solvers();
      if (solvers.empty()) {
        std::cout << "No LP solver plugins found.\n\n";
        std::cout << "Search directories:\n";
        for (const auto& dir : registry.searched_directories()) {
          std::cout << "  " << dir << '\n';
        }
        const auto& errors = registry.load_errors();
        if (!errors.empty()) {
          std::cout << "\nPlugin load errors:\n";
          for (const auto& err : errors) {
            std::cout << "  " << err << '\n';
          }
        }
        std::cout
            << "\nHints:\n"
            << "  - Set GTOPT_PLUGIN_DIR to the directory containing solver "
               "plugin libraries\n"
            << "  - Ensure solver plugins (osi and/or highs) are installed\n"
            << "  - Install COIN-OR (coinor-libcbc-dev) for CLP/CBC support\n"
            << "  - Install HiGHS for HiGHS support\n";
      } else {
        std::cout << "Available LP solvers:\n";
        for (const auto& s : solvers) {
          std::cout << "  " << s << '\n';
        }
      }
      return 0;
    }

    if (vm.contains("check-solvers")) {
      const auto solver_arg =
          get_opt<std::string>(vm, "check-solvers").value_or("");
      const bool is_verbose = vm.contains("verbose");
      if (solver_arg.empty()) {
        // Run tests against every available solver.
        return gtopt::check_all_solvers(is_verbose);
      }
      // Run tests against the specified solver only.
      auto& registry = gtopt::SolverRegistry::instance();
      registry.load_all_plugins();
      if (!registry.has_solver(solver_arg)) {
        const auto avail = registry.available_solvers();
        std::cerr << "ERROR: solver '" << solver_arg << "' not available.\n";
        if (!avail.empty()) {
          std::cerr << "Available:";
          for (const auto& s : avail) {
            std::cerr << ' ' << s;
          }
          std::cerr << '\n';
        }
        return 1;
      }
      const auto report = gtopt::run_solver_tests(solver_arg, is_verbose);
      for (const auto& r : report.results) {
        const char* mark = r.passed ? "✓" : "✗";
        std::cout << std::format(
            "  {} {:<30} {:.3f}s", mark, r.name, r.duration_s);
        if (!r.passed && !r.detail.empty()) {
          std::cout << "\n    " << r.detail;
        }
        std::cout << '\n';
      }
      std::cout << std::format(
          "\n  {} passed, {} failed\n", report.n_passed(), report.n_failed());
      return report.passed() ? 0 : 1;
    }

    if (vm.contains("list-dialects")) {
      // Diagnostic dump of the naming + unit dictionaries.  Format is
      // one row per (canonical, dialect, alias, unit) tuple, tab-
      // separated for grep/awk friendliness.  The optional `--list-
      // dialects <name>` filter restricts the output to a single
      // dialect — useful when migrating from one tool to another.
      //
      // Class context is NOT printed: the global-alias index inside
      // NamesRegistry loses the per-alias class after build, and the
      // class-agnostic unit lookup is enough for the diagnostic
      // use-case (which is "what does PLEXOS call my pmax field?").
      const auto filter =
          get_opt<std::string>(vm, "list-dialects").value_or("");
      const auto& names = gtopt::NamesRegistry::instance();
      const auto& units = gtopt::UnitRegistry::instance();

      // Single forward sweep through the per-alias index also gives us
      // the set of registered dialect names — we use it for an
      // unknown-filter error so `--list-dialects mistype` does not
      // print zero rows silently.
      std::set<std::string> known_dialects;
      for (const auto& [canonical, aliases] : names.canonical_to_aliases()) {
        for (const auto& alias : aliases) {
          if (const auto d = names.dialect_for(alias); d.has_value()) {
            known_dialects.emplace(*d);
          }
        }
      }
      if (!filter.empty() && !known_dialects.contains(filter)) {
        std::cerr << std::format(
            "ERROR: --list-dialects: unknown dialect '{}'.  Known "
            "dialects: ",
            filter);
        bool first = true;
        for (const auto& d : known_dialects) {
          std::cerr << (first ? "" : ", ") << d;
          first = false;
        }
        std::cerr << '\n';
        return 1;
      }

      std::cout << "# canonical\tdialect\talias\tunit\n";
      std::size_t printed = 0;
      for (const auto& [canonical, aliases] : names.canonical_to_aliases()) {
        for (const auto& alias : aliases) {
          const auto dialect = names.dialect_for(alias).value_or("");
          if (!filter.empty() && dialect != filter) {
            continue;
          }
          const auto unit =
              units.class_agnostic_unit_for(canonical, dialect).value_or("");
          std::cout << std::format(
              "{}\t{}\t{}\t{}\n", canonical, dialect, alias, unit);
          ++printed;
        }
      }
      if (filter.empty()) {
        std::cout << std::format(
            "\n# {} rows ({} canonicals, {} dialects, names from {}, "
            "units from {})\n",
            printed,
            names.canonical_to_aliases().size(),
            known_dialects.size(),
            names.source_path().has_value() ? names.source_path()->string()
                                            : std::string("<built-in>"),
            units.source_path().has_value() ? units.source_path()->string()
                                            : std::string("<built-in>"));
      } else {
        std::cout << std::format(
            "\n# {} rows matching dialect '{}'\n", printed, filter);
      }
      return 0;
    }

    if (vm.contains("json-schema")) {
      // Diagnostic dump of the input data-model JSON Schema, generated
      // from the daw_json_link contracts.  Mirrors --list-dialects: an
      // empty argument dumps the full Planning schema; a name selects a
      // known top-level type; an unknown name is reported on stderr (by
      // dump_json_schema, which then returns an empty string) and exits
      // non-zero.
      const auto name = get_opt<std::string>(vm, "json-schema").value_or("");
      const auto schema = gtopt::dump_json_schema(name);
      if (schema.empty()) {
        return 1;
      }
      std::cout << schema << '\n';
      return 0;
    }

    // --solver validation is handled lazily: SolverRegistry::create()
    // will attempt to load only the requested plugin on demand.  If the
    // solver is unavailable, create() throws a clear error listing all
    // available solvers (after loading all pending plugins).

    std::vector<std::string> system_files;
    if (vm.contains("system-file")) {
      system_files = vm["system-file"].as<std::vector<std::string>>();
    }
    // When no files are given, gtopt_main() will try to auto-detect a
    // planning JSON in the current directory (convention:
    // dirname/dirname.json).

    //
    // Exit codes:
    //   0 = success (optimal solution)
    //   1 = non-optimal solution (infeasible/abandoned but no critical error)
    //   2 = input error (missing file, invalid JSON, bad options)
    //   3 = internal error (unexpected exception, solver crash)
    //
    // Build MainOptions from CLI, then merge config-file defaults.  We
    // need these *before* setting up logging so the file-sink redirect
    // (when stdout is not a TTY) knows where the resolved
    // output/log directory lives.
    auto main_opts = parse_main_options(vm, std::move(system_files));
    merge_config_defaults(main_opts, load_gtopt_config());

    //
    // LOG system configuration
    //
    const auto quiet = get_opt<bool>(vm, "quiet").value_or(false);
    {
      spdlog::cfg::load_env_levels();

      // Use a clean pattern without source file/line information.
      spdlog::set_pattern("[%H:%M:%S.%e] %v");

      // Default: info.  --verbose: trace.  --quiet: off.
      if (quiet) {
        spdlog::set_level(spdlog::level::off);
      } else if (vm.contains("verbose")) {
        spdlog::set_level(spdlog::level::trace);
      } else {
        spdlog::set_level(spdlog::level::info);
      }

      spdlog::cfg::load_argv_levels(argc, argv);
    }

    //
    // Non-interactive stdout (run via run_gtopt, CI, redirection, …):
    // suppress every spdlog message on stdout and route them only to
    // <output_directory>/logs/gtopt.log.  The calling process gets a
    // clean stdout and a single canonical log file — no duplicate
    // tee-ing required on the Python side.
    //
    // We pre-resolve directory arguments here so the log file lands
    // next to the rest of the output (e.g. case_dir/results/logs/...);
    // gtopt_main() repeats resolve_planning_args() and is idempotent.
    //
    if (!quiet && ::isatty(STDOUT_FILENO) == 0) {
      if (auto resolved = gtopt::resolve_planning_args(main_opts); resolved) {
        main_opts = std::move(*resolved);
      }
      gtopt::setup_file_logging(main_opts, /*suppress_stdout=*/true);
    }

    spdlog::info("starting gtopt {}", GTOPT_VERSION);

    auto result = gtopt::gtopt_main(main_opts);
    if (result.has_value()) {
      gtopt::flush_default_logger_best_effort();
      return *result;  // 0 = optimal, 1 = non-optimal
    }
    spdlog::critical(result.error());
    const int exit_code = classify_error_exit_code(result.error());
    gtopt::flush_default_logger_best_effort();
    return exit_code;
  } catch (const std::exception& ex) {
    try {
      spdlog::critical("Exception: {}", ex.what());
    } catch (...) {
      spdlog::critical(ex.what());
    }
    gtopt::flush_default_logger_best_effort();
    return 3;  // internal error
  } catch (...) {
    spdlog::critical("Unknown exception");
    gtopt::flush_default_logger_best_effort();
    return 3;  // internal error
  }
}
