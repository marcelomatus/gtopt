/**
 * @file      test_cli_options.cpp
 * @brief     Unit tests for cli_options.hpp (boost::program_options replacement)
 * @date      Sun Feb  9 07:14:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for the CLI option parser that replaced boost::program_options
 */

#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

// cli_options.hpp is in the standalone directory, but we test it independently
// by including it directly with a relative path from the test source
#include "../../standalone/source/cli_options.hpp"

TEST_CASE("cli::variables_map - Basic operations")
{
  cli::variables_map vm;

  SUBCASE("Empty map contains nothing")
  {
    CHECK_FALSE(vm.contains("anything"));
  }

  SUBCASE("Set and retrieve string value")
  {
    vm.set("name", std::string("value"));
    CHECK(vm.contains("name"));
    CHECK(vm["name"].as<std::string>() == "value");
  }

  SUBCASE("Set and retrieve int value")
  {
    vm.set("count", 42);
    CHECK(vm.contains("count"));
    CHECK(vm["count"].as<int>() == 42);
  }

  SUBCASE("Set and retrieve double value")
  {
    vm.set("threshold", 3.14);
    CHECK(vm.contains("threshold"));
    CHECK(vm["threshold"].as<double>() == doctest::Approx(3.14));
  }

  SUBCASE("Set and retrieve bool value")
  {
    vm.set("flag", true);
    CHECK(vm.contains("flag"));
    CHECK(vm["flag"].as<bool>() == true);
  }

  SUBCASE("Overwrite existing value")
  {
    vm.set("key", std::string("first"));
    vm.set("key", std::string("second"));
    CHECK(vm["key"].as<std::string>() == "second");
  }

  SUBCASE("Access non-existent key throws")
  {
    CHECK_THROWS_AS(vm["nonexistent"], std::out_of_range);
  }
}

TEST_CASE("cli::options_description - Option definition")
{
  cli::options_description desc("Test Options");

  SUBCASE("Flag option")
  {
    desc.add_options()("help,h", "show help");

    const auto& opts = desc.options();
    CHECK(opts.size() == 1);
    CHECK(opts[0].long_name == "help");
    CHECK(opts[0].short_name == 'h');
    CHECK(opts[0].value_type == 0);  // flag
  }

  SUBCASE("String option")
  {
    desc.add_options()(
        "output,o",
        cli::options_description::value<std::string>(),
        "output file");

    const auto& opts = desc.options();
    CHECK(opts.size() == 1);
    CHECK(opts[0].long_name == "output");
    CHECK(opts[0].short_name == 'o');
    CHECK(opts[0].value_type == 1);  // string
  }

  SUBCASE("Int option with implicit value")
  {
    desc.add_options()(
        "level,l",
        cli::options_description::value<int>().implicit_value(1),
        "verbosity level");

    const auto& opts = desc.options();
    CHECK(opts.size() == 1);
    CHECK(opts[0].long_name == "level");
    CHECK(opts[0].has_implicit);
    CHECK(std::any_cast<int>(opts[0].implicit_value) == 1);
  }

  SUBCASE("Bool option with implicit value")
  {
    desc.add_options()(
        "verbose,v",
        cli::options_description::value<bool>().implicit_value(true),
        "enable verbose mode");

    const auto& opts = desc.options();
    CHECK(opts.size() == 1);
    CHECK(opts[0].long_name == "verbose");
    CHECK(opts[0].has_implicit);
    CHECK(std::any_cast<bool>(opts[0].implicit_value) == true);
  }

  SUBCASE("Multiple options in chain")
  {
    desc.add_options()
        ("help,h", "show help")
        ("version,V", "show version")
        ("output,o",
         cli::options_description::value<std::string>(),
         "output file");

    const auto& opts = desc.options();
    CHECK(opts.size() == 3);
    CHECK(opts[0].long_name == "help");
    CHECK(opts[1].long_name == "version");
    CHECK(opts[2].long_name == "output");
  }

  SUBCASE("Option without short name")
  {
    desc.add_options()("long-only", "a long-only option");

    const auto& opts = desc.options();
    CHECK(opts.size() == 1);
    CHECK(opts[0].long_name == "long-only");
    CHECK(opts[0].short_name == '\0');
  }

  SUBCASE("Help text output")
  {
    desc.add_options()
        ("help,h", "show help")
        ("output,o",
         cli::options_description::value<std::string>(),
         "output file");

    std::ostringstream oss;
    oss << desc;
    std::string help = oss.str();

    CHECK(help.find("--help") != std::string::npos);
    CHECK(help.find("-h") != std::string::npos);
    CHECK(help.find("--output") != std::string::npos);
    CHECK(help.find("<string>") != std::string::npos);
  }
}

