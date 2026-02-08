#include <array>
#include <iostream>
#include "gtopt/linear_parser.hpp"

namespace gtopt
{

void LinearParser::printResult(const ParseResult& result)
{
  std::cout << "Coefficients: ";
  for (const auto& [var, coeff] : result.coefficients) {
    std::cout << var << ":" << coeff << " ";
  }

  if (result.constraint_type == ConstraintType::RANGE) {
    std::cout << "\n";
    std::cout << "Lower bound: " << result.lower_bound.value_or(0) << "\n";
    std::cout << "Upper bound: " << result.upper_bound.value_or(0) << "\n";
    std::cout << "Constraint: RANGE";
  } else {
    std::cout << "\n";
    std::cout << "RHS: " << result.rhs << "\n";
    std::cout << "Constraint: ";
    switch (result.constraint_type) {
      case ConstraintType::LESS_EQUAL:
        std::cout << "<=";
        break;
      case ConstraintType::EQUAL:
        std::cout << "=";
        break;
      case ConstraintType::GREATER_EQUAL:
        std::cout << ">=";
        break;
      case ConstraintType::RANGE:
        break;  // Already handled above
    }
  }
  std::cout << "\n\n";
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
    std::cout << "Expression: " << expr << "\n";
    try {
      auto result = LinearParser::parse(expr);
      LinearParser::printResult(result);

      // Example: get coefficient vector for specific variable order
      auto vars = result.getVariableNames();
      if (!vars.empty()) {
        auto coeff_vector = result.getCoefficientsVector(vars);
        std::cout << "Coefficient vector [";
        for (std::size_t i = 0; i < coeff_vector.size(); ++i) {
          std::cout << coeff_vector[i];
          if (i < (coeff_vector.size() - 1)) {
            std::cout << ", ";
          }
        }
        std::cout << "]\n\n";
      }
    } catch (const std::exception& e) {
      std::cout << "Error: " << e.what() << "\n\n";
    }
  }

  return 0;
}

}  // namespace gtopt
