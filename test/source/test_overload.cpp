/**
 * @file      test_overload.cpp
 * @brief     Unit tests for Overload pattern helper
 * @date      Sat Feb  8 07:35:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for the Overload helper that combines multiple callables for std::visit
 */

#include <string>
#include <variant>

#include <doctest/doctest.h>
#include <gtopt/overload.hpp>

using namespace gtopt;

TEST_CASE("Overload - Basic functionality")
{
  std::variant<int, double, std::string> v;

  SUBCASE("Integer overload")
  {
    v = 42;
    auto result = std::visit(
        Overload {
            [](int val) { return std::format("int: {}", val); },
            [](double val) { return std::format("double: {}", val); },
            [](const std::string& val) { return std::format("string: {}", val); },
        },
        v);
    CHECK(result == "int: 42");
  }

  SUBCASE("Double overload")
  {
    v = 3.14;
    auto result = std::visit(
        Overload {
            [](int val) { return std::format("int: {}", val); },
            [](double val) { return std::format("double: {}", val); },
            [](const std::string& val) { return std::format("string: {}", val); },
        },
        v);
    CHECK(result == "double: 3.14");
  }

  SUBCASE("String overload")
  {
    v = std::string("hello");
    auto result = std::visit(
        Overload {
            [](int val) { return std::format("int: {}", val); },
            [](double val) { return std::format("double: {}", val); },
            [](const std::string& val) { return std::format("string: {}", val); },
        },
        v);
    CHECK(result == "string: hello");
  }
}

TEST_CASE("Overload - CTAD (Class Template Argument Deduction)")
{
  // Test that CTAD works correctly without explicit template arguments
  std::variant<int, std::string> v = 10;

  auto visitor = Overload {
      [](int val) { return val * 2; },
      [](const std::string&) { return 0; },
  };

  auto result = std::visit(visitor, v);
  CHECK(result == 20);
}

TEST_CASE("Overload - Multiple argument types")
{
  std::variant<int, float, double> v1 = 5;
  std::variant<int, float, double> v2 = 3.0;

  auto result = std::visit(
      Overload {
          [](int a, int b) { return a + b; },
          [](int a, double b) { return static_cast<double>(a) + b; },
          [](double a, int b) { return a + static_cast<double>(b); },
          [](auto a, auto b) { return a + b; },
      },
      v1,
      v2);

  CHECK(result == 8.0);
}

TEST_CASE("Overload - With captured variables")
{
  int multiplier = 3;
  std::variant<int, double> v = 10;

  auto result = std::visit(
      Overload {
          [&multiplier](int val) { return val * multiplier; },
          [&multiplier](double val) {
            return val * static_cast<double>(multiplier);
          },
      },
      v);

  CHECK(result == 30);
}

TEST_CASE("Overload - Generic lambda fallback")
{
  std::variant<int, double, std::string, char> v;

  SUBCASE("Handles all types with generic lambda")
  {
    v = 42;
    auto result1 = std::visit(Overload {[](auto val) { return true; }}, v);
    CHECK(result1);

    v = 3.14;
    auto result2 = std::visit(Overload {[](auto val) { return true; }}, v);
    CHECK(result2);

    v = std::string("test");
    auto result3 = std::visit(Overload {[](auto val) { return true; }}, v);
    CHECK(result3);

    v = 'c';
    auto result4 = std::visit(Overload {[](auto val) { return true; }}, v);
    CHECK(result4);
  }
}

TEST_CASE("Overload - Return type void")
{
  std::variant<int, std::string> v = 42;
  bool was_int = false;
  bool was_string = false;

  std::visit(
      Overload {
          [&was_int](int) { was_int = true; },
          [&was_string](const std::string&) { was_string = true; },
      },
      v);

  CHECK(was_int);
  CHECK_FALSE(was_string);

  v = std::string("test");
  was_int = false;
  was_string = false;

  std::visit(
      Overload {
          [&was_int](int) { was_int = true; },
          [&was_string](const std::string&) { was_string = true; },
      },
      v);

  CHECK_FALSE(was_int);
  CHECK(was_string);
}

TEST_CASE("Overload - Nested variants")
{
  using InnerVariant = std::variant<int, double>;
  using OuterVariant = std::variant<InnerVariant, std::string>;

  OuterVariant v = InnerVariant {42};

  auto result = std::visit(
      Overload {
          [](const InnerVariant& inner) {
            return std::visit(
                Overload {
                    [](int val) { return val; },
                    [](double val) { return static_cast<int>(val); },
                },
                inner);
          },
          [](const std::string&) { return 0; },
      },
      v);

  CHECK(result == 42);
}
