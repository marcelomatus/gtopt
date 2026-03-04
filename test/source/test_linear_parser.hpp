/**
 * @file      test_linear_parser.cpp
 * @brief     Unit tests for the LinearParser class
 * @date      Wed Feb 19 01:30:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for parsing linear constraint expressions
 */

#include <doctest/doctest.h>
#include <gtopt/linear_parser.hpp>

using namespace gtopt;

TEST_SUITE("LinearParser")
{
  TEST_CASE("Parse simple less-equal constraint")
  {
    auto result = LinearParser::parse("3*x - 2*y <= 20");

    CHECK(result.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(result.coefficients.at("x") == doctest::Approx(3.0));
    CHECK(result.coefficients.at("y") == doctest::Approx(-2.0));
    CHECK(result.rhs == doctest::Approx(20.0));
    CHECK_FALSE(result.lower_bound.has_value());
    CHECK_FALSE(result.upper_bound.has_value());
  }

  TEST_CASE("Parse simple equality constraint")
  {
    auto result = LinearParser::parse("x + y = 10");

    CHECK(result.constraint_type == ConstraintType::EQUAL);
    CHECK(result.coefficients.at("x") == doctest::Approx(1.0));
    CHECK(result.coefficients.at("y") == doctest::Approx(1.0));
    CHECK(result.rhs == doctest::Approx(10.0));
  }

  TEST_CASE("Parse simple greater-equal constraint")
  {
    auto result = LinearParser::parse("5*a - 3b >= 2c + 1");

    CHECK(result.constraint_type == ConstraintType::GREATER_EQUAL);
    CHECK(result.coefficients.at("a") == doctest::Approx(5.0));
    CHECK(result.coefficients.at("b") == doctest::Approx(-3.0));
    CHECK(result.coefficients.at("c") == doctest::Approx(-2.0));
    CHECK(result.rhs == doctest::Approx(1.0));
  }

  TEST_CASE("Parse range constraint with <=")
  {
    auto result = LinearParser::parse("20 <= 3*x + 2*y <= 30");

    CHECK(result.constraint_type == ConstraintType::RANGE);
    CHECK(result.coefficients.at("x") == doctest::Approx(3.0));
    CHECK(result.coefficients.at("y") == doctest::Approx(2.0));
    CHECK(result.lower_bound.has_value());
    CHECK(result.upper_bound.has_value());
    CHECK(result.lower_bound.value_or(0.0) == doctest::Approx(20.0));
    CHECK(result.upper_bound.value_or(0.0) == doctest::Approx(30.0));
  }

  TEST_CASE("Parse range constraint with >=")
  {
    auto result = LinearParser::parse("5 >= 2*a - b >= -10");

    CHECK(result.constraint_type == ConstraintType::RANGE);
    CHECK(result.coefficients.at("a") == doctest::Approx(2.0));
    CHECK(result.coefficients.at("b") == doctest::Approx(-1.0));
    CHECK(result.lower_bound.has_value());
    CHECK(result.upper_bound.has_value());
    CHECK(result.lower_bound.value_or(0.0) == doctest::Approx(-10.0));
    CHECK(result.upper_bound.value_or(0.0) == doctest::Approx(5.0));
  }

  TEST_CASE("Parse constraint with multiple terms")
  {
    auto result = LinearParser::parse("2*x1 - 3*x2 + x3 <= 15");

    CHECK(result.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(result.coefficients.size() == 3);
    CHECK(result.coefficients.at("x1") == doctest::Approx(2.0));
    CHECK(result.coefficients.at("x2") == doctest::Approx(-3.0));
    CHECK(result.coefficients.at("x3") == doctest::Approx(1.0));
    CHECK(result.rhs == doctest::Approx(15.0));
  }

  TEST_CASE("Parse constraint with negative variable")
  {
    auto result = LinearParser::parse("-x + 2*y >= -5");

    CHECK(result.constraint_type == ConstraintType::GREATER_EQUAL);
    CHECK(result.coefficients.at("x") == doctest::Approx(-1.0));
    CHECK(result.coefficients.at("y") == doctest::Approx(2.0));
    CHECK(result.rhs == doctest::Approx(-5.0));
  }

  TEST_CASE("Parse constraint with constant on left side")
  {
    auto result = LinearParser::parse("10 <= 3*x + y");

    CHECK(result.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(result.coefficients.at("x") == doctest::Approx(-3.0));
    CHECK(result.coefficients.at("y") == doctest::Approx(-1.0));
    CHECK(result.rhs == doctest::Approx(-10.0));
  }

  TEST_CASE("getVariableNames returns sorted names")
  {
    auto result = LinearParser::parse("3*z + 2*a - x <= 10");

    auto names = result.getVariableNames();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "a");
    CHECK(names[1] == "x");
    CHECK(names[2] == "z");
  }

  TEST_CASE("getCoefficientsVector returns correct order")
  {
    auto result = LinearParser::parse("3*x - 2*y + z <= 10");

    const std::vector<std::string> vars = {
        "x",
        "y",
        "z",
        "w",
    };
    auto coeffs = result.getCoefficientsVector(vars);

    REQUIRE(coeffs.size() == 4);
    CHECK(coeffs[0] == doctest::Approx(3.0));
    CHECK(coeffs[1] == doctest::Approx(-2.0));
    CHECK(coeffs[2] == doctest::Approx(1.0));
    CHECK(coeffs[3] == doctest::Approx(0.0));  // missing variable
  }

  TEST_CASE("Error: empty expression")
  {
    CHECK_THROWS_AS(static_cast<void>(LinearParser::parse("")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: no constraint operator")
  {
    CHECK_THROWS_AS(static_cast<void>(LinearParser::parse("3*x + 2*y")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: empty side of constraint")
  {
    CHECK_THROWS_AS(static_cast<void>(LinearParser::parse("<= 10")),
                    std::invalid_argument);
  }

  TEST_CASE("ConstraintType enum values")
  {
    CHECK(ConstraintType::LESS_EQUAL != ConstraintType::EQUAL);
    CHECK(ConstraintType::EQUAL != ConstraintType::GREATER_EQUAL);
    CHECK(ConstraintType::GREATER_EQUAL != ConstraintType::RANGE);
  }
}
