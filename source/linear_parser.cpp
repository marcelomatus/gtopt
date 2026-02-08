#include <array>
#include <print>
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
    std::println("");
    std::println("Lower bound: {}", result.lower_bound.value_or(0));
    std::println("Upper bound: {}", result.upper_bound.value_or(0));
    std::print("Constraint: RANGE");
  } else {
    std::println("");
    std::println("RHS: {}", result.rhs);
    std::print("Constraint: ");
    switch (result.constraint_type) {
      case ConstraintType::LESS_EQUAL:
        std::print("<=");
        break;
      case ConstraintType::EQUAL:
        std::print("=");
        break;
      case ConstraintType::GREATER_EQUAL:
        std::print(">=");
        break;
      case ConstraintType::RANGE:
        break;  // Already handled above
    }
  }
  std::println("");
  std::println("");
}

int LinearParser::do_main()
{
  constexpr std::array test_expressions = {"3*x - 2*y <= 20",
                                           "5*a - 3b >= 2c + 1",
                                           "x + y = 10",
                                           "2*x1 - 3*x2 + x3 <= 15",
                                           "-x + 2*y >= -5",
                                           "10 <= 3*x + y",
                                           "20 <= 3*x + 2*y <= 30",
                                           "5 >= 2*a - b >= -10",
                                           "-1 <= x - y <= 5"};

  for (const auto& expr : test_expressions) {
    std::println("Expression: {}", expr);
    try {
      auto result = LinearParser::parse(expr);
      printResult(result);

      // Example: get coefficient vector for specific variable order
      auto vars = result.getVariableNames();
      if (!vars.empty()) {
        auto coeff_vector = result.getCoefficientsVector(vars);
        std::print("Coefficient vector [");
        for (std::size_t i = 0; i < coeff_vector.size(); ++i) {
          std::print("{}", coeff_vector[i]);
          if (i < (coeff_vector.size() - 1)) {
            std::print(", ");
          }
        }
        std::println("]");
        std::println("");
      }
    } catch (const std::exception& e) {
      std::println("Error: {}", e.what());
      std::println("");
    }
  }

  return 0;
}

}  // namespace gtopt
