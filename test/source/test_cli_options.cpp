/**
 * @file      test_cli_options.cpp
 * @brief     Unit tests for the modern C++ command-line parser (cli_options.hpp)
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests cover: variables_map, options_description, positional arguments,
 * command_line_parser, typed values, error handling, and edge cases.
 */

#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/cli_options.hpp>

using namespace gtopt::cli;

// ---- variables_map tests ----

TEST_CASE("cli variables_map - empty map contains nothing")
{
  variables_map vm;
  CHECK_FALSE(vm.contains("any-key"));
}

TEST_CASE("cli variables_map - stores and retrieves values")
{
  variables_map vm;
  vm["name"].set(std::any(std::string("hello")));

  CHECK(vm.contains("name"));
  CHECK(vm["name"].as<std::string>() == "hello");
}

TEST_CASE("cli variables_map - contains returns false for set-but-empty")
{
  variables_map vm;
  // Accessing a key creates a default (empty) option_value
  auto& ov = vm["missing"];
  CHECK(ov.empty());
  CHECK_FALSE(vm.contains("missing"));
}

// ---- option_value tests ----

TEST_CASE("cli option_value - empty by default")
{
  option_value ov;
  CHECK(ov.empty());
}

TEST_CASE("cli option_value - stores int")
{
  option_value ov;
  ov.set(std::any(42));
  CHECK_FALSE(ov.empty());
  CHECK(ov.as<int>() == 42);
}

TEST_CASE("cli option_value - stores double")
{
  option_value ov;
  ov.set(std::any(3.14));
  CHECK(ov.as<double>() == doctest::Approx(3.14));
}

TEST_CASE("cli option_value - stores bool")
{
  option_value ov;
  ov.set(std::any(true));
  CHECK(ov.as<bool>() == true);
}

TEST_CASE("cli option_value - bad cast throws")
{
  option_value ov;
  ov.set(std::any(42));
  CHECK_THROWS_AS(
      [[maybe_unused]] auto v = ov.as<std::string>(), std::bad_any_cast);
}

// ---- options_description tests ----

TEST_CASE("cli options_description - add flag option")
{
  options_description desc("Test");
  desc.add_options()("help,h", "show help");

  auto* opt = desc.find_long("help");
  REQUIRE(opt != nullptr);
  CHECK(opt->long_name == "help");
  CHECK(opt->short_name == 'h');
  CHECK(opt->description == "show help");
  CHECK_FALSE(opt->takes_value);
}

TEST_CASE("cli options_description - add valued option")
{
  options_description desc;
  desc.add_options()("count,c", value<int>(), "a count");

  auto* opt = desc.find_long("count");
  REQUIRE(opt != nullptr);
  CHECK(opt->takes_value);
  CHECK_FALSE(opt->has_implicit);
  CHECK_FALSE(opt->multi_value);
}

TEST_CASE("cli options_description - add option with implicit value")
{
  options_description desc;
  desc.add_options()(
      "verbose,v", value<bool>().implicit_value(true), "be verbose");

  auto* opt = desc.find_long("verbose");
  REQUIRE(opt != nullptr);
  CHECK(opt->takes_value);
  CHECK(opt->has_implicit);
  CHECK(std::any_cast<bool>(opt->implicit_value) == true);
}

TEST_CASE("cli options_description - find by short name")
{
  options_description desc;
  desc.add_options()("output,o", value<std::string>(), "output file");

  auto* opt = desc.find_short('o');
  REQUIRE(opt != nullptr);
  CHECK(opt->long_name == "output");
}

TEST_CASE("cli options_description - find returns nullptr for unknown")
{
  options_description desc;
  desc.add_options()("known", "a known option");

  CHECK(desc.find_long("unknown") == nullptr);
  CHECK(desc.find_short('x') == nullptr);
}

TEST_CASE("cli options_description - option without short name")
{
  options_description desc;
  desc.add_options()("long-only", "no shortcut");

  auto* opt = desc.find_long("long-only");
  REQUIRE(opt != nullptr);
  CHECK(opt->short_name == '\0');
}

TEST_CASE("cli options_description - multi-value vector option")
{
  options_description desc;
  desc.add_options()(
      "files,f", value<std::vector<std::string>>(), "input files");

  auto* opt = desc.find_long("files");
  REQUIRE(opt != nullptr);
  CHECK(opt->multi_value);
}

