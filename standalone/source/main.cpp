#include <iostream>
#include <string>

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
                        .allow_unregistered()
                        .positional(pos_desc);
      po::store(parser, vm);
      po::notify(vm);

      // Warn about any unrecognised options
      for (const auto& opt : parser.unrecognized()) {
        std::cerr << "WARNING: unknown option '" << opt << "' ignored\n";
      }
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
      const auto& registry = gtopt::SolverRegistry::instance();
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

    // Validate --solver early so the user gets a clear error
    if (vm.contains("solver")) {
      const auto& registry = gtopt::SolverRegistry::instance();
      const auto solver = vm["solver"].as<std::string>();
      if (!registry.has_solver(solver)) {
        const auto avail = registry.available_solvers();
        std::cerr << "ERROR: LP solver '" << solver << "' not available.\n";
        if (avail.empty()) {
          std::cerr << "No LP solver plugins found.\n"
                    << "Hints:\n"
                    << "  - Set GTOPT_PLUGIN_DIR to the plugin directory\n"
                    << "  - Ensure libgtopt_solver_osi.so and/or "
                       "libgtopt_solver_highs.so are installed\n";
        } else {
          std::cerr << "Available LP solvers:";
          for (const auto& s : avail) {
            std::cerr << " " << s;
          }
          std::cerr << '\n';
        }
        return 1;
      }
    }

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

      spdlog::info(std::format("starting gtopt {}", GTOPT_VERSION));
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
    // Classify the error string for exit code selection.
    // Input-related errors contain recognizable keywords.
    const auto& err = result.error();
    if (err.contains("not found") || err.contains("not exist")
        || err.contains("Cannot open") || err.contains("parse")
        || err.contains("Invalid") || err.contains("JSON"))
    {
      return 2;  // input error
    }
    return 3;  // internal/solver error
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
