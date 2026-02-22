/**
 * @file      test_app_options.cpp
 * @brief     Unit tests for application command-line option utilities
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the functions in app_options.hpp:
 * get_opt, make_options_description, apply_cli_options, and make_flat_options.
 */

#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/app_options.hpp>

using namespace gtopt;

// ---- Helper to parse command-line args into a variables_map ----
namespace
{
po::variables_map parse_args(const std::vector<std::string>& args,
                             const po::options_description& desc)
{
  po::positional_options_description
      pos_desc;  // NOLINT(misc-const-correctness)
  pos_desc.add("system-file", -1);

  po::variables_map vm;
  auto parser = po::command_line_parser(args)
                    .options(desc)
                    .allow_unregistered()
                    .positional(pos_desc);
  po::store(parser, vm);
  po::notify(vm);
  return vm;
}
}  // namespace
// ---- Tests for get_opt ----

TEST_CASE("get_opt - returns value when present")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--output-directory", "/tmp/out"}, desc);

  auto result = get_opt<std::string>(vm, "output-directory");
  REQUIRE(result.has_value());
  CHECK((result && *result == "/tmp/out"));
}

TEST_CASE("get_opt - returns nullopt when absent")
{
  auto desc = make_options_description();
  auto vm = parse_args({}, desc);

  auto result = get_opt<std::string>(vm, "output-directory");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("get_opt - works with bool type")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--use-single-bus"}, desc);

  auto result = get_opt<bool>(vm, "use-single-bus");
  REQUIRE(result.has_value());
  CHECK((result && *result == true));
}

TEST_CASE("get_opt - works with int type")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--use-lp-names", "2"}, desc);

  auto result = get_opt<int>(vm, "use-lp-names");
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) == 2);
}

TEST_CASE("get_opt - works with double type")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--matrix-eps", "0.001"}, desc);

  auto result = get_opt<double>(vm, "matrix-eps");
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1.0) == doctest::Approx(0.001));
}

TEST_CASE("get_opt - implicit bool value")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--use-kirchhoff"}, desc);

  auto result = get_opt<bool>(vm, "use-kirchhoff");
  REQUIRE(result.has_value());
  CHECK(result.value_or(false) == true);
}

TEST_CASE("get_opt - implicit int value")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--use-lp-names"}, desc);

  auto result = get_opt<int>(vm, "use-lp-names");
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) == 1);
}

// ---- Tests for make_options_description ----

TEST_CASE("make_options_description - contains expected options")
{
  auto desc = make_options_description();

  // Verify key options are registered by parsing them
  CHECK_NOTHROW(parse_args({"--help"}, desc));
  CHECK_NOTHROW(parse_args({"--version"}, desc));
  CHECK_NOTHROW(parse_args({"--verbose"}, desc));
  CHECK_NOTHROW(parse_args({"--quiet"}, desc));
  CHECK_NOTHROW(parse_args({"--use-single-bus"}, desc));
  CHECK_NOTHROW(parse_args({"--use-kirchhoff"}, desc));
  CHECK_NOTHROW(parse_args({"--just-create"}, desc));
  CHECK_NOTHROW(parse_args({"--fast-parsing"}, desc));
}

TEST_CASE("make_options_description - short options work")
{
  auto desc = make_options_description();

  auto vm = parse_args({"-b"}, desc);
  CHECK(vm.contains("use-single-bus"));

  vm = parse_args({"-k"}, desc);
  CHECK(vm.contains("use-kirchhoff"));

  vm = parse_args({"-n", "1"}, desc);
  CHECK(vm.contains("use-lp-names"));

  vm = parse_args({"-e", "0.01"}, desc);
  CHECK(vm.contains("matrix-eps"));
}

TEST_CASE("make_options_description - positional system-file")
{
  auto desc = make_options_description();
  auto vm = parse_args({"my_system.json"}, desc);

  REQUIRE(vm.contains("system-file"));
  auto files = vm["system-file"].as<std::vector<std::string>>();
  REQUIRE(files.size() == 1);
  CHECK(files[0] == "my_system.json");
}

TEST_CASE("make_options_description - multiple positional files")
{
  auto desc = make_options_description();
  auto vm = parse_args({"file1.json", "file2.json"}, desc);

  REQUIRE(vm.contains("system-file"));
  auto files = vm["system-file"].as<std::vector<std::string>>();
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "file1.json");
  CHECK(files[1] == "file2.json");
}

TEST_CASE("make_options_description - string options")
{
  auto desc = make_options_description();
  auto vm = parse_args(
      {
          "--input-directory",
          "/in",
          "--output-directory",
          "/out",
          "--input-format",
          "parquet",
          "--output-format",
          "csv",
          "--compression-format",
          "gzip",
          "--lp-file",
          "model.lp",
          "--json-file",
          "model.json",
      },
      desc);

  CHECK(get_opt<std::string>(vm, "input-directory").value_or("") == "/in");
  CHECK(get_opt<std::string>(vm, "output-directory").value_or("") == "/out");
  CHECK(get_opt<std::string>(vm, "input-format").value_or("") == "parquet");
  CHECK(get_opt<std::string>(vm, "output-format").value_or("") == "csv");
  CHECK(get_opt<std::string>(vm, "compression-format").value_or("") == "gzip");
  CHECK(get_opt<std::string>(vm, "lp-file").value_or("") == "model.lp");
  CHECK(get_opt<std::string>(vm, "json-file").value_or("") == "model.json");
}

