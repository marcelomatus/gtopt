#include <format>
#include <iostream>
#include <string>

#include <gtopt/check_solvers.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/resolve_planning_args.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/version.hpp>
#include <unistd.h>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#endif

#include <spdlog/cfg/argv.h>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

int main(int argc, char** argv)
{
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

  Same with the in-place rebuild mode (lowest steady-state RAM,
  re-flattens from collections on every solve):
    gtopt case.json --memory-saving rebuild

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
      return *result;  // 0 = optimal, 1 = non-optimal
    }
    spdlog::critical(result.error());
    return classify_error_exit_code(result.error());
  } catch (const std::exception& ex) {
    try {
      spdlog::critical("Exception: {}", ex.what());
    } catch (...) {
      spdlog::critical(ex.what());
    }
    return 3;  // internal error
  } catch (...) {
    spdlog::critical("Unknown exception");
    return 3;  // internal error
  }
}
