#include <array>
#include <print>
#include <ranges>

#include "gtopt/linear_parser.hpp"

namespace gtopt
{

void LinearParser::printResult(const ParseResult& result)
{
  std::print("Coefficients: ");
  for (const auto& [var, coeff] : result.coefficients) {
    std::print("{}:{} ", var, coeff);
  }

  if (result.constraint_type == ConstraintType::RANGE) {
    std::print(
        "\nLower bound: {}\nUpper bound: {}\nConstraint: RANGE",
        result.lower_bound.value_or(0),
        result.upper_bound.value_or(0));
  } else {
    constexpr auto constraint_str =
        [](ConstraintType ct) noexcept -> std::string_view
    {
      switch (ct) {
        case ConstraintType::LESS_EQUAL:
          return "<=";
        case ConstraintType::EQUAL:
          return "=";
        case ConstraintType::GREATER_EQUAL:
          return ">=";
        case ConstraintType::RANGE:
          return "";
      }
      return "";
    };
    std::print(
        "\nRHS: {}\nConstraint: {}", result.rhs, constraint_str(result.constraint_type));
  }
  std::println("\n");
}

int LinearParser::do_main()
{
  constexpr std::array test_expressions = {
      "3*x - 2*y <= 20",
      "5*a - 3b >= 2c + 1",
      "x + y = 10",
      "2*x1 - 3*x2 + x3 <= 15",
      "-x + 2*y >= -5",
      "10 <= 3*x + y",
      "20 <= 3*x + 2*y <= 30",
      "5 >= 2*a - b >= -10",
      "-1 <= x - y <= 5",
  };

  for (const auto& expr : test_expressions) {
    std::println("Expression: {}", expr);
    try {
      auto result = LinearParser::parse(expr);
      LinearParser::printResult(result);

      // Example: get coefficient vector for specific variable order
      auto vars = result.getVariableNames();
      if (!vars.empty()) {
        auto coeff_vector = result.getCoefficientsVector(vars);
        std::print("Coefficient vector [");
        for (const auto& [i, val] : std::views::enumerate(coeff_vector)) {
          if (i > 0) {
            std::print(", ");
          }
          std::print("{}", val);
        }
        std::println("]\n");
      }
    } catch (const std::exception& e) {
      std::println("Error: {}\n", e.what());
    }
  }

  return 0;
}

}  // namespace gtopt
