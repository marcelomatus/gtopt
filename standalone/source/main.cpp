#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <boost/program_options.hpp>
#include <daw/daw_read_file.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/version.h>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#include <spdlog/cfg/argv.h>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace
{
using namespace gtopt;

int Main(const std::vector<std::string>& planning_files,
         const std::optional<std::string>& input_directory,
         const std::optional<std::string>& input_format,
         const std::optional<std::string>& output_directory,
         const std::optional<std::string>& output_format,
         const std::optional<std::string>& compression_format,
         const std::optional<bool>& use_single_bus,
         const std::optional<std::string>& lp_file,
         const std::optional<int>& use_lp_names,
         const std::optional<double>& matrix_eps,
         const std::optional<std::string>& json_file,
         const std::optional<bool>& just_create,
         const std::optional<bool>& fast_parsing)
{
  //
  // parsing the system from json
  //
  static constexpr auto FastParsePolicy = daw::json::options::parse_flags<
      daw::json::options::PolicyCommentTypes::hash,
      daw::json::options::CheckedParseMode::no,
      daw::json::options::ExcludeSpecialEscapes::no,
      daw::json::options::UseExactMappingsByDefault::no>;

  static constexpr auto StrictParsePolicy = daw::json::options::parse_flags<
      daw::json::options::PolicyCommentTypes::hash,
      daw::json::options::CheckedParseMode::yes,
      daw::json::options::ExcludeSpecialEscapes::yes,
      daw::json::options::UseExactMappingsByDefault::yes>;

  const auto strict_parsing = !fast_parsing.value_or(false);
  if (!strict_parsing) {
    spdlog::info("using fast json parsing");
  }

  Planning planning;
  {
    spdlog::stopwatch sw;

    for (auto&& planning_file : planning_files) {
      std::filesystem::path fpath(planning_file);
      fpath.replace_extension(".json");
      const auto json_result = daw::read_file(fpath.string());

      if (!json_result) {
        spdlog::critical(
            fmt::format("problem reading input file {}", planning_file));
        return 1;
      }
      spdlog::info(fmt::format("parsing input file {}", fpath.string()));

      auto&& json_doc = json_result.value();
      try {
        if (strict_parsing) {
          auto plan =
              daw::json::from_json<Planning>(json_doc, StrictParsePolicy);
          planning.merge(std::move(plan));
        } else {
          auto plan = daw::json::from_json<Planning>(json_doc, FastParsePolicy);
          planning.merge(std::move(plan));
        }
      } catch (daw::json::json_exception const& jex) {
        spdlog::critical(
            fmt::format("parsing file {} failed, {}",
                        fpath.string(),
                        to_formatted_string(jex, json_doc.c_str())));
      } catch (...) {
        throw;
      }
    }

    spdlog::info(fmt::format("parsing all json files {}", sw));
  }

  //
  // update the planning options
  //
  if (use_single_bus) {
    planning.options.use_single_bus = use_single_bus;
  }

  if (use_lp_names) {
    planning.options.use_lp_names = use_lp_names.value();
  }

  if (output_directory) {
    planning.options.output_directory = output_directory.value();
  }

  if (input_directory) {
    planning.options.input_directory = input_directory.value();
  }

  if (output_format) {
    planning.options.output_format = output_format.value();
  }

  if (compression_format) {
    planning.options.compression_format = compression_format.value();
  }

  if (input_format) {
    planning.options.input_format = input_format.value();
  }

  //

  if (json_file) {
    spdlog::stopwatch sw;

    std::filesystem::path jpath(json_file.value());
    jpath.replace_extension(".json");
    std::ofstream jfile(jpath);
    if (jfile) {
      jfile << daw::json::to_json(planning) << '\n';
    } else {
      spdlog::error(fmt::format("can't create json file {}", jpath.string()));
    }

    spdlog::info(fmt::format("writing system json file {}", sw));
  }

  //
  // create and load the lp
  //

  const auto eps = matrix_eps.value_or(0);  // Default to exact matching
  const auto lp_names = use_lp_names.value_or(0);  // Default to no names

  FlatOptions flat_opts;
  flat_opts.eps = eps;  // Coefficient epsilon tolerance
  flat_opts.col_with_names = lp_names > 0;  // Include variable names?
  flat_opts.row_with_names = lp_names > 0;  // Include constraint names?
  flat_opts.col_with_name_map = lp_names > 1;  // Include variable name mapping?
  flat_opts.row_with_name_map =
      lp_names > 1;  // Include constraint name mapping?
  flat_opts.reserve_matrix = false;  // Don't pre-reserve matrix (already done)
  flat_opts.reserve_factor = 2;  // Memory reservation factor

  PlanningLP planning_lp {planning, flat_opts};

  if (lp_file) {
    planning_lp.write_lp(lp_file.value());
  }

  if (just_create.value_or(false)) {
    return 0;
  }

  bool optimal = false;
  {
    spdlog::stopwatch sw;

    auto result = planning_lp.run_lp();
    spdlog::info(fmt::format("planning  {}", sw));

    optimal = result.has_value();
  }

  if (optimal) {
    spdlog::stopwatch sw;

    planning_lp.write_out();
    spdlog::info(fmt::format("writing output  {}", sw));
  }

  return optimal ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
  //
  // process the command options
  //

  namespace po = boost::program_options;

  try {
    po::options_description desc("Gtoptp options");
    desc.add_options()("help,h", "describes arguments")  //
        ("verbose,v", "activates maximun verbosity")  //
        ("quiet,q",
         po::value<bool>()->implicit_value(true),
         "do not log in the stdout")  //
        ("version,V", "shows program version")  //
        ("system-file,s",
         po::value<std::vector<std::string>>(),
         "name of the system file")  //
        ("lp-file,l",
         po::value<std::string>(),
         "name of the lp file to save")  //
        ("json-file,j",
         po::value<std::string>(),
         "name of the json file to save")  //
        ("input-directory,D", po::value<std::string>(), "input directory")  //
        ("input-format,F", po::value<std::string>(), "input format")  //
        ("output-directory,d",
         po::value<std::string>(),
         "output directory")  //
        ("output-format,f",
         po::value<std::string>(),
         "output format [parquet, csv]")  //
        ("compression-format,C",
         po::value<std::string>(),
         "compression format in parquet [uncompressed, gzip, zstd, lzo]")  //
        ("use-single-bus,b",
         po::value<bool>()->implicit_value(true),
         "use single bus mode")  //
        ("use-lp-names,n",
         po::value<int>()->implicit_value(1),
         "use real col/row names in the lp file")  //
        ("matrix-eps,e",
         po::value<double>(),
         "eps value to define A matrix non-zero values")  //
        ("just-create,c",
         po::value<bool>()->implicit_value(true),
         "just create the problem, then exit")  //
        ("fast-parsing,p",
         po::value<bool>()->implicit_value(true),
         "use fast (non strict) json parsing");

    po::positional_options_description pos_desc;
    pos_desc.add("system-file", -1);

    po::variables_map vm;
    try {
      po::store(po::command_line_parser(argc, argv)
                    .options(desc)
                    .allow_unregistered()
                    .positional(pos_desc)
                    .run(),
                vm);
      po::notify(vm);
    } catch (boost::program_options::error& e) {
      std::cout << "ERROR: " << e.what() << "\n";
      std::cout << desc << "\n";
      return 1;
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

    std::optional<std::string> lp_file;
    if (vm.contains("lp-file")) {
      lp_file = vm["lp-file"].as<std::string>();
    }

    std::optional<std::string> json_file;
    if (vm.contains("json-file")) {
      json_file = vm["json-file"].as<std::string>();
    }

    std::optional<bool> quiet;
    if (vm.contains("quiet")) {
      quiet = vm["quiet"].as<bool>();
    }
    std::optional<bool> use_single_bus;
    if (vm.contains("use-single-bus")) {
      use_single_bus = vm["use-single-bus"].as<bool>();
    }

    std::optional<int> use_lp_names;
    if (vm.contains("use-lp-names")) {
      use_lp_names = vm["use-lp-names"].as<int>();
    }

    std::optional<double> matrix_eps;
    if (vm.contains("matrix-eps")) {
      matrix_eps = vm["matrix-eps"].as<double>();
    }

    std::optional<bool> just_create;
    if (vm.contains("just-create")) {
      just_create = vm["just-create"].as<bool>();
    }

    std::optional<bool> fast_parsing;
    if (vm.contains("fast-parsing")) {
      fast_parsing = vm["fast-parsing"].as<bool>();
    }

    std::optional<std::string> input_directory;
    if (vm.contains("input-directory")) {
      input_directory = vm["input-directory"].as<std::string>();
    }

    std::optional<std::string> output_directory;
    if (vm.contains("output-directory")) {
      output_directory = vm["output-directory"].as<std::string>();
    }

    std::optional<std::string> output_format;
    if (vm.contains("output-format")) {
      output_format = vm["output-format"].as<std::string>();
    }

    std::optional<std::string> compression_format;
    if (vm.contains("compression-format")) {
      compression_format = vm["compression-format"].as<std::string>();
    }

    std::optional<std::string> input_format;
    if (vm.contains("input-format")) {
      input_format = vm["input-format"].as<std::string>();
    }

    //
    // LOG system configuration
    //
    {
      spdlog::cfg::load_env_levels();
      spdlog::cfg::load_argv_levels(argc, argv);

      spdlog::info(fmt::format("starting gtopt {}", GTOPT_VERSION));
    }

    //
    // dispatch the real main function
    //
    return Main(system_files,
                input_directory,
                input_format,
                output_directory,
                output_format,
                compression_format,
                use_single_bus,
                lp_file,
                use_lp_names,
                matrix_eps,
                json_file,
                just_create,
                fast_parsing);

  } catch (daw::json::json_exception const& jex) {
    spdlog::critical(
        fmt::format("exception thrown by json parser: {}", jex.reason()));
  } catch (std::exception const& ex) {
    spdlog::critical(fmt::format("exception thrown: {}", ex.what()));
  } catch (...) {
    spdlog::critical(fmt::format("unknown exception"));
  }

  return 1;
}
