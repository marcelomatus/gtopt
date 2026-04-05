/**
 * @file      test_config_file.hpp
 * @brief     Unit tests for config_file.hpp INI parser
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/config_file.hpp>
#include <gtopt/main_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("parse_ini_file - Basic sections and key-value pairs")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp =
      std::filesystem::temp_directory_path() / "test_ini_basic.ini";
  {
    std::ofstream f(tmp);
    f << "[global]\n"
      << "ai_enabled = false\n"
      << "\n"
      << "[gtopt]\n"
      << "solver = highs\n"
      << "threads = 4\n"
      << "algorithm = barrier\n"
      << "output-format = parquet\n";
  }

  const auto data = parse_ini_file(tmp);

  REQUIRE(data.contains("global"));
  CHECK(data.at("global").at("ai_enabled") == "false");

  REQUIRE(data.contains("gtopt"));
  CHECK(data.at("gtopt").at("solver") == "highs");
  CHECK(data.at("gtopt").at("threads") == "4");
  CHECK(data.at("gtopt").at("algorithm") == "barrier");
  CHECK(data.at("gtopt").at("output-format") == "parquet");

  std::filesystem::remove(tmp);
}

TEST_CASE("parse_ini_file - Comments and blank lines")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp =
      std::filesystem::temp_directory_path() / "test_ini_comments.ini";
  {
    std::ofstream f(tmp);
    f << "# This is a comment\n"
      << "; This is also a comment\n"
      << "\n"
      << "[section]\n"
      << "key1 = value1\n"
      << "# another comment\n"
      << "key2 = value2\n";
  }

  const auto data = parse_ini_file(tmp);

  REQUIRE(data.contains("section"));
  CHECK(data.at("section").at("key1") == "value1");
  CHECK(data.at("section").at("key2") == "value2");
  CHECK(data.at("section").size() == 2);

  std::filesystem::remove(tmp);
}

TEST_CASE("parse_ini_file - Whitespace trimming")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp = std::filesystem::temp_directory_path() / "test_ini_trim.ini";
  {
    std::ofstream f(tmp);
    f << "[  section  ]\n"
      << "  key1  =  value with spaces  \n"
      << "key2=no_spaces\n";
  }

  const auto data = parse_ini_file(tmp);

  REQUIRE(data.contains("section"));
  CHECK(data.at("section").at("key1") == "value with spaces");
  CHECK(data.at("section").at("key2") == "no_spaces");

  std::filesystem::remove(tmp);
}

TEST_CASE("parse_ini_file - Nonexistent file returns empty")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto data = parse_ini_file("/tmp/nonexistent_gtopt_test_file.ini");
  CHECK(data.empty());
}

TEST_CASE("parse_ini_file - Empty value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp =
      std::filesystem::temp_directory_path() / "test_ini_empty.ini";
  {
    std::ofstream f(tmp);
    f << "[gtopt]\n"
      << "key1 = \n"
      << "key2 = value\n";
  }

  const auto data = parse_ini_file(tmp);

  REQUIRE(data.contains("gtopt"));
  CHECK(data.at("gtopt").at("key1").empty());
  CHECK(data.at("gtopt").at("key2") == "value");

  std::filesystem::remove(tmp);
}

TEST_CASE("load_gtopt_config - Reads [gtopt] section via GTOPT_CONFIG")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp =
      std::filesystem::temp_directory_path() / "test_gtopt_config.conf";
  {
    std::ofstream f(tmp);
    f << "[gtopt]\n"
      << "solver = highs\n"
      << "threads = 8\n"
      << "algorithm = dual\n"
      << "output-format = csv\n"
      << "output-compression = gzip\n"
      << "sddp-max-iterations = 500\n"
      << "sddp-convergence-tol = 1e-6\n"
      << "use-single-bus = true\n"
      << "lp-debug = false\n";
  }

  // Point GTOPT_CONFIG to our test file
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  setenv("GTOPT_CONFIG", tmp.c_str(), 1);

  const auto opts = load_gtopt_config();

  REQUIRE(opts.solver.has_value());
  CHECK(*opts.solver == "highs");
  REQUIRE(opts.threads.has_value());
  CHECK(*opts.threads == 8);
  REQUIRE(opts.algorithm.has_value());
  CHECK(*opts.algorithm == static_cast<int>(LPAlgo::dual));
  REQUIRE(opts.output_format.has_value());
  CHECK(*opts.output_format == "csv");
  REQUIRE(opts.output_compression.has_value());
  CHECK(*opts.output_compression == "gzip");
  REQUIRE(opts.sddp_max_iterations.has_value());
  CHECK(*opts.sddp_max_iterations == 500);
  REQUIRE(opts.sddp_convergence_tol.has_value());
  CHECK(*opts.sddp_convergence_tol == doctest::Approx(1e-6));
  REQUIRE(opts.use_single_bus.has_value());
  CHECK(*opts.use_single_bus == true);
  REQUIRE(opts.lp_debug.has_value());
  CHECK(*opts.lp_debug == false);

  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  unsetenv("GTOPT_CONFIG");
  std::filesystem::remove(tmp);
}

TEST_CASE("merge_config_defaults - CLI overrides config")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  MainOptions cli_opts;
  cli_opts.solver = "clp";
  cli_opts.threads = 2;

  MainOptions config_opts;
  config_opts.solver = "highs";
  config_opts.threads = 8;
  config_opts.output_format = "csv";
  config_opts.sddp_max_iterations = 200;

  merge_config_defaults(cli_opts, config_opts);

  // CLI values preserved
  CHECK(*cli_opts.solver == "clp");
  CHECK(*cli_opts.threads == 2);

  // Config values fill in missing CLI fields
  REQUIRE(cli_opts.output_format.has_value());
  CHECK(*cli_opts.output_format == "csv");
  REQUIRE(cli_opts.sddp_max_iterations.has_value());
  CHECK(*cli_opts.sddp_max_iterations == 200);

  // Fields absent from both remain nullopt
  CHECK_FALSE(cli_opts.input_directory.has_value());
}

TEST_CASE("merge_config_defaults - Empty config changes nothing")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  MainOptions cli_opts;
  cli_opts.solver = "clp";

  const MainOptions empty_config;
  merge_config_defaults(cli_opts, empty_config);

  CHECK(*cli_opts.solver == "clp");
  CHECK_FALSE(cli_opts.threads.has_value());
}