// ---- Tests for apply_cli_options ----

TEST_CASE("apply_cli_options - no options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt);

  CHECK_FALSE(planning.options.use_single_bus.has_value());
  CHECK_FALSE(planning.options.use_kirchhoff.has_value());
  CHECK_FALSE(planning.options.use_lp_names.has_value());
  CHECK_FALSE(planning.options.input_directory.has_value());
  CHECK_FALSE(planning.options.input_format.has_value());
  CHECK_FALSE(planning.options.output_directory.has_value());
  CHECK_FALSE(planning.options.output_format.has_value());
  CHECK_FALSE(planning.options.compression_format.has_value());
}

TEST_CASE("apply_cli_options - all options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    true,
                    false,
                    std::optional<int>(2),
                    std::optional<std::string>("/input"),
                    std::optional<std::string>("parquet"),
                    std::optional<std::string>("/output"),
                    std::optional<std::string>("csv"),
                    std::optional<std::string>("gzip"));

  REQUIRE(planning.options.use_single_bus.has_value());
  CHECK((planning.options.use_single_bus
         && *planning.options.use_single_bus == true));

  REQUIRE(planning.options.use_kirchhoff.has_value());
  CHECK((planning.options.use_kirchhoff
         && *planning.options.use_kirchhoff == false));

  REQUIRE(planning.options.use_lp_names.has_value());
  CHECK((planning.options.use_lp_names
         && *planning.options.use_lp_names == true));

  REQUIRE(planning.options.input_directory.has_value());
  CHECK((planning.options.input_directory
         && *planning.options.input_directory == "/input"));

  REQUIRE(planning.options.input_format.has_value());
  CHECK((planning.options.input_format
         && *planning.options.input_format == "parquet"));

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "/output"));

  REQUIRE(planning.options.output_format.has_value());
  CHECK((planning.options.output_format
         && *planning.options.output_format == "csv"));

  REQUIRE(planning.options.compression_format.has_value());
  CHECK((planning.options.compression_format
         && *planning.options.compression_format == "gzip"));
}

TEST_CASE("apply_cli_options - partial options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    true,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::optional<std::string>("/output"),
                    std::nullopt,
                    std::nullopt);

  REQUIRE(planning.options.use_single_bus.has_value());
  CHECK((planning.options.use_single_bus
         && *planning.options.use_single_bus == true));

  CHECK_FALSE(planning.options.use_kirchhoff.has_value());
  CHECK_FALSE(planning.options.use_lp_names.has_value());

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "/output"));

  CHECK_FALSE(planning.options.input_directory.has_value());
}

TEST_CASE("apply_cli_options - does not overwrite existing when nullopt")
{
  Planning planning {};
  planning.options.output_directory = "original_dir";
  planning.options.use_kirchhoff = true;

  apply_cli_options(planning,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt);

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "original_dir"));

  REQUIRE(planning.options.use_kirchhoff.has_value());
  CHECK((planning.options.use_kirchhoff
         && *planning.options.use_kirchhoff == true));
}

TEST_CASE("apply_cli_options - overwrites existing when value provided")
{
  Planning planning {};
  planning.options.output_directory = "original_dir";

  apply_cli_options(planning,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::optional<std::string>("new_dir"),
                    std::nullopt,
                    std::nullopt);

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "new_dir"));
}

// ---- Tests for apply_cli_options(Planning&, const MainOptions&) overload ----

TEST_CASE("apply_cli_options(MainOptions) - no options applied")
{
  Planning planning {};
  apply_cli_options(planning, MainOptions {});

  CHECK_FALSE(planning.options.use_single_bus.has_value());
  CHECK_FALSE(planning.options.use_kirchhoff.has_value());
  CHECK_FALSE(planning.options.use_lp_names.has_value());
  CHECK_FALSE(planning.options.input_directory.has_value());
  CHECK_FALSE(planning.options.output_directory.has_value());
  CHECK_FALSE(planning.options.output_format.has_value());
  CHECK_FALSE(planning.options.compression_format.has_value());
}

TEST_CASE("apply_cli_options(MainOptions) - all options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    MainOptions {
                        .input_directory = "/in",
                        .input_format = "parquet",
                        .output_directory = "/out",
                        .output_format = "csv",
                        .compression_format = "gzip",
                        .use_single_bus = true,
                        .use_kirchhoff = false,
                        .use_lp_names = 2,
                    });

  REQUIRE(planning.options.use_single_bus.has_value());
  CHECK((planning.options.use_single_bus
         && *planning.options.use_single_bus == true));

  REQUIRE(planning.options.use_kirchhoff.has_value());
  CHECK((planning.options.use_kirchhoff
         && *planning.options.use_kirchhoff == false));

  REQUIRE(planning.options.input_directory.has_value());
  CHECK((planning.options.input_directory
         && *planning.options.input_directory == "/in"));

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "/out"));

  REQUIRE(planning.options.output_format.has_value());
  CHECK((planning.options.output_format
         && *planning.options.output_format == "csv"));

  REQUIRE(planning.options.compression_format.has_value());
  CHECK((planning.options.compression_format
         && *planning.options.compression_format == "gzip"));
}