TEST_CASE("cli::parse_args - Flag parsing")
{
  cli::options_description desc("Test");
  desc.add_options()
      ("help,h", "show help")
      ("verbose,v", "verbose mode");

  SUBCASE("Long flag")
  {
    const char* argv[] = {"prog", "--help"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm.contains("help"));
    CHECK_FALSE(vm.contains("verbose"));
  }

  SUBCASE("Short flag")
  {
    const char* argv[] = {"prog", "-h"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm.contains("help"));
  }

  SUBCASE("Multiple flags")
  {
    const char* argv[] = {"prog", "--help", "-v"};
    cli::variables_map vm;
    cli::parse_args(3, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm.contains("help"));
    CHECK(vm.contains("verbose"));
  }
}

TEST_CASE("cli::parse_args - Value parsing")
{
  cli::options_description desc("Test");
  desc.add_options()
      ("output,o",
       cli::options_description::value<std::string>(),
       "output file")
      ("count,c",
       cli::options_description::value<int>(),
       "count")
      ("threshold,t",
       cli::options_description::value<double>(),
       "threshold");

  SUBCASE("Long option with space-separated value")
  {
    const char* argv[] = {"prog", "--output", "file.txt"};
    cli::variables_map vm;
    cli::parse_args(3, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm["output"].as<std::string>() == "file.txt");
  }

  SUBCASE("Long option with = value")
  {
    const char* argv[] = {"prog", "--output=file.txt"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm["output"].as<std::string>() == "file.txt");
  }

  SUBCASE("Short option with value")
  {
    const char* argv[] = {"prog", "-o", "file.txt"};
    cli::variables_map vm;
    cli::parse_args(3, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm["output"].as<std::string>() == "file.txt");
  }

  SUBCASE("Int value")
  {
    const char* argv[] = {"prog", "--count", "42"};
    cli::variables_map vm;
    cli::parse_args(3, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm["count"].as<int>() == 42);
  }

  SUBCASE("Double value")
  {
    const char* argv[] = {"prog", "--threshold", "3.14"};
    cli::variables_map vm;
    cli::parse_args(3, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm["threshold"].as<double>() == doctest::Approx(3.14));
  }
}

TEST_CASE("cli::parse_args - Implicit values")
{
  cli::options_description desc("Test");
  desc.add_options()
      ("quiet,q",
       cli::options_description::value<bool>().implicit_value(true),
       "quiet mode")
      ("level,l",
       cli::options_description::value<int>().implicit_value(1),
       "level");

  SUBCASE("Implicit bool")
  {
    const char* argv[] = {"prog", "--quiet"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm.contains("quiet"));
    CHECK(vm["quiet"].as<bool>() == true);
  }

  SUBCASE("Implicit int")
  {
    const char* argv[] = {"prog", "--level"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm.contains("level"));
    CHECK(vm["level"].as<int>() == 1);
  }

  SUBCASE("Short option with implicit value")
  {
    const char* argv[] = {"prog", "-q"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "", vm);

    CHECK(vm["quiet"].as<bool>() == true);
  }
}

