#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <boost/program_options.hpp>
#include <daw/daw_read_file.h>
#include <gtopt/json/json_system.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/version.h>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#include <spdlog/cfg/argv.h>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace
{
using namespace gtopt;

int Main(const std::vector<std::string>& system_files,
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

  System system;
  {
    spdlog::stopwatch sw;

    for (auto&& system_file : system_files) {
      std::filesystem::path fpath(system_file);
      fpath.replace_extension(".json");
      const auto json_result = daw::read_file(fpath.string());

      if (!json_result) {
        spdlog::critical("problem reading input file {}", system_file);
        return 1;
      }
      spdlog::info("parsing input file {}", fpath.string());

      auto&& json_doc = json_result.value();
      try {
        if (strict_parsing) {
          auto sys = daw::json::from_json<System>(json_doc, StrictParsePolicy);
          system.merge(sys);
        } else {
          auto sys = daw::json::from_json<System>(json_doc, FastParsePolicy);
          system.merge(sys);
        }
      } catch (daw::json::json_exception const& jex) {
        spdlog::critical("parsing file {} failed, {}",
                         fpath.string(),
                         to_formatted_string(jex, json_doc.c_str()));
      } catch (...) {
        throw;
      }
    }

    spdlog::info("parsing all json files {}", sw);
  }

  //
  // update the system options
  //
  if (use_single_bus) {
    system.options.use_single_bus = use_single_bus;
  }

  if (use_lp_names) {
    system.options.use_lp_names = use_lp_names.value();
  }

  if (output_directory) {
    system.options.output_directory = output_directory.value();
  }

  if (input_directory) {
    system.options.input_directory = input_directory.value();
  }

  if (output_format) {
    system.options.output_format = output_format.value();
  }

  if (compression_format) {
    system.options.compression_format = compression_format.value();
  }

  if (input_format) {
    system.options.input_format = input_format.value();
  }

  //

  if (json_file) {
    spdlog::stopwatch sw;

    std::filesystem::path jpath(json_file.value());
    jpath.replace_extension(".json");
    std::ofstream jfile(jpath);
    if (jfile) {
      jfile << daw::json::to_json(system) << '\n';
    } else {
      spdlog::error("can't create json file {}", jpath.string());
    }

    spdlog::info("writing system json file {}", sw);
  }

  //
  // create and load the lp
  //

  SystemLP system_lp {std::move(system)};

  LinearInterface lp_interface;
  {
    spdlog::stopwatch sw;

    constexpr size_t reserve_size = 1'024;
    LinearProblem linear_problem(system_lp.name(), reserve_size);
    system_lp.add_to_lp(linear_problem);
    spdlog::info("lp creation {}", sw);

    const auto eps = matrix_eps.value_or(0);
    const auto lp_names = use_lp_names.value_or(0);
    const FlatOptions flat_opts {.eps = eps,
                                 .col_with_names = lp_names > 0,
                                 .row_with_names = lp_names > 0,
                                 .col_with_name_map = lp_names > 1,
                                 .row_with_name_map = lp_names > 1,
                                 .reserve_matrix = false,
                                 .reserve_factor = 2};

    auto flat_lp = linear_problem.to_flat(flat_opts);
    spdlog::info("lp flattening {}", sw);

    lp_interface.load_flat(flat_lp);

    spdlog::info("lp loading {}", sw);
  }

  if (lp_file) {
    spdlog::stopwatch sw;

    const std::filesystem::path lpath {lp_file.value()};
    lp_interface.write_lp(lpath.stem());

    spdlog::info("lp writing {}", sw);
  }

  if (just_create.value_or(false)) {
    spdlog::info("just creating the problem, exiting now");
    return 0;
  }

  //
  // solve the problem
  //
  {
    spdlog::stopwatch sw;

    const LPOptions lp_opts {};
    const auto status = lp_interface.resolve(lp_opts);

    if (!status) {
      lp_interface.write_lp("error");

      spdlog::error("problem is not feasible, check the error.lp file");

      return 1;
    }

    spdlog::info("lp solving {}", sw);
  }

  //
  // write the output
  //
  {
    spdlog::stopwatch sw;

    system_lp.write_out(lp_interface);

    spdlog::info("write output {}", sw);
  }

  return 0;
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

      spdlog::info("starting gtopt {}", GTOPT_VERSION);
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
    spdlog::critical("exception thrown by json parser: {}", jex.reason());
  } catch (std::exception const& ex) {
    spdlog::critical("exception thrown: {}", ex.what());
  } catch (...) {
    spdlog::critical("unknown exception");
  }

  return 1;
}