TEST_CASE("apply_cli_options(MainOptions) - does not overwrite when nullopt")
{
  Planning planning {};
  planning.options.output_directory = "existing";
  planning.options.use_kirchhoff = true;

  apply_cli_options(planning, MainOptions {});

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "existing"));

  REQUIRE(planning.options.use_kirchhoff.has_value());
  CHECK((planning.options.use_kirchhoff
         && *planning.options.use_kirchhoff == true));
}

TEST_CASE("apply_cli_options(MainOptions) - overwrites existing when provided")
{
  Planning planning {};
  planning.options.output_directory = "original";

  apply_cli_options(planning, MainOptions {.output_directory = "replaced"});

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "replaced"));
}

// ---- Tests for make_flat_options ----

TEST_CASE("make_flat_options - defaults when both nullopt")
{
  auto opts = make_flat_options(std::nullopt, std::nullopt);

  CHECK(opts.eps == doctest::Approx(0.0));
  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
  CHECK(opts.reserve_matrix == false);
  CHECK(opts.reserve_factor == doctest::Approx(2.0));
}

TEST_CASE("make_flat_options - lp_names level 0 disables names")
{
  auto opts = make_flat_options(std::optional<int>(0), std::nullopt);

  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
}

TEST_CASE("make_flat_options - lp_names level 1 enables names only")
{
  auto opts = make_flat_options(std::optional<int>(1), std::nullopt);

  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
}

TEST_CASE("make_flat_options - lp_names level 2 enables names and maps")
{
  auto opts = make_flat_options(std::optional<int>(2), std::nullopt);

  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == true);
  CHECK(opts.row_with_name_map == true);
}

TEST_CASE("make_flat_options - custom eps value")
{
  auto opts = make_flat_options(std::nullopt, std::optional<double>(0.001));

  CHECK(opts.eps == doctest::Approx(0.001));
}

TEST_CASE("make_flat_options - both parameters provided")
{
  auto opts =
      make_flat_options(std::optional<int>(2), std::optional<double>(1e-6));

  CHECK(opts.eps == doctest::Approx(1e-6));
  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == true);
  CHECK(opts.row_with_name_map == true);
  CHECK(opts.reserve_matrix == false);
  CHECK(opts.reserve_factor == doctest::Approx(2.0));
}

// ---- Integration-style tests combining get_opt with options description ----

TEST_CASE("Integration - parse and extract all option types")
{
  auto desc = make_options_description();
  auto vm = parse_args(
      {
          "system.json",
          "--use-single-bus",
          "--use-lp-names",
          "2",
          "--matrix-eps",
          "0.01",
          "--output-directory",
          "/results",
          "--output-format",
          "parquet",
          "--compression-format",
          "zstd",
      },
      desc);

  auto use_single_bus = get_opt<bool>(vm, "use-single-bus");
  auto use_lp_names = get_opt<int>(vm, "use-lp-names");
  auto matrix_eps = get_opt<double>(vm, "matrix-eps");
  auto output_directory = get_opt<std::string>(vm, "output-directory");
  auto output_format = get_opt<std::string>(vm, "output-format");
  auto compression_format = get_opt<std::string>(vm, "compression-format");

  REQUIRE(use_single_bus.has_value());
  CHECK((use_single_bus && *use_single_bus == true));

  REQUIRE(use_lp_names.has_value());
  CHECK((use_lp_names && *use_lp_names == 2));

  REQUIRE(matrix_eps.has_value());
  CHECK((matrix_eps && *matrix_eps == doctest::Approx(0.01)));

  REQUIRE(output_directory.has_value());
  CHECK((output_directory && *output_directory == "/results"));

  REQUIRE(output_format.has_value());
  CHECK((output_format && *output_format == "parquet"));

  REQUIRE(compression_format.has_value());
  CHECK((compression_format && *compression_format == "zstd"));

  // Apply to planning
  Planning planning {};
  apply_cli_options(planning,
                    use_single_bus,
                    get_opt<bool>(vm, "use-kirchhoff"),
                    use_lp_names,
                    get_opt<std::string>(vm, "input-directory"),
                    get_opt<std::string>(vm, "input-format"),
                    output_directory,
                    output_format,
                    compression_format);

  REQUIRE(planning.options.use_single_bus.has_value());
  CHECK((planning.options.use_single_bus
         && *planning.options.use_single_bus == true));
  CHECK_FALSE(planning.options.use_kirchhoff.has_value());

  // Build flat options
  auto flat_opts = make_flat_options(use_lp_names, matrix_eps);
  CHECK(flat_opts.eps == doctest::Approx(0.01));
  CHECK(flat_opts.col_with_names == true);
  CHECK(flat_opts.col_with_name_map == true);
}