TEST_CASE("cli options_description - streaming output")
{
  options_description desc("My Options");
  desc.add_options()("help,h", "show help")(
      "count,c", value<int>(), "a count");

  std::ostringstream oss;
  oss << desc;
  auto output = oss.str();
  CHECK(output.find("My Options") != std::string::npos);
  CHECK(output.find("--help") != std::string::npos);
  CHECK(output.find("-h") != std::string::npos);
  CHECK(output.find("--count") != std::string::npos);
  CHECK(output.find("show help") != std::string::npos);
}

// ---- command_line_parser tests ----

TEST_CASE("cli parser - parse long flag option")
{
  options_description desc;
  desc.add_options()("help,h", "help");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--help"}).options(desc);
  store(parser, vm);

  CHECK(vm.contains("help"));
}

TEST_CASE("cli parser - parse short flag option")
{
  options_description desc;
  desc.add_options()("help,h", "help");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"-h"}).options(desc);
  store(parser, vm);

  CHECK(vm.contains("help"));
}

TEST_CASE("cli parser - parse long option with value")
{
  options_description desc;
  desc.add_options()("name,n", value<std::string>(), "name");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--name", "alice"})
            .options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("name"));
  CHECK(vm["name"].as<std::string>() == "alice");
}

TEST_CASE("cli parser - parse long option with equals syntax")
{
  options_description desc;
  desc.add_options()("name,n", value<std::string>(), "name");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--name=bob"})
            .options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("name"));
  CHECK(vm["name"].as<std::string>() == "bob");
}

TEST_CASE("cli parser - parse short option with value")
{
  options_description desc;
  desc.add_options()("count,c", value<int>(), "count");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"-c", "5"}).options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("count"));
  CHECK(vm["count"].as<int>() == 5);
}

TEST_CASE("cli parser - parse double value")
{
  options_description desc;
  desc.add_options()("eps,e", value<double>(), "epsilon");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--eps", "0.001"})
            .options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("eps"));
  CHECK(vm["eps"].as<double>() == doctest::Approx(0.001));
}

TEST_CASE("cli parser - implicit bool value without argument")
{
  options_description desc;
  desc.add_options()(
      "verbose,v", value<bool>().implicit_value(true), "verbose");

  variables_map vm;
  auto parser = command_line_parser(std::vector<std::string> {"--verbose"})
                    .options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("verbose"));
  CHECK(vm["verbose"].as<bool>() == true);
}

TEST_CASE("cli parser - implicit int value without argument")
{
  options_description desc;
  desc.add_options()("level,l", value<int>().implicit_value(1), "level");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--level"}).options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("level"));
  CHECK(vm["level"].as<int>() == 1);
}

TEST_CASE("cli parser - implicit value overridden by explicit argument")
{
  options_description desc;
  desc.add_options()("level,l", value<int>().implicit_value(1), "level");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--level", "3"})
            .options(desc);
  store(parser, vm);

  REQUIRE(vm.contains("level"));
  CHECK(vm["level"].as<int>() == 3);
}

TEST_CASE("cli parser - positional arguments")
{
  options_description desc;
  desc.add_options()("files,f", value<std::vector<std::string>>(), "files");

  positional_options_description pos;
  pos.add("files", -1);

  variables_map vm;
  auto parser
      = command_line_parser(
            std::vector<std::string> {"file1.txt", "file2.txt"})
            .options(desc)
            .positional(pos);
  store(parser, vm);

  REQUIRE(vm.contains("files"));
  auto files = vm["files"].as<std::vector<std::string>>();
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "file1.txt");
  CHECK(files[1] == "file2.txt");
}

TEST_CASE("cli parser - mixed positional and named arguments")
{
  options_description desc;
  desc.add_options()("files,f", value<std::vector<std::string>>(), "files")(
      "output,o", value<std::string>(), "output");

  positional_options_description pos;
  pos.add("files", -1);

  variables_map vm;
  auto parser = command_line_parser(std::vector<std::string> {
                                        "input.txt", "--output", "/tmp/out"})
                    .options(desc)
                    .positional(pos);
  store(parser, vm);

  REQUIRE(vm.contains("files"));
  auto files = vm["files"].as<std::vector<std::string>>();
  REQUIRE(files.size() == 1);
  CHECK(files[0] == "input.txt");

  REQUIRE(vm.contains("output"));
  CHECK(vm["output"].as<std::string>() == "/tmp/out");
}

