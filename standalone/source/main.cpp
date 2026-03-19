#include <iostream>
#include <string>

#include <gtopt/app_options.hpp>
#include <gtopt/gtopt_main.hpp>
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

    std::vector<std::string> system_files;
    if (vm.contains("system-file")) {
      system_files = vm["system-file"].as<std::vector<std::string>>();
    } else {
      std::cerr << "ERROR: a system file is needed, use --help" << '\n';
      return 2;  // input error
    }

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
    auto result =
        gtopt::gtopt_main(parse_main_options(vm, std::move(system_files)));
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