TEST_CASE("cli::parse_args - Positional arguments")
{
  cli::options_description desc("Test");
  desc.add_options()
      ("help,h", "show help")
      ("files,f",
       cli::options_description::value<std::vector<std::string>>(),
       "input files");

  SUBCASE("Single positional")
  {
    const char* argv[] = {"prog", "input.txt"};
    cli::variables_map vm;
    cli::parse_args(2, const_cast<char**>(argv), desc, "files", vm);

    auto files = vm["files"].as<std::vector<std::string>>();
    CHECK(files.size() == 1);
    CHECK(files[0] == "input.txt");
  }

  SUBCASE("Multiple positional")
  {
    const char* argv[] = {"prog", "a.txt", "b.txt", "c.txt"};
    cli::variables_map vm;
    cli::parse_args(4, const_cast<char**>(argv), desc, "files", vm);

    auto files = vm["files"].as<std::vector<std::string>>();
    CHECK(files.size() == 3);
    CHECK(files[0] == "a.txt");
    CHECK(files[1] == "b.txt");
    CHECK(files[2] == "c.txt");
  }

  SUBCASE("Mixed flags and positional")
  {
    const char* argv[] = {"prog", "--help", "a.txt", "b.txt"};
    cli::variables_map vm;
    cli::parse_args(4, const_cast<char**>(argv), desc, "files", vm);

    CHECK(vm.contains("help"));
    auto files = vm["files"].as<std::vector<std::string>>();
    CHECK(files.size() == 2);
  }
}

TEST_CASE("cli::parse_args - Error handling")
{
  cli::options_description desc("Test");
  desc.add_options()("help,h", "show help");

  SUBCASE("Unknown option throws")
  {
    const char* argv[] = {"prog", "--unknown"};
    cli::variables_map vm;

    CHECK_THROWS_AS(
        cli::parse_args(2, const_cast<char**>(argv), desc, "", vm),
        cli::parse_error);
  }

  SUBCASE("Allow unregistered")
  {
    const char* argv[] = {"prog", "--unknown", "--help"};
    cli::variables_map vm;

    CHECK_NOTHROW(
        cli::parse_args(3, const_cast<char**>(argv), desc, "", vm, true));
    CHECK(vm.contains("help"));
  }

  SUBCASE("Missing required value throws")
  {
    cli::options_description desc2("Test");
    desc2.add_options()(
        "output,o",
        cli::options_description::value<std::string>(),
        "output file");

    const char* argv[] = {"prog", "--output"};
    cli::variables_map vm;

    CHECK_THROWS_AS(
        cli::parse_args(2, const_cast<char**>(argv), desc2, "", vm),
        cli::parse_error);
  }
}

TEST_CASE("cli::parse_args - Complex scenario matching main.cpp usage")
{
  // Mimics the actual gtopt standalone usage pattern
  cli::options_description desc("Gtoptp options");
  desc.add_options()
      ("help,h", "describes arguments")
      ("verbose,v", "activates maximum verbosity")
      ("quiet,q",
       cli::options_description::value<bool>().implicit_value(true),
       "quiet mode")
      ("version,V", "shows version")
      ("system-file,s",
       cli::options_description::value<std::vector<std::string>>(),
       "system file")
      ("output-directory,d",
       cli::options_description::value<std::string>(),
       "output directory")
      ("use-single-bus,b",
       cli::options_description::value<bool>().implicit_value(true),
       "single bus mode")
      ("use-lp-names,n",
       cli::options_description::value<int>().implicit_value(1),
       "use lp names")
      ("matrix-eps,e",
       cli::options_description::value<double>(),
       "matrix epsilon");

  const char* argv[] = {
      "gtopt",
      "--help",
      "-v",
      "--output-directory", "/tmp/output",
      "--use-single-bus",
      "-n",
      "--matrix-eps", "0.001",
      "system1.json",
      "system2.json"
  };
  int argc = 11;

  cli::variables_map vm;
  cli::parse_args(argc, const_cast<char**>(argv), desc, "system-file", vm, true);

  CHECK(vm.contains("help"));
  CHECK(vm.contains("verbose"));
  CHECK(vm["output-directory"].as<std::string>() == "/tmp/output");
  CHECK(vm["use-single-bus"].as<bool>() == true);
  CHECK(vm["use-lp-names"].as<int>() == 1);
  CHECK(vm["matrix-eps"].as<double>() == doctest::Approx(0.001));

  auto files = vm["system-file"].as<std::vector<std::string>>();
  CHECK(files.size() == 2);
  CHECK(files[0] == "system1.json");
  CHECK(files[1] == "system2.json");
}

TEST_CASE("cli::parse_error - Exception hierarchy")
{
  cli::parse_error err("test error");

  CHECK(std::string(err.what()) == "test error");

  // Should be catchable as std::runtime_error
  try {
    throw cli::parse_error("some error");
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()) == "some error");
  }
}