TEST_CASE("cli parser - allow_unregistered skips unknown options")
{
  options_description desc;
  desc.add_options()("known", "known option");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--known", "--unknown"})
            .options(desc)
            .allow_unregistered();
  CHECK_NOTHROW(store(parser, vm));
  CHECK(vm.contains("known"));
}

TEST_CASE("cli parser - unknown option throws without allow_unregistered")
{
  options_description desc;
  desc.add_options()("known", "known option");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--unknown"})
            .options(desc);
  CHECK_THROWS_AS(store(parser, vm), parse_error);
}

TEST_CASE("cli parser - missing required value throws")
{
  options_description desc;
  desc.add_options()("name,n", value<std::string>(), "name");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--name"}).options(desc);
  CHECK_THROWS_AS(store(parser, vm), parse_error);
}

TEST_CASE("cli parser - bool value parsing true variants")
{
  options_description desc;
  desc.add_options()("flag,f", value<bool>(), "flag");

  for (const auto& tv : {"true", "1", "yes"}) {
    variables_map vm;
    auto parser
        = command_line_parser(std::vector<std::string> {"--flag", tv})
              .options(desc);
    store(parser, vm);
    REQUIRE(vm.contains("flag"));
    CHECK(vm["flag"].as<bool>() == true);
  }
}

TEST_CASE("cli parser - bool value parsing false variants")
{
  options_description desc;
  desc.add_options()("flag,f", value<bool>(), "flag");

  for (const auto& fv : {"false", "0", "no"}) {
    variables_map vm;
    auto parser
        = command_line_parser(std::vector<std::string> {"--flag", fv})
              .options(desc);
    store(parser, vm);
    REQUIRE(vm.contains("flag"));
    CHECK(vm["flag"].as<bool>() == false);
  }
}

TEST_CASE("cli parser - invalid bool value throws")
{
  options_description desc;
  desc.add_options()("flag,f", value<bool>(), "flag");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {"--flag", "maybe"})
            .options(desc);
  CHECK_THROWS_AS(store(parser, vm), parse_error);
}

TEST_CASE("cli parser - empty args produces empty map")
{
  options_description desc;
  desc.add_options()("help,h", "help");

  variables_map vm;
  auto parser
      = command_line_parser(std::vector<std::string> {}).options(desc);
  store(parser, vm);

  CHECK_FALSE(vm.contains("help"));
}

TEST_CASE("cli parser - multiple flags at once")
{
  options_description desc;
  desc.add_options()("alpha,a", "a")("beta,b", "b")("gamma,g", "g");

  variables_map vm;
  auto parser = command_line_parser(
                    std::vector<std::string> {"--alpha", "-b", "--gamma"})
                    .options(desc);
  store(parser, vm);

  CHECK(vm.contains("alpha"));
  CHECK(vm.contains("beta"));
  CHECK(vm.contains("gamma"));
}

TEST_CASE("cli parser - argc/argv constructor")
{
  options_description desc;
  desc.add_options()("verbose,v", "verbose");

  // Simulate argc/argv (argv[0] is program name, should be skipped)
  const char* argv[] = {"program", "--verbose"};
  int argc = 2;

  variables_map vm;
  auto parser
      = command_line_parser(argc, const_cast<char**>(argv)).options(desc);
  store(parser, vm);

  CHECK(vm.contains("verbose"));
}

// ---- notify is a no-op but should not throw ----

TEST_CASE("cli notify - does not throw")
{
  variables_map vm;
  CHECK_NOTHROW(notify(vm));
}

// ---- typed_value tests ----

TEST_CASE("cli typed_value - string is not multi-value")
{
  auto tv = value<std::string>();
  CHECK_FALSE(tv.multi);
}

TEST_CASE("cli typed_value - vector<string> is multi-value")
{
  auto tv = value<std::vector<std::string>>();
  CHECK(tv.multi);
}

TEST_CASE("cli typed_value - implicit_value sets flag and value")
{
  auto tv = value<int>().implicit_value(42);
  CHECK(tv.has_implicit);
  CHECK(tv.implicit_val == 42);
}
