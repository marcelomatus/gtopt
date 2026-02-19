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
#include <spdlog/stopwatch.h>

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
      std::cerr << "a system file is needed, use --help" << '\n';
      return 0;
    }

    const auto lp_file = get_opt<std::string>(vm, "lp-file");
    const auto json_file = get_opt<std::string>(vm, "json-file");
    const auto quiet = get_opt<bool>(vm, "quiet");
    const auto use_single_bus = get_opt<bool>(vm, "use-single-bus");
    const auto use_kirchhoff = get_opt<bool>(vm, "use-kirchhoff");
    const auto use_lp_names = get_opt<int>(vm, "use-lp-names");
    const auto matrix_eps = get_opt<double>(vm, "matrix-eps");
    const auto just_create = get_opt<bool>(vm, "just-create");
    const auto fast_parsing = get_opt<bool>(vm, "fast-parsing");
    const auto input_directory = get_opt<std::string>(vm, "input-directory");
    const auto output_directory = get_opt<std::string>(vm, "output-directory");
    const auto output_format = get_opt<std::string>(vm, "output-format");
    const auto compression_format =
        get_opt<std::string>(vm, "compression-format");
    const auto input_format = get_opt<std::string>(vm, "input-format");

    //
    // LOG system configuration
    //
    {
      spdlog::cfg::load_env_levels();

      spdlog::set_level(spdlog::level::info);
      if (quiet.value_or(false)) {
        spdlog::set_level(spdlog::level::off);
      } else if (!vm.contains("verbose")) {
        spdlog::set_level(spdlog::level::trace);
      }

      spdlog::cfg::load_argv_levels(argc, argv);

      spdlog::info(std::format("starting gtopt {}", GTOPT_VERSION));
    }

    //
    // dispatch the real main function
    //
    int result_value = 0;
    if (auto result = gtopt::gtopt_main(std::span {system_files},
                           input_directory,
                           input_format,
                           output_directory,
                           output_format,
                           compression_format,
                           use_single_bus,
                           use_kirchhoff,
                           lp_file,
                           use_lp_names,
                           matrix_eps,
                           json_file,
                           just_create,
                           fast_parsing))
    {
      result_value = 0;
    } else {
      spdlog::critical(result.error());
      result_value = 1;
    }
    return result_value;
  } catch (const std::exception& ex) {
    try {
      spdlog::critical(std::format("Exception: {}", ex.what()));
    } catch (...) {
      spdlog::critical(ex.what());
    }
    return 1;
  } catch (...) {
    spdlog::critical("Unknown exception");
    return 1;
  }
}
