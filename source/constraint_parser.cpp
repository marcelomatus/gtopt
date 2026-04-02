/**
 * @file      constraint_parser.cpp
 * @brief     Implementation of the user constraint expression parser
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <cctype>
#include <format>
#include <stdexcept>
#include <utility>

#include <gtopt/constraint_parser.hpp>

namespace gtopt
{

// ── Comment stripping ───────────────────────────────────────────────────────

std::string ConstraintParser::strip_comments(std::string_view input)
{
  std::string result;
  result.reserve(input.size());

  for (std::size_t i = 0; i < input.size(); ++i) {
    // Line comment: # or //
    if (input[i] == '#') {
      // Skip to end of line
      while (i < input.size() && input[i] != '\n') {
        ++i;
      }
      // Replace comment with whitespace to preserve line structure
      if (i < input.size()) {
        result += ' ';
      }
      continue;
    }
    if (input[i] == '/' && i + 1 < input.size() && input[i + 1] == '/') {
      // Skip to end of line
      while (i < input.size() && input[i] != '\n') {
        ++i;
      }
      if (i < input.size()) {
        result += ' ';
      }
      continue;
    }
    result += input[i];
  }
  return result;
}

// ── Lexer implementation ────────────────────────────────────────────────────

ConstraintParser::Lexer::Lexer(std::string_view input) noexcept
    : m_input_(input)
{
}

void ConstraintParser::Lexer::skip_whitespace_and_comments() noexcept
{
  while (m_pos_ < m_input_.size()
         && std::isspace(static_cast<unsigned char>(m_input_[m_pos_])) != 0)
  {
    ++m_pos_;
  }
}

ConstraintParser::Token ConstraintParser::Lexer::scan_number()
{
  const auto start = m_pos_;
  while (m_pos_ < m_input_.size()
         && (std::isdigit(static_cast<unsigned char>(m_input_[m_pos_])) != 0
             || (m_input_[m_pos_] == '.' && m_pos_ + 1 < m_input_.size()
                 && m_input_[m_pos_ + 1] != '.')))
  {
    ++m_pos_;
  }
  return Token {
      .type = TokenType::NUMBER,
      .value = std::string {m_input_.substr(start, m_pos_ - start)},
  };
}

ConstraintParser::Token ConstraintParser::Lexer::scan_string()
{
  const char quote = m_input_[m_pos_];  // remember opening quote (" or ')
  ++m_pos_;  // skip opening quote
  std::string result;
  while (m_pos_ < m_input_.size() && m_input_[m_pos_] != quote) {
    if (m_input_[m_pos_] == '\\' && m_pos_ + 1 < m_input_.size()) {
      ++m_pos_;  // skip backslash
    }
    result += m_input_[m_pos_];
    ++m_pos_;
  }
  if (m_pos_ >= m_input_.size()) {
    throw std::invalid_argument("Unterminated string literal");
  }
  ++m_pos_;  // skip closing quote
  return Token {
      .type = TokenType::STRING,
      .value = std::move(result),
  };
}

ConstraintParser::Token ConstraintParser::Lexer::scan_ident()
{
  const auto start = m_pos_;
  while (m_pos_ < m_input_.size()
         && (std::isalnum(static_cast<unsigned char>(m_input_[m_pos_])) != 0
             || m_input_[m_pos_] == '_'))
  {
    ++m_pos_;
  }
  return Token {
      .type = TokenType::IDENT,
      .value = std::string {m_input_.substr(start, m_pos_ - start)},
  };
}

ConstraintParser::Token ConstraintParser::Lexer::next()
{
  skip_whitespace_and_comments();

  if (m_pos_ >= m_input_.size()) {
    return Token {
        .type = TokenType::END,
        .value = {},
    };
  }

  const char c = m_input_[m_pos_];

  // Two-character operators
  if (m_pos_ + 1 < m_input_.size()) {
    const char next_c = m_input_[m_pos_ + 1];
    if (c == '<' && next_c == '=') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::LEQ,
          .value = "<=",
      };
    }
    if (c == '>' && next_c == '=') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::GEQ,
          .value = ">=",
      };
    }
    if (c == '.' && next_c == '.') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::DOTDOT,
          .value = "..",
      };
    }
  }

  // Single-character tokens
  switch (c) {
    case '(':
      ++m_pos_;
      return Token {
          .type = TokenType::LPAREN,
          .value = "(",
      };
    case ')':
      ++m_pos_;
      return Token {
          .type = TokenType::RPAREN,
          .value = ")",
      };
    case '{':
      ++m_pos_;
      return Token {
          .type = TokenType::LBRACE,
          .value = "{",
      };
    case '}':
      ++m_pos_;
      return Token {
          .type = TokenType::RBRACE,
          .value = "}",
      };
    case '.':
      ++m_pos_;
      return Token {
          .type = TokenType::DOT,
          .value = ".",
      };
    case '*':
      ++m_pos_;
      return Token {
          .type = TokenType::STAR,
          .value = "*",
      };
    case '+':
      ++m_pos_;
      return Token {
          .type = TokenType::PLUS,
          .value = "+",
      };
    case '-':
      ++m_pos_;
      return Token {
          .type = TokenType::MINUS,
          .value = "-",
      };
    case '=':
      ++m_pos_;
      return Token {
          .type = TokenType::EQ,
          .value = "=",
      };
    case ',':
      ++m_pos_;
      return Token {
          .type = TokenType::COMMA,
          .value = ",",
      };
    case '"':
    case '\'':
      return scan_string();
    default:
      break;
  }

  // Numbers
  if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
    return scan_number();
  }

  // Identifiers
  if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
    return scan_ident();
  }

  throw std::invalid_argument(
      std::format("Unexpected character '{}' at position {}", c, m_pos_));
}

ConstraintParser::Token ConstraintParser::Lexer::peek()
{
  const auto saved_pos = m_pos_;
  auto token = next();
  m_pos_ = saved_pos;
  return token;
}

// ── Parser implementation ─────────────────────────────────────────────────

ConstraintParser::Parser::Parser(Lexer lexer)
    : m_lexer_(lexer)
{
  advance();
}

void ConstraintParser::Parser::advance()
{
  m_current_ = m_lexer_.next();
}

void ConstraintParser::Parser::expect(TokenType type)
{
  if (m_current_.type != type) {
    throw std::invalid_argument(
        std::format("Expected token type {}, got '{}' (type {})",
                    static_cast<int>(type),
                    m_current_.value,
                    static_cast<int>(m_current_.type)));
  }
  advance();
}

bool ConstraintParser::Parser::match(TokenType type)
{
  if (m_current_.type == type) {
    advance();
    return true;
  }
  return false;
}

bool ConstraintParser::Parser::is_element_type(const std::string& name)
{
  return name == "generator" || name == "demand" || name == "line"
      || name == "battery" || name == "converter" || name == "reservoir"
      || name == "bus" || name == "waterway" || name == "turbine"
      || name == "junction" || name == "flow" || name == "seepage"
      || name == "reserve_provision" || name == "reserve_zone"
      || name == "flow_right" || name == "volume_right";
}

ConstraintExpr ConstraintParser::Parser::parse_constraint()
{
  ConstraintExpr result;

  // Parse the left-hand side expression
  auto lhs_terms = parse_expr();

  // Determine constraint type
  ConstraintType ctype {};
  if (m_current_.type == TokenType::LEQ) {
    ctype = ConstraintType::LESS_EQUAL;
    advance();
  } else if (m_current_.type == TokenType::GEQ) {
    ctype = ConstraintType::GREATER_EQUAL;
    advance();
  } else if (m_current_.type == TokenType::EQ) {
    ctype = ConstraintType::EQUAL;
    advance();
  } else {
    throw std::invalid_argument(
        std::format("Expected constraint operator (<=, >=, =), got '{}'",
                    m_current_.value));
  }

  // Parse the right-hand side expression
  auto rhs_terms = parse_expr();

  // Check for range constraint: lhs <= mid <= rhs
  if ((ctype == ConstraintType::LESS_EQUAL && m_current_.type == TokenType::LEQ)
      || (ctype == ConstraintType::GREATER_EQUAL
          && m_current_.type == TokenType::GEQ))
  {
    // This is a range constraint
    advance();
    auto rhs2_terms = parse_expr();

    // For range: lower <= expr <= upper
    // lhs_terms should be a constant (the lower bound)
    // rhs_terms is the expression
    // rhs2_terms is the upper bound (constant)
    double lower_val = 0.0;
    double upper_val = 0.0;

    // Extract lower bound from lhs_terms
    for (const auto& term : lhs_terms) {
      if (term.element.has_value() || term.sum_ref.has_value()) {
        throw std::invalid_argument(
            "Range constraint bounds must be constants");
      }
      lower_val += term.coefficient;
    }

    // Extract upper bound from rhs2_terms
    for (const auto& term : rhs2_terms) {
      if (term.element.has_value() || term.sum_ref.has_value()) {
        throw std::invalid_argument(
            "Range constraint bounds must be constants");
      }
      upper_val += term.coefficient;
    }

    // The middle expression (rhs_terms) becomes the constraint terms
    // Adjust bounds for any constants in the middle expression
    double mid_const = 0.0;
    std::vector<ConstraintTerm> var_terms;
    for (auto& term : rhs_terms) {
      if (term.element.has_value() || term.sum_ref.has_value()) {
        var_terms.push_back(std::move(term));
      } else {
        mid_const += term.coefficient;
      }
    }

    if (ctype == ConstraintType::GREATER_EQUAL) {
      // >= range: upper >= expr >= lower
      std::swap(lower_val, upper_val);
    }

    result.terms = std::move(var_terms);
    result.constraint_type = ConstraintType::RANGE;
    result.lower_bound = lower_val - mid_const;
    result.upper_bound = upper_val - mid_const;
  } else {
    // Single constraint: move all variables to LHS, constants to RHS
    double rhs_const = 0.0;
    std::vector<ConstraintTerm> all_terms;

    // LHS terms stay as-is
    for (auto& term : lhs_terms) {
      if (term.element.has_value() || term.sum_ref.has_value()) {
        all_terms.push_back(std::move(term));
      } else {
        rhs_const -= term.coefficient;  // move constant to RHS
      }
    }

    // RHS terms get negated and moved to LHS
    for (auto& term : rhs_terms) {
      if (term.element.has_value() || term.sum_ref.has_value()) {
        term.coefficient = -term.coefficient;
        all_terms.push_back(std::move(term));
      } else {
        rhs_const += term.coefficient;
      }
    }

    result.terms = std::move(all_terms);
    result.constraint_type = ctype;
    result.rhs = rhs_const;
  }

  // Parse optional for clause
  if (m_current_.type == TokenType::COMMA) {
    advance();
    if (m_current_.type == TokenType::IDENT && m_current_.value == "for") {
      advance();
      result.domain = parse_for_clause();
    }
  } else if (m_current_.type == TokenType::IDENT && m_current_.value == "for") {
    advance();
    result.domain = parse_for_clause();
  }

  return result;
}

std::vector<ConstraintTerm> ConstraintParser::Parser::parse_expr()
{
  std::vector<ConstraintTerm> terms;

  // Handle leading minus
  bool negate = false;
  if (m_current_.type == TokenType::MINUS) {
    negate = true;
    advance();
  } else if (m_current_.type == TokenType::PLUS) {
    advance();
  }

  terms.push_back(parse_term(negate));

  while (m_current_.type == TokenType::PLUS
         || m_current_.type == TokenType::MINUS)
  {
    const bool neg = (m_current_.type == TokenType::MINUS);
    advance();
    terms.push_back(parse_term(neg));
  }

  return terms;
}

ConstraintTerm ConstraintParser::Parser::parse_term(bool negate)
{
  ConstraintTerm term;
  const double sign = negate ? -1.0 : 1.0;

  // Check for sum(...) aggregation
  if (m_current_.type == TokenType::IDENT && m_current_.value == "sum") {
    return parse_sum_expr(sign);
  }

  if (m_current_.type == TokenType::NUMBER) {
    // Could be: number, number * element_ref, number * sum(...), or constant
    const double num = std::stod(m_current_.value);
    advance();

    if (m_current_.type == TokenType::STAR) {
      // number * element_ref  or  number * sum(...)
      advance();
      if (m_current_.type == TokenType::IDENT && m_current_.value == "sum") {
        // number * sum(...)
        auto sum_term = parse_sum_expr(sign * num);
        return sum_term;
      }
      if (m_current_.type == TokenType::IDENT
          && is_element_type(m_current_.value))
      {
        auto ref = parse_element_ref(std::move(m_current_.value));
        term.coefficient = sign * num;
        term.element = std::move(ref);
      } else {
        throw std::invalid_argument(
            std::format("Expected element type or 'sum' after '*', got '{}'",
                        m_current_.value));
      }
    } else {
      // Standalone number (constant term)
      term.coefficient = sign * num;
      term.element = std::nullopt;
    }
  } else if (m_current_.type == TokenType::IDENT
             && is_element_type(m_current_.value))
  {
    // element_ref without explicit coefficient (coefficient = 1.0)
    auto ref = parse_element_ref(std::move(m_current_.value));
    term.coefficient = sign;
    term.element = std::move(ref);
  } else {
    throw std::invalid_argument(
        std::format("Expected number, element reference, or 'sum', got '{}'",
                    m_current_.value));
  }

  return term;
}

ConstraintTerm ConstraintParser::Parser::parse_sum_expr(double sign)
{
  // Current token is 'sum', advance past it
  advance();
  expect(TokenType::LPAREN);

  // Expect: element_type ( id_list ) . attribute )
  if (m_current_.type != TokenType::IDENT || !is_element_type(m_current_.value))
  {
    throw std::invalid_argument(std::format(
        "Expected element type in sum(), got '{}'", m_current_.value));
  }

  SumElementRef sum_ref;
  sum_ref.element_type = std::move(m_current_.value);
  advance();

  expect(TokenType::LPAREN);

  // Parse element list: "id1", "id2", ...  or  3, 5, ...  or  all
  if (m_current_.type == TokenType::IDENT && m_current_.value == "all") {
    sum_ref.all_elements = true;
    advance();
    // Optional: , type="value"
    if (m_current_.type == TokenType::COMMA) {
      advance();
      if (m_current_.type == TokenType::IDENT && m_current_.value == "type") {
        advance();
        expect(TokenType::EQ);
        if (m_current_.type != TokenType::STRING) {
          throw std::invalid_argument(std::format(
              "Expected quoted string after 'type=' in sum(), got '{}'",
              m_current_.value));
        }
        sum_ref.type_filter = std::move(m_current_.value);
        advance();
      } else {
        throw std::invalid_argument(std::format(
            "Expected 'type=...' after comma in sum(all,...), got '{}'",
            m_current_.value));
      }
    }
  } else {
    // Parse comma-separated string or number list
    while (true) {
      if (m_current_.type == TokenType::STRING) {
        // Quoted name: "G1" or "uid:3"
        sum_ref.element_ids.push_back(std::move(m_current_.value));
        advance();
      } else if (m_current_.type == TokenType::NUMBER) {
        // Bare numeric UID: 3 → stored as "uid:3"
        sum_ref.element_ids.push_back("uid:" + m_current_.value);
        advance();
      } else {
        throw std::invalid_argument(std::format(
            "Expected element name (quoted) or numeric UID in sum(), got '{}'",
            m_current_.value));
      }

      if (m_current_.type == TokenType::COMMA) {
        advance();
      } else {
        break;
      }
    }
  }

  expect(TokenType::RPAREN);
  expect(TokenType::DOT);

  if (m_current_.type != TokenType::IDENT) {
    throw std::invalid_argument(std::format(
        "Expected attribute name after '.', got '{}'", m_current_.value));
  }
  sum_ref.attribute = std::move(m_current_.value);
  advance();

  expect(TokenType::RPAREN);

  ConstraintTerm term;
  term.coefficient = sign;
  term.sum_ref = std::move(sum_ref);
  return term;
}

ElementRef ConstraintParser::Parser::parse_element_ref(std::string type_name)
{
  ElementRef ref;
  ref.element_type = std::move(type_name);

  // Expect: ( "id" ) . attribute  or  ( number ) . attribute
  advance();  // skip element type identifier
  expect(TokenType::LPAREN);

  if (m_current_.type == TokenType::STRING) {
    // Quoted name: generator("G1") or generator("uid:3")
    ref.element_id = std::move(m_current_.value);
    advance();
  } else if (m_current_.type == TokenType::NUMBER) {
    // Bare numeric UID: generator(3) → stored as "uid:3"
    ref.element_id = "uid:" + m_current_.value;
    advance();
  } else {
    throw std::invalid_argument(std::format(
        "Expected element name (quoted string) or numeric UID, got '{}'",
        m_current_.value));
  }

  expect(TokenType::RPAREN);
  expect(TokenType::DOT);

  if (m_current_.type != TokenType::IDENT) {
    throw std::invalid_argument(std::format(
        "Expected attribute name after '.', got '{}'", m_current_.value));
  }
  ref.attribute = std::move(m_current_.value);
  advance();

  return ref;
}

ConstraintDomain ConstraintParser::Parser::parse_for_clause()
{
  ConstraintDomain domain;

  // Set defaults to "all" for unspecified dimensions
  domain.scenarios = IndexRange {
      .is_all = true,
      .values = {},
  };
  domain.stages = IndexRange {
      .is_all = true,
      .values = {},
  };
  domain.blocks = IndexRange {
      .is_all = true,
      .values = {},
  };

  expect(TokenType::LPAREN);

  while (m_current_.type != TokenType::RPAREN
         && m_current_.type != TokenType::END)
  {
    if (m_current_.type != TokenType::IDENT) {
      throw std::invalid_argument(std::format(
          "Expected dimension name in for clause, got '{}'", m_current_.value));
    }

    std::string dim = std::move(m_current_.value);
    advance();

    // Accept 'in' or '='
    if ((m_current_.type == TokenType::IDENT && m_current_.value == "in")
        || m_current_.type == TokenType::EQ)
    {
      advance();
    } else {
      throw std::invalid_argument(
          std::format("Expected 'in' or '=' after dimension '{}', got '{}'",
                      dim,
                      m_current_.value));
    }

    auto index_set = parse_index_set();

    if (dim == "scenario") {
      domain.scenarios = std::move(index_set);
    } else if (dim == "stage") {
      domain.stages = std::move(index_set);
    } else if (dim == "block") {
      domain.blocks = std::move(index_set);
    } else {
      throw std::invalid_argument(
          std::format("Unknown dimension '{}' in for clause (expected "
                      "'scenario', 'stage', or 'block')",
                      dim));
    }

    // Skip comma between dimension specs
    if (m_current_.type == TokenType::COMMA) {
      advance();
    }
  }

  expect(TokenType::RPAREN);
  return domain;
}

IndexRange ConstraintParser::Parser::parse_index_set()
{
  IndexRange range;

  // Check for 'all' keyword
  if (m_current_.type == TokenType::IDENT && m_current_.value == "all") {
    range.is_all = true;
    advance();
    return range;
  }

  range.is_all = false;

  // Check for brace-enclosed set: {1, 3..5, 8}
  if (m_current_.type == TokenType::LBRACE) {
    advance();

    while (m_current_.type != TokenType::RBRACE
           && m_current_.type != TokenType::END)
    {
      if (m_current_.type != TokenType::NUMBER) {
        throw std::invalid_argument(std::format(
            "Expected number in index set, got '{}'", m_current_.value));
      }
      const int start = std::stoi(m_current_.value);
      advance();

      if (m_current_.type == TokenType::DOTDOT) {
        // Range: start..end
        advance();
        if (m_current_.type != TokenType::NUMBER) {
          throw std::invalid_argument(std::format(
              "Expected number after '..', got '{}'", m_current_.value));
        }
        const int end = std::stoi(m_current_.value);
        advance();
        range.values.reserve(range.values.size() + static_cast<size_t>(end)
                             - static_cast<size_t>(start) + 1);
        for (int i = start; i <= end; ++i) {
          range.values.push_back(i);
        }
      } else {
        range.values.push_back(start);
      }

      if (m_current_.type == TokenType::COMMA) {
        advance();
      }
    }

    expect(TokenType::RBRACE);
    return range;
  }

  // Single number or bare range: N or N..M
  if (m_current_.type == TokenType::NUMBER) {
    const int start = std::stoi(m_current_.value);
    advance();

    if (m_current_.type == TokenType::DOTDOT) {
      // Bare range: N..M
      advance();
      if (m_current_.type != TokenType::NUMBER) {
        throw std::invalid_argument(std::format(
            "Expected number after '..', got '{}'", m_current_.value));
      }
      const int end = std::stoi(m_current_.value);
      advance();
      range.values.reserve(static_cast<size_t>(end) - static_cast<size_t>(start)
                           + 1);
      for (int i = start; i <= end; ++i) {
        range.values.push_back(i);
      }
    } else {
      range.values.push_back(start);
    }
    return range;
  }

  throw std::invalid_argument(
      std::format("Expected index set (number, range, or 'all'), got '{}'",
                  m_current_.value));
}

// ── Public API ──────────────────────────────────────────────────────────────

ConstraintExpr ConstraintParser::parse(std::string_view name,
                                       std::string_view expression)
{
  if (expression.empty()) {
    throw std::invalid_argument("Empty constraint expression");
  }

  // Strip comments before tokenizing
  const std::string cleaned = strip_comments(expression);

  const Lexer lexer(cleaned);
  Parser parser(lexer);
  auto result = parser.parse_constraint();
  result.name = std::string {name};
  return result;
}

ConstraintExpr ConstraintParser::parse(std::string_view expression)
{
  return parse("", expression);
}

}  // namespace gtopt
