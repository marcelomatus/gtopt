/**
 * @file      constraint_parser.cpp
 * @brief     Implementation of the user constraint expression parser
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <gtopt/constraint_parser.hpp>

namespace gtopt
{

namespace
{

/// Format a diagnostic error message with caret pointer and optional hint.
///
/// Layout:
/// ```
/// Parse error at column N: <message>
///   <source>
///        ^
///   hint: <hint>
/// ```
[[nodiscard]] std::string format_error_with_caret(std::string_view source,
                                                  std::size_t column,
                                                  std::string_view message,
                                                  std::string_view hint)
{
  std::string out =
      std::format("Parse error at column {}: {}", column + 1, message);
  if (!source.empty()) {
    out += "\n  ";
    out += source;
    out += "\n  ";
    const std::size_t padding = column < source.size() ? column : source.size();
    out.append(padding, ' ');
    out += '^';
  }
  if (!hint.empty()) {
    out += "\n  hint: ";
    out += hint;
  }
  return out;
}

}  // namespace

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
      .start_pos = start,
  };
}

ConstraintParser::Token ConstraintParser::Lexer::scan_string()
{
  const auto start = m_pos_;
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
    constexpr std::string_view kMsg = "unterminated string literal";
    constexpr std::string_view kHint = "close the string with a matching quote";
    throw ConstraintParseError(
        format_error_with_caret(m_input_, start, kMsg, kHint),
        std::string {kMsg},
        std::string {kHint},
        start);
  }
  ++m_pos_;  // skip closing quote
  return Token {
      .type = TokenType::STRING,
      .value = std::move(result),
      .start_pos = start,
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
      .start_pos = start,
  };
}

ConstraintParser::Token ConstraintParser::Lexer::next()
{
  skip_whitespace_and_comments();

  if (m_pos_ >= m_input_.size()) {
    return Token {
        .type = TokenType::END,
        .value = {},
        .start_pos = m_pos_,
    };
  }

  const auto start = m_pos_;
  const char c = m_input_[m_pos_];

  // Two-character operators
  if (m_pos_ + 1 < m_input_.size()) {
    const char next_c = m_input_[m_pos_ + 1];
    if (c == '<' && next_c == '=') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::LEQ,
          .value = "<=",
          .start_pos = start,
      };
    }
    if (c == '>' && next_c == '=') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::GEQ,
          .value = ">=",
          .start_pos = start,
      };
    }
    if (c == '<' && next_c == '>') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::NE,
          .value = "<>",
          .start_pos = start,
      };
    }
    if (c == '!' && next_c == '=') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::NE,
          .value = "!=",
          .start_pos = start,
      };
    }
    if (c == '=' && next_c == '=') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::EQ,
          .value = "==",
          .start_pos = start,
      };
    }
    if (c == '.' && next_c == '.') {
      m_pos_ += 2;
      return Token {
          .type = TokenType::DOTDOT,
          .value = "..",
          .start_pos = start,
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
          .start_pos = start,
      };
    case ')':
      ++m_pos_;
      return Token {
          .type = TokenType::RPAREN,
          .value = ")",
          .start_pos = start,
      };
    case '{':
      ++m_pos_;
      return Token {
          .type = TokenType::LBRACE,
          .value = "{",
          .start_pos = start,
      };
    case '}':
      ++m_pos_;
      return Token {
          .type = TokenType::RBRACE,
          .value = "}",
          .start_pos = start,
      };
    case '.':
      ++m_pos_;
      return Token {
          .type = TokenType::DOT,
          .value = ".",
          .start_pos = start,
      };
    case '*':
      ++m_pos_;
      return Token {
          .type = TokenType::STAR,
          .value = "*",
          .start_pos = start,
      };
    case '/':
      // `//` line comments are stripped before lexing, so a bare `/`
      // is always division.
      ++m_pos_;
      return Token {
          .type = TokenType::SLASH,
          .value = "/",
          .start_pos = start,
      };
    case '+':
      ++m_pos_;
      return Token {
          .type = TokenType::PLUS,
          .value = "+",
          .start_pos = start,
      };
    case '-':
      ++m_pos_;
      return Token {
          .type = TokenType::MINUS,
          .value = "-",
          .start_pos = start,
      };
    case '=':
      ++m_pos_;
      return Token {
          .type = TokenType::EQ,
          .value = "=",
          .start_pos = start,
      };
    case '<':
      ++m_pos_;
      return Token {
          .type = TokenType::LT,
          .value = "<",
          .start_pos = start,
      };
    case '>':
      ++m_pos_;
      return Token {
          .type = TokenType::GT,
          .value = ">",
          .start_pos = start,
      };
    case ',':
      ++m_pos_;
      return Token {
          .type = TokenType::COMMA,
          .value = ",",
          .start_pos = start,
      };
    case ':':
      ++m_pos_;
      return Token {
          .type = TokenType::COLON,
          .value = ":",
          .start_pos = start,
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

  const std::string msg = std::format("unexpected character '{}'", c);
  constexpr std::string_view kHint =
      "remove the stray character or quote it as a string literal";
  throw ConstraintParseError(
      format_error_with_caret(m_input_, start, msg, kHint),
      msg,
      std::string {kHint},
      start);
}

ConstraintParser::Token ConstraintParser::Lexer::peek()
{
  const auto saved_pos = m_pos_;
  auto token = next();
  m_pos_ = saved_pos;
  return token;
}

// ── Parser implementation ─────────────────────────────────────────────────

ConstraintParser::Parser::Parser(Lexer lexer, std::string_view raw_source)
    : m_lexer_(lexer)
    , m_raw_source_(raw_source)
{
  advance();
}

void ConstraintParser::Parser::advance()
{
  m_current_ = m_lexer_.next();
}

std::string_view ConstraintParser::Parser::token_type_name(
    TokenType type) noexcept
{
  switch (type) {
    case TokenType::END:
      return "end of input";
    case TokenType::NUMBER:
      return "number";
    case TokenType::STRING:
      return "string literal";
    case TokenType::IDENT:
      return "identifier";
    case TokenType::LPAREN:
      return "'('";
    case TokenType::RPAREN:
      return "')'";
    case TokenType::LBRACE:
      return "'{'";
    case TokenType::RBRACE:
      return "'}'";
    case TokenType::DOT:
      return "'.'";
    case TokenType::STAR:
      return "'*'";
    case TokenType::SLASH:
      return "'/'";
    case TokenType::PLUS:
      return "'+'";
    case TokenType::MINUS:
      return "'-'";
    case TokenType::LEQ:
      return "'<='";
    case TokenType::GEQ:
      return "'>='";
    case TokenType::EQ:
      return "'='";
    case TokenType::LT:
      return "'<'";
    case TokenType::GT:
      return "'>'";
    case TokenType::NE:
      return "'!='";
    case TokenType::COMMA:
      return "','";
    case TokenType::COLON:
      return "':'";
    case TokenType::DOTDOT:
      return "'..'";
  }
  return "unknown token";
}

void ConstraintParser::Parser::error_at(std::size_t column,
                                        const std::string& message,
                                        std::string_view hint) const
{
  throw ConstraintParseError(
      format_error_with_caret(m_raw_source_, column, message, hint),
      message,
      std::string {hint},
      column);
}

void ConstraintParser::Parser::error_at_current(const std::string& message,
                                                std::string_view hint) const
{
  error_at(m_current_.start_pos, message, hint);
}

void ConstraintParser::Parser::expect(TokenType type)
{
  expect(type, {});
}

void ConstraintParser::Parser::expect(TokenType type, std::string_view hint)
{
  if (m_current_.type != type) {
    error_at_current(std::format("expected {}, got '{}'",
                                 token_type_name(type),
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     hint);
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

bool ConstraintParser::Parser::is_singleton_class(const std::string& name)
{
  // Singleton classes expose globally-scoped read-only scalars and are
  // parsed as `class.attribute` (no parens, no element id).  The set of
  // legal scalars is enforced by the resolver against the allow-list
  // registered in `system_lp.cpp::register_all_ampl_element_names`,
  // except for `stage.*` whose values come from the active StageLP at
  // resolution time (calendar metadata of the stage being assembled).
  return name == "options" || name == "system" || name == "stage";
}

ConstraintExpr ConstraintParser::Parser::parse_constraint()
{
  ConstraintExpr result;

  // Parse the left-hand side expression
  auto lhs_terms = parse_add_expr();

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
    error_at_current(
        std::format(
            "expected constraint operator (<=, >=, =), got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "use '=' for equality (not '==')");
  }

  // Parse the right-hand side expression
  auto rhs_terms = parse_add_expr();

  // Check for range constraint: lhs <= mid <= rhs
  if ((ctype == ConstraintType::LESS_EQUAL && m_current_.type == TokenType::LEQ)
      || (ctype == ConstraintType::GREATER_EQUAL
          && m_current_.type == TokenType::GEQ))
  {
    // This is a range constraint
    advance();
    auto rhs2_terms = parse_add_expr();

    // For range: lower <= expr <= upper
    // lhs_terms should be a constant (the lower bound)
    // rhs_terms is the expression
    // rhs2_terms is the upper bound (constant)
    double lower_val = 0.0;
    double upper_val = 0.0;

    auto is_non_const = [](const ConstraintTerm& t)
    {
      return t.element.has_value() || t.sum_ref.has_value()
          || t.param_name.has_value() || t.abs_expr || t.minmax_expr
          || t.if_expr;
    };

    // Extract lower bound from lhs_terms
    for (const auto& term : lhs_terms) {
      if (is_non_const(term)) {
        error_at_current(
            "range constraint bounds must be numeric constants",
            "use `lower <= expr <= upper` with constant lower and upper");
      }
      lower_val += term.coefficient;
    }

    // Extract upper bound from rhs2_terms
    for (const auto& term : rhs2_terms) {
      if (is_non_const(term)) {
        error_at_current(
            "range constraint bounds must be numeric constants",
            "use `lower <= expr <= upper` with constant lower and upper");
      }
      upper_val += term.coefficient;
    }

    // The middle expression (rhs_terms) becomes the constraint terms
    // Adjust bounds for any constants in the middle expression
    double mid_const = 0.0;
    std::vector<ConstraintTerm> var_terms;
    for (auto& term : rhs_terms) {
      if (is_non_const(term)) {
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

    // Helper: true if a term references a variable, named parameter,
    // or nonlinear wrapper (abs/min/max/if).
    auto is_non_constant = [](const ConstraintTerm& t)
    {
      return t.element.has_value() || t.sum_ref.has_value()
          || t.param_name.has_value() || t.abs_expr || t.minmax_expr
          || t.if_expr;
    };

    // LHS terms stay as-is
    for (auto& term : lhs_terms) {
      if (is_non_constant(term)) {
        all_terms.push_back(std::move(term));
      } else {
        rhs_const -= term.coefficient;  // move constant to RHS
      }
    }

    // RHS terms get negated and moved to LHS
    for (auto& term : rhs_terms) {
      if (is_non_constant(term)) {
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

namespace
{

// ── Linear-expression helpers ──────────────────────────────────────────────
//
// Expressions are represented as `std::vector<ConstraintTerm>`.  A term
// that has none of `element`, `sum_ref`, or `param_name` set is a pure
// constant contributing `coefficient` to the expression value.  The helpers
// below manipulate such vectors while preserving linearity.

/// Return the sum of coefficients if every term is a pure constant;
/// return nullopt if any term references a variable, parameter, or
/// nonlinear wrapper (`abs`, `min`/`max`, `if`).
std::optional<double> extract_constant(
    const std::vector<ConstraintTerm>& terms) noexcept
{
  double acc = 0.0;
  for (const auto& t : terms) {
    if (t.element || t.sum_ref || t.param_name || t.abs_expr || t.minmax_expr
        || t.if_expr)
    {
      return std::nullopt;
    }
    acc += t.coefficient;
  }
  return acc;
}

void scale_terms(std::vector<ConstraintTerm>& terms, double k) noexcept
{
  for (auto& t : terms) {
    t.coefficient *= k;
  }
}

/// Multiply two linear expressions.  At least one side must be a pure
/// constant; any product of two variable-bearing expressions is
/// non-linear and rejected at parse time.
std::vector<ConstraintTerm> multiply_terms(std::vector<ConstraintTerm> lhs,
                                           std::vector<ConstraintTerm> rhs)
{
  const auto l_const = extract_constant(lhs);
  const auto r_const = extract_constant(rhs);
  if (l_const && r_const) {
    return {
        ConstraintTerm {
            .coefficient = *l_const * *r_const,
        },
    };
  }
  if (r_const) {
    scale_terms(lhs, *r_const);
    return lhs;
  }
  if (l_const) {
    scale_terms(rhs, *l_const);
    return rhs;
  }
  throw std::invalid_argument(
      "Non-linear product: both sides of '*' contain variables or "
      "parameters; only scalar-by-expression products are allowed");
}

/// Divide a linear expression by a pure-constant expression.  The divisor
/// must collapse to a single numeric value; dividing by a variable or
/// parameter reference is rejected as non-linear.
std::vector<ConstraintTerm> divide_terms(std::vector<ConstraintTerm> lhs,
                                         const std::vector<ConstraintTerm>& rhs)
{
  const auto r_const = extract_constant(rhs);
  if (!r_const) {
    throw std::invalid_argument(
        "Division by a non-constant expression is not allowed; "
        "the divisor must evaluate to a numeric literal");
  }
  if (*r_const == 0.0) {
    throw std::invalid_argument("Division by zero in constraint expression");
  }
  scale_terms(lhs, 1.0 / *r_const);
  return lhs;
}

}  // namespace

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_add_expr()
{
  auto terms = parse_mul_expr();

  while (m_current_.type == TokenType::PLUS
         || m_current_.type == TokenType::MINUS)
  {
    const bool neg = (m_current_.type == TokenType::MINUS);
    advance();
    auto rhs = parse_mul_expr();
    if (neg) {
      scale_terms(rhs, -1.0);
    }
    terms.insert(terms.end(),
                 std::make_move_iterator(rhs.begin()),
                 std::make_move_iterator(rhs.end()));
  }

  return terms;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_mul_expr()
{
  auto terms = parse_unary();

  while (m_current_.type == TokenType::STAR
         || m_current_.type == TokenType::SLASH)
  {
    const bool is_div = (m_current_.type == TokenType::SLASH);
    const auto op_col = m_current_.start_pos;
    advance();
    auto rhs = parse_unary();
    try {
      terms = is_div ? divide_terms(std::move(terms), rhs)
                     : multiply_terms(std::move(terms), std::move(rhs));
    } catch (const std::invalid_argument& e) {
      error_at(op_col,
               e.what(),
               is_div ? "divisor must be a numeric constant (no variables)"
                      : "at least one operand of '*' must be a constant");
    }
  }

  return terms;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_unary()
{
  if (m_current_.type == TokenType::PLUS) {
    advance();
    return parse_unary();
  }
  if (m_current_.type == TokenType::MINUS) {
    advance();
    auto terms = parse_unary();
    scale_terms(terms, -1.0);
    return terms;
  }
  return parse_primary();
}

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_primary()
{
  // Parenthesized subexpression
  if (m_current_.type == TokenType::LPAREN) {
    advance();
    auto inner = parse_add_expr();
    expect(TokenType::RPAREN);
    return inner;
  }

  // Numeric literal
  if (m_current_.type == TokenType::NUMBER) {
    ConstraintTerm t {
        .coefficient = std::stod(m_current_.value),
    };
    advance();
    return {
        std::move(t),
    };
  }

  // sum(...) aggregation
  if (m_current_.type == TokenType::IDENT && m_current_.value == "sum") {
    return {
        parse_sum_expr(),
    };
  }

  // abs(linear_expr) — F5 auto-linearization
  if (m_current_.type == TokenType::IDENT && m_current_.value == "abs") {
    return parse_abs_expr();
  }

  // state(element_ref) — Phase 1e cross-phase state-variable wrapper
  if (m_current_.type == TokenType::IDENT && m_current_.value == "state") {
    return parse_state_expr();
  }

  // max(arg1, arg2, ...) / min(arg1, arg2, ...) — F7 auto-linearization
  if (m_current_.type == TokenType::IDENT
      && (m_current_.value == "max" || m_current_.value == "min"))
  {
    const auto kind =
        (m_current_.value == "max") ? MinMaxKind::Max : MinMaxKind::Min;
    return parse_minmax_expr(kind);
  }

  // if <cond> then (<expr>) [ else (<expr>) ] — F8 data conditional
  if (m_current_.type == TokenType::IDENT && m_current_.value == "if") {
    return parse_if_expr();
  }

  // Element reference or bare parameter
  if (m_current_.type == TokenType::IDENT) {
    if (is_element_type(m_current_.value)) {
      auto type_name = std::move(m_current_.value);
      auto ref = parse_element_ref(std::move(type_name));
      return {
          ConstraintTerm {
              .coefficient = 1.0,
              .element = std::move(ref),
          },
      };
    }
    // Singleton class scalar: `options.attribute`, `system.attribute`.
    // No element id, no parens — encoded as an ElementRef with empty
    // element_id and resolved at row-assembly time via the AMPL scalar
    // registry (see element_column_resolver.cpp::resolve_single_param).
    if (is_singleton_class(m_current_.value)) {
      auto class_name = std::move(m_current_.value);
      advance();
      expect(TokenType::DOT,
             "follow a singleton class with '.attribute' (no parens)");
      if (m_current_.type != TokenType::IDENT) {
        error_at_current(
            std::format(
                "expected scalar attribute name after '{}.', got '{}'",
                class_name,
                m_current_.value.empty() ? "end of input" : m_current_.value),
            "scalar attribute is an identifier, e.g. options.scale_objective");
      }
      ElementRef ref {
          .element_type = std::move(class_name),
          .element_id = {},
          .attribute = std::move(m_current_.value),
      };
      advance();
      return {
          ConstraintTerm {
              .coefficient = 1.0,
              .element = std::move(ref),
          },
      };
    }
    // Bare identifier → named parameter reference
    ConstraintTerm t {
        .coefficient = 1.0,
        .param_name = std::move(m_current_.value),
    };
    advance();
    return {
        std::move(t),
    };
  }

  error_at_current(
      std::format("expected number, '(', element reference, 'sum', "
                  "or parameter name, got '{}'",
                  m_current_.value.empty() ? "end of input" : m_current_.value),
      "valid primary: a number, `(expr)`, `element('id').attr`, `sum(...)`, "
      "or a parameter name");
}

ConstraintTerm ConstraintParser::Parser::parse_sum_expr()
{
  // Current token is 'sum', advance past it
  advance();
  expect(TokenType::LPAREN);

  // Expect: element_type ( id_list ) . attribute )
  if (m_current_.type != TokenType::IDENT || !is_element_type(m_current_.value))
  {
    error_at_current(
        std::format(
            "expected element type in sum(), got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "element types: generator, demand, line, battery, reservoir, ...");
  }

  SumElementRef sum_ref;
  sum_ref.element_type = std::move(m_current_.value);
  advance();

  expect(TokenType::LPAREN);

  // Parse element list: "id1", "id2", ...  or  3, 5, ...  or  all
  if (m_current_.type == TokenType::IDENT && m_current_.value == "all") {
    sum_ref.all_elements = true;
    advance();
    // Back-compat shortcut: `, type="..."` (legacy v0 single-filter form)
    // is lowered to a one-element `filters` entry.
    if (m_current_.type == TokenType::COMMA) {
      advance();
      if (m_current_.type == TokenType::IDENT && m_current_.value == "type") {
        advance();
        expect(TokenType::EQ);
        if (m_current_.type != TokenType::STRING) {
          error_at_current(
              std::format(
                  "expected quoted string after 'type=' in sum(), "
                  "got '{}'",
                  m_current_.value.empty() ? "end of input" : m_current_.value),
              "use a string literal, e.g. type=\"thermal\"");
        }
        SumPredicate pred;
        pred.attr = "type";
        pred.op = SumPredicate::Op::Eq;
        pred.string_value = std::move(m_current_.value);
        sum_ref.filters.push_back(std::move(pred));
        advance();
      } else {
        error_at_current(
            std::format(
                "expected 'type=...' after comma in sum(all,...), "
                "got '{}'",
                m_current_.value.empty() ? "end of input" : m_current_.value),
            "use the predicate form, e.g. "
            "sum(generator(all: type=\"thermal\" and bus=\"B1\").generation)");
      }
    }
    // New multi-predicate form: `all : pred1 and pred2 and ...`
    if (m_current_.type == TokenType::COLON) {
      advance();
      parse_sum_predicates(sum_ref);
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
        error_at_current(
            std::format(
                "expected element name (quoted) or numeric UID in "
                "sum(), got '{}'",
                m_current_.value.empty() ? "end of input" : m_current_.value),
            "use quoted names like 'G1' or bare UIDs like 3");
      }

      if (m_current_.type == TokenType::COMMA) {
        advance();
      } else {
        break;
      }
    }
  }

  expect(TokenType::RPAREN, "close the id list with ')'");
  expect(TokenType::DOT, "follow the id list with '.attribute'");

  if (m_current_.type != TokenType::IDENT) {
    error_at_current(std::format("expected attribute name after '.', got '{}'",
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     "attribute is an identifier like 'generation' or 'load'");
  }
  sum_ref.attribute = std::move(m_current_.value);
  advance();

  expect(TokenType::RPAREN, "close sum(...) with ')'");

  ConstraintTerm term;
  term.coefficient = 1.0;
  term.sum_ref = std::move(sum_ref);
  return term;
}

void ConstraintParser::Parser::parse_sum_predicates(SumElementRef& sum_ref)
{
  // Grammar (inside `sum(type(all : ... ).attr)`):
  //   predicates := predicate ( ("and" | "&&") predicate )*
  //   predicate  := IDENT op value
  //   op         := "=" | "==" | "!=" | "<>" | "<" | "<=" | ">" | ">=" | "in"
  //   value      := NUMBER | STRING | "{" value_list "}"
  sum_ref.filters.push_back(parse_one_predicate());
  while (m_current_.type == TokenType::IDENT
         && (m_current_.value == "and" || m_current_.value == "&&"))
  {
    advance();
    sum_ref.filters.push_back(parse_one_predicate());
  }
}

SumPredicate ConstraintParser::Parser::parse_one_predicate()
{
  SumPredicate pred;

  if (m_current_.type != TokenType::IDENT) {
    error_at_current(
        std::format(
            "expected filter attribute name, got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "filter predicates look like `type=\"thermal\"` or `cap>=50`");
  }
  pred.attr = std::move(m_current_.value);
  advance();

  // Operator
  if (m_current_.type == TokenType::EQ) {
    pred.op = SumPredicate::Op::Eq;
    advance();
  } else if (m_current_.type == TokenType::NE) {
    pred.op = SumPredicate::Op::Ne;
    advance();
  } else if (m_current_.type == TokenType::LT) {
    pred.op = SumPredicate::Op::Lt;
    advance();
  } else if (m_current_.type == TokenType::LEQ) {
    pred.op = SumPredicate::Op::Le;
    advance();
  } else if (m_current_.type == TokenType::GT) {
    pred.op = SumPredicate::Op::Gt;
    advance();
  } else if (m_current_.type == TokenType::GEQ) {
    pred.op = SumPredicate::Op::Ge;
    advance();
  } else if (m_current_.type == TokenType::IDENT && m_current_.value == "in") {
    pred.op = SumPredicate::Op::In;
    advance();
    expect(TokenType::LBRACE, "use `in { a, b, c }` for set membership");
    while (true) {
      if (m_current_.type != TokenType::STRING
          && m_current_.type != TokenType::NUMBER)
      {
        error_at_current(
            std::format(
                "expected string or number in set, got '{}'",
                m_current_.value.empty() ? "end of input" : m_current_.value),
            "set members must be numeric or quoted string literals");
      }
      pred.set_values.push_back(std::move(m_current_.value));
      advance();
      if (m_current_.type != TokenType::COMMA) {
        break;
      }
      advance();
    }
    expect(TokenType::RBRACE, "close the set with '}'");
    return pred;
  } else {
    error_at_current(std::format("expected comparison operator, got '{}'",
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     "valid ops: = == != <> < <= > >= in");
  }

  // Value: number or string literal
  if (m_current_.type == TokenType::NUMBER) {
    double val = 0.0;
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto& s = m_current_.value;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (ec != std::errc {}) {
      error_at_current(std::format("invalid numeric value '{}'", s),
                       "expected a decimal number on the RHS of the predicate");
    }
    pred.number_value = val;
    advance();
  } else if (m_current_.type == TokenType::STRING
             || m_current_.type == TokenType::IDENT)
  {
    // STRING literal or bare identifier (e.g. `bus=B1`) — treat as string.
    pred.string_value = std::move(m_current_.value);
    advance();
  } else {
    error_at_current(
        std::format(
            "expected number or string on RHS of predicate, got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "use a number (e.g. 50), a quoted string (e.g. \"thermal\"), "
        "or a bare identifier for a literal");
  }

  return pred;
}

// ── F5: abs(linear_expr) ───────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_abs_expr()
{
  // Current token is 'abs'
  const auto abs_col = m_current_.start_pos;
  advance();
  expect(TokenType::LPAREN, "use `abs(<linear_expr>)`");

  auto inner = parse_add_expr();

  expect(TokenType::RPAREN, "close `abs(` with ')'");

  // Reject nested abs(abs(...)).
  for (const auto& t : inner) {
    if (t.abs_expr) {
      error_at(abs_col,
               "nested abs(abs(...)) is not allowed",
               "rewrite to remove the inner abs, or introduce a "
               "helper constraint for the inner magnitude");
    }
  }

  // Constant-folding: abs(pure constant) → |sum|
  if (const auto c = extract_constant(inner)) {
    return {
        ConstraintTerm {
            .coefficient = std::abs(*c),
        },
    };
  }

  ConstraintTerm term;
  term.coefficient = 1.0;
  term.abs_expr =
      std::make_shared<const AbsExpr>(AbsExpr {.inner = std::move(inner)});
  return {
      std::move(term),
  };
}

// ── Phase 1e: state(element_ref) ───────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_state_expr()
{
  // Current token is `state`
  const auto state_col = m_current_.start_pos;
  advance();
  expect(TokenType::LPAREN, "use `state(<element_ref>)`");

  // The argument must be a single element reference: `type("id").attr`.
  // Reject nested `state(state(...))`, sums, numbers, parens, and any
  // other compound construct — Phase 1e is intentionally narrow.
  if (m_current_.type != TokenType::IDENT || !is_element_type(m_current_.value))
  {
    error_at(state_col,
             std::format(
                 "state(...) expects a single element reference, "
                 "got '{}'",
                 m_current_.value.empty() ? "end of input" : m_current_.value),
             "use `state(<element>(\"id\").attr)`, e.g. "
             "state(reservoir(\"R1\").efin)");
  }
  auto type_name = std::move(m_current_.value);
  auto ref = parse_element_ref(std::move(type_name));
  ref.state_wrapped = true;

  expect(TokenType::RPAREN, "close `state(` with ')'");

  return {
      ConstraintTerm {
          .coefficient = 1.0,
          .element = std::move(ref),
      },
  };
}

// ── F7: min/max(arg1, arg2, ...) ───────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_minmax_expr(
    MinMaxKind kind)
{
  // Current token is 'min' or 'max'
  const auto mm_col = m_current_.start_pos;
  advance();
  expect(TokenType::LPAREN,
         kind == MinMaxKind::Max ? "use `max(arg1, arg2, ...)`"
                                 : "use `min(arg1, arg2, ...)`");

  std::vector<std::vector<ConstraintTerm>> args;
  args.push_back(parse_add_expr());
  while (m_current_.type == TokenType::COMMA) {
    advance();
    args.push_back(parse_add_expr());
  }

  expect(TokenType::RPAREN,
         kind == MinMaxKind::Max ? "close `max(` with ')'"
                                 : "close `min(` with ')'");

  if (args.size() < 2) {
    error_at(mm_col,
             kind == MinMaxKind::Max
                 ? "max(...) requires at least two arguments"
                 : "min(...) requires at least two arguments",
             "use `max(a, b[, c, ...])` or `min(a, b[, c, ...])`");
  }

  // If every argument is a pure constant, fold at parse time.
  bool all_const = true;
  std::vector<double> const_vals;
  const_vals.reserve(args.size());
  for (const auto& a : args) {
    if (const auto c = extract_constant(a)) {
      const_vals.push_back(*c);
    } else {
      all_const = false;
      break;
    }
  }
  if (all_const) {
    const double folded = kind == MinMaxKind::Max
        ? *std::ranges::max_element(const_vals)
        : *std::ranges::min_element(const_vals);
    return {
        ConstraintTerm {
            .coefficient = folded,
        },
    };
  }

  ConstraintTerm term;
  term.coefficient = 1.0;
  term.minmax_expr = std::make_shared<const MinMaxExpr>(MinMaxExpr {
      .kind = kind,
      .args = std::move(args),
  });
  return {
      std::move(term),
  };
}

// ── F8: if <cond> then (<expr>) [ else (<expr>) ] ──────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<ConstraintTerm> ConstraintParser::Parser::parse_if_expr()
{
  // Current token is 'if'
  advance();

  auto cond = parse_if_cond();

  if (m_current_.type != TokenType::IDENT || m_current_.value != "then") {
    error_at_current(std::format("expected 'then' after if-condition, got '{}'",
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     "use `if <cond> then (<expr>) [ else (<expr>) ]`");
  }
  advance();

  expect(TokenType::LPAREN, "wrap the `then` branch in parentheses");
  auto then_branch = parse_add_expr();
  expect(TokenType::RPAREN, "close the `then` branch with ')'");

  std::vector<ConstraintTerm> else_branch;
  if (m_current_.type == TokenType::IDENT && m_current_.value == "else") {
    advance();
    expect(TokenType::LPAREN, "wrap the `else` branch in parentheses");
    else_branch = parse_add_expr();
    expect(TokenType::RPAREN, "close the `else` branch with ')'");
  }

  ConstraintTerm term;
  term.coefficient = 1.0;
  term.if_expr = std::make_shared<const IfExpr>(IfExpr {
      .cond = std::move(cond),
      .then_branch = std::move(then_branch),
      .else_branch = std::move(else_branch),
  });
  return {
      std::move(term),
  };
}

std::vector<IfCondAtom> ConstraintParser::Parser::parse_if_cond()
{
  std::vector<IfCondAtom> atoms;
  atoms.push_back(parse_if_cond_atom());
  while (m_current_.type == TokenType::IDENT
         && (m_current_.value == "and" || m_current_.value == "&&"))
  {
    advance();
    atoms.push_back(parse_if_cond_atom());
  }
  return atoms;
}

// Parse an integer literal from a NUMBER token into a `uid_t`
// (the shared backing type of all strong uids: StageUid, ScenarioUid,
// BlockUid).  Uses `std::from_chars` so no locale / exceptions / allocs,
// and emits a parser-friendly diagnostic on failure or overflow.
uid_t ConstraintParser::Parser::parse_if_cond_integer(const std::string& s)
{
  uid_t val = 0;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (ec != std::errc {}) {
    error_at_current(
        std::format("invalid integer literal '{}' in if-condition", s),
        "if-condition RHS must fit in a 32-bit signed integer");
  }
  return val;
}

IfCondAtom ConstraintParser::Parser::parse_if_cond_atom()
{
  IfCondAtom atom;

  if (m_current_.type != TokenType::IDENT) {
    error_at_current(
        std::format(
            "expected coordinate name (scenario/stage/block), got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "if-conditions compare loop coordinates to integer literals");
  }

  const auto coord_col = m_current_.start_pos;
  const std::string coord_name = std::move(m_current_.value);
  if (coord_name == "scenario") {
    atom.coord = IfCondAtom::Coord::Scenario;
  } else if (coord_name == "stage") {
    atom.coord = IfCondAtom::Coord::Stage;
  } else if (coord_name == "block") {
    atom.coord = IfCondAtom::Coord::Block;
  } else {
    error_at(coord_col,
             std::format("unknown if-condition coordinate '{}'", coord_name),
             "use 'scenario', 'stage', or 'block'");
  }
  advance();

  // Operator
  if (m_current_.type == TokenType::EQ) {
    atom.op = IfCondAtom::Op::Eq;
    advance();
  } else if (m_current_.type == TokenType::NE) {
    atom.op = IfCondAtom::Op::Ne;
    advance();
  } else if (m_current_.type == TokenType::LT) {
    atom.op = IfCondAtom::Op::Lt;
    advance();
  } else if (m_current_.type == TokenType::LEQ) {
    atom.op = IfCondAtom::Op::Le;
    advance();
  } else if (m_current_.type == TokenType::GT) {
    atom.op = IfCondAtom::Op::Gt;
    advance();
  } else if (m_current_.type == TokenType::GEQ) {
    atom.op = IfCondAtom::Op::Ge;
    advance();
  } else if (m_current_.type == TokenType::IDENT && m_current_.value == "in") {
    atom.op = IfCondAtom::Op::In;
    advance();
    expect(TokenType::LBRACE, "use `in { n1, n2, n1..nN }`");
    while (true) {
      if (m_current_.type != TokenType::NUMBER) {
        error_at_current(
            std::format(
                "expected integer in condition set, got '{}'",
                m_current_.value.empty() ? "end of input" : m_current_.value),
            "if-condition sets are integer literals or ranges");
      }
      const uid_t start = parse_if_cond_integer(m_current_.value);
      advance();
      if (m_current_.type == TokenType::DOTDOT) {
        advance();
        if (m_current_.type != TokenType::NUMBER) {
          error_at_current(
              std::format(
                  "expected integer after '..', got '{}'",
                  m_current_.value.empty() ? "end of input" : m_current_.value),
              "range form is `start..end`");
        }
        const uid_t end = parse_if_cond_integer(m_current_.value);
        advance();
        for (uid_t i = start; i <= end; ++i) {
          atom.set_values.push_back(i);
        }
      } else {
        atom.set_values.push_back(start);
      }
      if (m_current_.type != TokenType::COMMA) {
        break;
      }
      advance();
    }
    expect(TokenType::RBRACE, "close the set with '}'");
    return atom;
  } else {
    error_at_current(std::format("expected comparison operator, got '{}'",
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     "valid ops: = == != <> < <= > >= in");
  }

  // Integer literal on RHS (scalar compare)
  if (m_current_.type != TokenType::NUMBER) {
    error_at_current(
        std::format(
            "expected integer literal in if-condition, got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "RHS of an if-condition must be an integer literal");
  }
  atom.number = parse_if_cond_integer(m_current_.value);
  advance();
  return atom;
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
    error_at_current(
        std::format(
            "expected element name (quoted string) or numeric UID, "
            "got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "use 'name' for named refs or bare integer UIDs, e.g. generator(3)");
  }

  expect(TokenType::RPAREN, "close the element id with ')'");
  expect(TokenType::DOT, "follow the element with '.attribute'");

  if (m_current_.type != TokenType::IDENT) {
    error_at_current(std::format("expected attribute name after '.', got '{}'",
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     "attribute is an identifier like 'generation' or 'load'");
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
      error_at_current(
          std::format(
              "expected dimension name in for clause, got '{}'",
              m_current_.value.empty() ? "end of input" : m_current_.value),
          "dimensions are 'scenario', 'stage', or 'block'");
    }

    const auto dim_col = m_current_.start_pos;
    std::string dim = std::move(m_current_.value);
    advance();

    // Accept 'in' or '='
    if ((m_current_.type == TokenType::IDENT && m_current_.value == "in")
        || m_current_.type == TokenType::EQ)
    {
      advance();
    } else {
      error_at_current(
          std::format(
              "expected 'in' or '=' after dimension '{}', got '{}'",
              dim,
              m_current_.value.empty() ? "end of input" : m_current_.value),
          "use `dim in {1,2,3}` or `dim = 3`");
    }

    auto index_set = parse_index_set();

    if (dim == "scenario") {
      domain.scenarios = std::move(index_set);
    } else if (dim == "stage") {
      domain.stages = std::move(index_set);
    } else if (dim == "block") {
      domain.blocks = std::move(index_set);
    } else {
      error_at(dim_col,
               std::format("unknown dimension '{}' in for clause", dim),
               "use 'scenario', 'stage', or 'block'");
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
        error_at_current(
            std::format(
                "expected number in index set, got '{}'",
                m_current_.value.empty() ? "end of input" : m_current_.value),
            "use integer indices like `{1, 2, 5..8}`");
      }
      const int start = std::stoi(m_current_.value);
      advance();

      if (m_current_.type == TokenType::DOTDOT) {
        // Range: start..end
        advance();
        if (m_current_.type != TokenType::NUMBER) {
          error_at_current(
              std::format(
                  "expected number after '..', got '{}'",
                  m_current_.value.empty() ? "end of input" : m_current_.value),
              "range requires upper bound, e.g. `1..10`");
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

  error_at_current(
      std::format("expected index set (number, range, or 'all'), got '{}'",
                  m_current_.value.empty() ? "end of input" : m_current_.value),
      "valid forms: `all`, `N`, `N..M`, `{N, M..P}`");
}

// ── Public API ──────────────────────────────────────────────────────────────

ConstraintExpr ConstraintParser::parse(std::string_view name,
                                       std::string_view expression)
{
  if (expression.empty()) {
    throw ConstraintParseError("Parse error: empty constraint expression",
                               "empty constraint expression",
                               "provide at least one term and a comparison",
                               0);
  }

  // Strip comments before tokenizing
  const std::string cleaned = strip_comments(expression);

  const Lexer lexer(cleaned);
  Parser parser(lexer, cleaned);
  auto result = parser.parse_constraint();
  result.name = std::string {name};
  // Compound attributes (e.g. `line.flow` → `+flowp - flown`) are now
  // expanded lazily at row-assembly time via the decentralized AMPL
  // registry — see `resolve_col_to_row` in element_column_resolver.cpp.
  return result;
}

ConstraintExpr ConstraintParser::parse(std::string_view expression)
{
  return parse("", expression);
}

}  // namespace gtopt
