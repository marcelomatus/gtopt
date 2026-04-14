#include <format>
#include <iostream>
#include <string>

#include <gtopt/check_solvers.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/version.hpp>

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
    // LOG system configuration
    //
    {
      spdlog::cfg::load_env_levels();

      // Use a clean pattern without source file/line information.
      spdlog::set_pattern("[%H:%M:%S.%e] %v");

      // Default: info.  --verbose: trace.  --quiet: off.
      const auto quiet = get_opt<bool>(vm, "quiet");
      if (quiet.value_or(false)) {
        spdlog::set_level(spdlog::level::off);
      } else if (vm.contains("verbose")) {
        spdlog::set_level(spdlog::level::trace);
      } else {
        spdlog::set_level(spdlog::level::info);
      }

      spdlog::cfg::load_argv_levels(argc, argv);

      spdlog::info("starting gtopt {}", GTOPT_VERSION);
    }

    //
    // Exit codes:
    //   0 = success (optimal solution)
    //   1 = non-optimal solution (infeasible/abandoned but no critical error)
    //   2 = input error (missing file, invalid JSON, bad options)
    //   3 = internal error (unexpected exception, solver crash)
    //
    // dispatch the real main function
    //
    // Build MainOptions from CLI, then merge config-file defaults
    auto main_opts = parse_main_options(vm, std::move(system_files));
    merge_config_defaults(main_opts, load_gtopt_config());

    auto result = gtopt::gtopt_main(main_opts);
    if (result.has_value()) {
      return *result;  // 0 = optimal, 1 = non-optimal
    }
    spdlog::critical(result.error());
    return classify_error_exit_code(result.error());
  } catch (const std::exception& ex) {
    try {
      spdlog::critical(std::format("Exception: {}", ex.what()));
    } catch (...) {
      spdlog::critical(ex.what());
    }
    return 3;  // internal error
  } catch (...) {
    spdlog::critical("Unknown exception");
    return 3;  // internal error
  }
}
