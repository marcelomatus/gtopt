#include <algorithm>
#include <cctype>
#include <concepts>
#include <iomanip>
#include <iostream>
#include <optional>
#include <ranges>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gtopt
{

enum class ConstraintType : std::uint8_t
{
  LESS_EQUAL,  // <=
  EQUAL,  // =
  GREATER_EQUAL,  // >=
  RANGE  // lower <= expr <= upper
};

struct ParseResult
{
  std::unordered_map<std::string, double> coefficients;
  double rhs;
  ConstraintType constraint_type;

  // For range constraints (lower <= expr <= upper)
  std::optional<double> lower_bound;
  std::optional<double> upper_bound;

  // Helper to get coefficient vector in a specific variable order
  [[nodiscard]] std::vector<double> getCoefficientsVector(
      const std::vector<std::string>& variable_order) const
  {
    std::vector<double> result;
    result.reserve(variable_order.size());

    for (const auto& var : variable_order) {
      if (auto it = coefficients.find(var); it != coefficients.end()) {
        result.push_back(it->second);
      } else {
        result.push_back(0.0);
      }
    }
    return result;
  }

  // Get all variable names in sorted order
  [[nodiscard]] std::vector<std::string> getVariableNames() const
  {
    std::vector<std::string> names;
    names.reserve(coefficients.size());

    for (const auto& [var, coeff] : coefficients) {
      names.push_back(var);
    }

    std::ranges::sort(names);
    return names;
  }
};

class LinearParser
{
private:
  // Remove all whitespace from the expression
  [[nodiscard]] static std::string removeWhitespace(std::string_view expr)
  {
    std::string result;
    result.reserve(expr.size());

    std::ranges::copy_if(
        expr,
        std::back_inserter(result),
        [](char c) noexcept
        { return !std::isspace(static_cast<unsigned char>(c)); });
    return result;
  }

  // Find and parse constraint operator(s) - handles both single and range
  // constraints
  [[nodiscard]] static std::variant<
      std::pair<ConstraintType, std::size_t>,
      std::tuple<ConstraintType, std::size_t, std::size_t>>
  findConstraintOperator(std::string_view expr)
  {
    std::vector<std::pair<std::size_t, std::string_view>> operators;
    operators.reserve(4);  // Most expressions won't have more than 2 operators

    // Find all operators with their positions
    for (std::size_t pos = 0; pos < expr.length(); ++pos) {
      if (pos + 1 < expr.length()) {
        std::string_view two_char = expr.substr(pos, 2);
        if (two_char == "<=" || two_char == ">=") {
          operators.emplace_back(pos, two_char);
          ++pos;  // Skip next character
          continue;
        }
      }
      if (expr[pos] == '=') {
        // Make sure it's not part of <= or >=
        if ((pos == 0 || expr[pos - 1] != '<')
            && (pos == 0 || expr[pos - 1] != '>'))
        {
          operators.emplace_back(pos, std::string_view {"="});
        }
      }
    }

    if (operators.empty()) {
      throw std::invalid_argument(
          "No valid constraint operator found (<=, =, >=)");
    }

    if (operators.size() == 1) {
      // Single constraint
      auto [pos, op] = operators[0];
      constexpr auto getConstraintType =
          [](std::string_view op) noexcept -> ConstraintType
      {
        if (op == "<=")
          return ConstraintType::LESS_EQUAL;
        if (op == ">=")
          return ConstraintType::GREATER_EQUAL;
        return ConstraintType::EQUAL;
      };

      return std::make_pair(getConstraintType(op), pos);
    } else if (operators.size() == 2) {
      // Range constraint: check if it's valid (both should be <= or >=)
      auto [pos1, op1] = operators[0];
      auto [pos2, op2] = operators[1];

      if ((op1 == "<=" && op2 == "<=") || (op1 == ">=" && op2 == ">=")) {
        return std::make_tuple(ConstraintType::RANGE, pos1, pos2);
      } else {
        throw std::invalid_argument(
            "Invalid range constraint: operators must be consistent");
      }
    } else {
      throw std::invalid_argument("Too many constraint operators found");
    }
  }

  // Parse a single term like "3*x", "-2*y", "5*a", "-3b", "2c"
  [[nodiscard]] static std::pair<double, std::string> parseTerm(
      std::string_view term)
  {
    if (term.empty()) {
      throw std::invalid_argument("Empty term");
    }

    std::string_view cleaned = term;
    double coefficient = 1.0;

    // Handle sign
    bool negative = false;
    if (cleaned.front() == '-') {
      negative = true;
      cleaned.remove_prefix(1);
    } else if (cleaned.front() == '+') {
      cleaned.remove_prefix(1);
    }

    // Find the variable (last alphabetic character sequence)
    std::size_t var_start = std::string_view::npos;
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
      if (std::isalpha(static_cast<unsigned char>(cleaned[i]))) {
        if (var_start == std::string_view::npos) {
          var_start = i;
        }
      } else if (var_start != std::string_view::npos) {
        // Found end of variable
        break;
      }
    }

    if (var_start == std::string_view::npos) {
      // No variable found, this is a constant term
      try {
        coefficient = std::stod(std::string {cleaned});
        return {negative ? -coefficient : coefficient, std::string {}};
      } catch (const std::exception&) {
        throw std::invalid_argument(std::string {"Invalid constant term: "}
                                    + std::string {term});
      }
    }

    // Extract variable name (assume it goes to the end)
    std::string variable {cleaned.substr(var_start)};

    // Extract coefficient part
    std::string_view coeff_part = cleaned.substr(0, var_start);

    // Remove trailing '*' if present
    if (!coeff_part.empty() && coeff_part.back() == '*') {
      coeff_part.remove_suffix(1);
    }

    if (coeff_part.empty()) {
      coefficient = 1.0;
    } else {
      try {
        coefficient = std::stod(std::string {coeff_part});
      } catch (const std::exception&) {
        throw std::invalid_argument(
            std::string {"Invalid coefficient in term: "} + std::string {term});
      }
    }

    return {negative ? -coefficient : coefficient, std::move(variable)};
  }

  // Split expression into terms, handling signs correctly
  [[nodiscard]] static std::vector<std::string> splitIntoTerms(
      std::string_view expr)
  {
    std::vector<std::string> terms;
    std::string current_term;
    current_term.reserve(32);  // Reserve space for typical term length

    for (std::size_t i = 0; i < expr.size(); ++i) {
      char c = expr[i];

      if ((c == '+' || c == '-') && i > 0) {
        // This is a term separator
        if (!current_term.empty()) {
          terms.push_back(std::move(current_term));
          current_term.clear();
        }
        current_term += c;  // Start new term with sign
      } else {
        current_term += c;
      }
    }

    if (!current_term.empty()) {
      terms.push_back(std::move(current_term));
    }

    return terms;
  }

  // Parse one side of the constraint (left or right)
  [[nodiscard]] static std::pair<std::unordered_map<std::string, double>,
                                 double>
  parseSide(std::string_view side)
  {
    std::unordered_map<std::string, double> coefficients;
    double constant = 0.0;

    auto terms = splitIntoTerms(side);

    for (const auto& term : terms) {
      auto [coeff, var] = parseTerm(term);

      if (var.empty()) {
        // Constant term
        constant += coeff;
      } else {
        // Variable term
        coefficients[var] += coeff;
      }
    }

    return {std::move(coefficients), constant};
  }

public:
  [[nodiscard]] static ParseResult parse(std::string_view expression)
  {
    // Remove whitespace
    std::string cleaned = removeWhitespace(expression);

    if (cleaned.empty()) {
      throw std::invalid_argument("Empty expression");
    }

    // Find constraint operator(s)
    auto constraint_info = findConstraintOperator(cleaned);

    if (std::holds_alternative<std::pair<ConstraintType, std::size_t>>(
            constraint_info))
    {
      // Single constraint
      auto [constraint_type, op_pos] =
          std::get<std::pair<ConstraintType, std::size_t>>(constraint_info);

      // Determine operator length
      constexpr auto getOpLength =
          [](ConstraintType type) noexcept -> std::size_t
      { return (type == ConstraintType::EQUAL) ? 1 : 2; };

      std::size_t op_length = getOpLength(constraint_type);

      // Split into left and right sides
      std::string_view left_side {cleaned.data(), op_pos};
      std::string_view right_side {cleaned.data() + op_pos + op_length,
                                   cleaned.size() - op_pos - op_length};

      if (left_side.empty() || right_side.empty()) {
        throw std::invalid_argument("Empty left or right side of constraint");
      }

      // Parse both sides
      auto [left_coeffs, left_const] = parseSide(left_side);
      auto [right_coeffs, right_const] = parseSide(right_side);

      // Move all variables to left side, constants to right side
      std::unordered_map<std::string, double> final_coeffs =
          std::move(left_coeffs);

      for (const auto& [var, coeff] : right_coeffs) {
        final_coeffs[var] -= coeff;
      }

      double final_rhs = right_const - left_const;

      // Remove zero coefficients using erase_if (C++20)
      std::erase_if(final_coeffs,
                    [](const auto& pair) noexcept
                    { return std::abs(pair.second) < 1e-10; });

      return ParseResult {.coefficients = std::move(final_coeffs),
                          .rhs = final_rhs,
                          .constraint_type = constraint_type,
                          .lower_bound = std::nullopt,
                          .upper_bound = std::nullopt};
    } else {
      // Range constraint: lower <= expr <= upper or upper >= expr >= lower
      auto [constraint_type, pos1, pos2] =
          std::get<std::tuple<ConstraintType, std::size_t, std::size_t>>(
              constraint_info);

      // Split into three parts
      std::string_view part1 {cleaned.data(), pos1};
      std::string_view part2 {cleaned.data() + pos1 + 2, pos2 - pos1 - 2};
      std::string_view part3 {cleaned.data() + pos2 + 2,
                              cleaned.size() - pos2 - 2};

      if (part1.empty() || part2.empty() || part3.empty()) {
        throw std::invalid_argument("Empty parts in range constraint");
      }

      // Parse all three parts
      auto [coeffs1, const1] = parseSide(part1);
      auto [coeffs2, const2] = parseSide(part2);
      auto [coeffs3, const3] = parseSide(part3);

      // Determine which part contains variables (should be the middle one)
      bool part1_has_vars = !coeffs1.empty();
      bool part2_has_vars = !coeffs2.empty();
      bool part3_has_vars = !coeffs3.empty();

      std::unordered_map<std::string, double> final_coeffs;
      double lower_bound, upper_bound;

      if (part2_has_vars && !part1_has_vars && !part3_has_vars) {
        // Standard case: const1 <= expr <= const3
        final_coeffs = std::move(coeffs2);

        // Check if operators are <= (normal) or >= (reversed)
        std::string_view op1 {cleaned.data() + pos1, 2};
        if (op1 == "<=") {
          lower_bound = const1 - const2;
          upper_bound = const3 - const2;
        } else {  // >=
          // For >=, the bounds are reversed
          lower_bound = const3 - const2;
          upper_bound = const1 - const2;
        }
      } else if (part1_has_vars && !part2_has_vars && !part3_has_vars) {
        // expr <= const2 <= const3 (unusual but possible)
        final_coeffs = std::move(coeffs1);
        lower_bound = const2 - const1;
        upper_bound = const3 - const1;
      } else if (part3_has_vars && !part1_has_vars && !part2_has_vars) {
        // const1 <= const2 <= expr (unusual but possible)
        final_coeffs = std::move(coeffs3);
        lower_bound = const1 - const3;
        upper_bound = const2 - const3;
      } else {
        throw std::invalid_argument(
            "Range constraint must have variables in exactly one part");
      }

      // Remove zero coefficients using erase_if (C++20)
      std::erase_if(final_coeffs,
                    [](const auto& pair) noexcept
                    { return std::abs(pair.second) < 1e-10; });

      return ParseResult {.coefficients = std::move(final_coeffs),
                          .rhs = 0.0,  // Not used for range constraints
                          .constraint_type = ConstraintType::RANGE,
                          .lower_bound = lower_bound,
                          .upper_bound = upper_bound};
    }
  }
};

// Example usage and testing

void printResult(const ParseResult& result)
{
  std::cout << "Coefficients: ";
  for (const auto& [var, coeff] : result.coefficients) {
    std::cout << var << ":" << coeff << " ";
  }

  if (result.constraint_type == ConstraintType::RANGE) {
    std::cout << "\nLower bound: " << result.lower_bound.value_or(0);
    std::cout << "\nUpper bound: " << result.upper_bound.value_or(0);
    std::cout << "\nConstraint: RANGE";
  } else {
    std::cout << "\nRHS: " << result.rhs;
    std::cout << "\nConstraint: ";
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

int do_main()
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
    std::cout << "Expression: " << expr << std::endl;
    try {
      auto result = LinearParser::parse(expr);
      printResult(result);

      // Example: get coefficient vector for specific variable order
      auto vars = result.getVariableNames();
      if (!vars.empty()) {
        auto coeff_vector = result.getCoefficientsVector(vars);
        std::cout << "Coefficient vector [";
        for (std::size_t i = 0; i < coeff_vector.size(); ++i) {
          std::cout << coeff_vector[i];
          if (i < coeff_vector.size() - 1)
            std::cout << ", ";
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
