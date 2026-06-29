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

#include <gtopt/block_lp.hpp>
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
    case '[':
      ++m_pos_;
      return Token {
          .type = TokenType::LBRACKET,
          .value = "[",
          .start_pos = start,
      };
    case ']':
      ++m_pos_;
      return Token {
          .type = TokenType::RBRACKET,
          .value = "]",
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
    case TokenType::LBRACKET:
      return "'['";
    case TokenType::RBRACKET:
      return "']'";
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
      || name == "flow_right" || name == "volume_right"
      || name == "lng_terminal" || name == "fuel" || name == "emission_zone"
      || name == "emission_source" || name == "commitment"
      || name == "simple_commitment" || name == "decision_variable";
}

bool ConstraintParser::Parser::is_singleton_class(const std::string& name)
{
  // Singleton classes expose globally-scoped read-only scalars and are
  // parsed as `class.attribute` (no parens, no element id).  The set of
  // legal scalars is enforced by the resolver against the allow-list
  // registered in `system_lp.cpp::register_all_ampl_element_names`,
  // except for `stage.*` whose values come from the active StageLP at
  // resolution time (calendar metadata of the stage being assembled).
  // `block.*` likewise reads the active BlockLP (currently `block.duration`),
  // and additionally folds into a term's `duration_weighted` Δt factor when
  // it multiplies a variable (see `multiply_terms`).
  return name == "options" || name == "system" || name == "stage"
      || name == "block";
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
          || t.if_expr || t.time_agg || t.coeff_profile.has_value();
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
          || t.if_expr || t.time_agg || t.coeff_profile.has_value();
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

  // A per-block coefficient profile must always be attached to a
  // variable-bearing term (via `[...] * variable`).  A profile that
  // survived parsing without a variable (e.g. a bare `[1,2,3] <= 0`)
  // would silently contribute nothing at LP assembly — reject it here.
  for (const auto& t : result.terms) {
    if (t.coeff_profile
        && !(t.element || t.sum_ref || t.param_name || t.abs_expr
             || t.minmax_expr || t.if_expr || t.time_agg))
    {
      error_at_current(
          "a per-block coefficient profile must multiply a variable term",
          "write `[1,2,3] * generator(\"G1\").generation`, not a bare "
          "profile");
    }
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
/// return nullopt if any term references a variable, parameter,
/// nonlinear wrapper (`abs`, `min`/`max`, `if`), or carries a per-block
/// coefficient profile (a profile is not a plain scalar constant).
std::optional<double> extract_constant(
    const std::vector<ConstraintTerm>& terms) noexcept
{
  double acc = 0.0;
  for (const auto& t : terms) {
    if (t.element || t.sum_ref || t.param_name || t.abs_expr || t.minmax_expr
        || t.if_expr || t.time_agg || t.coeff_profile)
    {
      return std::nullopt;
    }
    acc += t.coefficient;
  }
  return acc;
}

/// If @p terms is exactly one pure-profile term (a per-block coefficient
/// profile with no element/sum/param/wrapper), return its profile values;
/// otherwise nullopt.  Used by `multiply_terms` to detect the
/// `[v0, v1, ...] * variable` form.  A pure-profile term's scalar
/// `coefficient` is always 1.0 (any scaling is folded into the profile
/// entries by `scale_terms`), so the profile alone is authoritative.
std::optional<std::vector<double>> extract_profile(
    const std::vector<ConstraintTerm>& terms)
{
  if (terms.size() != 1) {
    return std::nullopt;
  }
  const auto& t = terms.front();
  if (t.element || t.sum_ref || t.param_name || t.abs_expr || t.minmax_expr
      || t.if_expr || t.time_agg || !t.coeff_profile)
  {
    return std::nullopt;
  }
  // `t.coeff_profile` is the same `std::optional<std::vector<double>>` as
  // the return type and is guaranteed engaged here — return it directly
  // (a `*`-dereference + re-wrap would be flagged as an error-prone
  // optional round-trip).
  return t.coeff_profile;
}

void scale_terms(std::vector<ConstraintTerm>& terms, double k) noexcept
{
  for (auto& t : terms) {
    if (t.coeff_profile) {
      // Profile terms keep `coefficient == 1.0` and fold the scalar into
      // every profile entry so the profile remains authoritative (and
      // `extract_profile` need not re-apply `coefficient`).
      for (auto& v : *t.coeff_profile) {
        v *= k;
      }
    } else {
      t.coefficient *= k;
    }
  }
}

/// If `ts` is a single bare `block.duration` singleton scalar (no profile,
/// no prior Δt weight, no aggregate/wrapper), return its coefficient; else
/// nullopt.  Used by `multiply_terms` to recognise the one param×variable
/// product that is allowed — `block.duration * <var>` — and fold it into a
/// per-block Δt weight on the variable term.
[[nodiscard]] std::optional<double> as_block_duration_coef(
    const std::vector<ConstraintTerm>& ts)
{
  if (ts.size() != 1) {
    return std::nullopt;
  }
  const auto& t = ts.front();
  if (t.element && t.element->element_id.empty()
      && t.element->element_type == BlockLP::ClassName
      && t.element->attribute == BlockLP::DurationName && !t.coeff_profile
      && !t.duration_weighted && !t.sum_ref && !t.param_name && !t.abs_expr
      && !t.minmax_expr && !t.if_expr && !t.time_agg)
  {
    return t.coefficient;
  }
  return std::nullopt;
}

/// Multiply two linear expressions.  At least one side must be a pure
/// constant (or a per-block coefficient profile, which then attaches to
/// the variable term on the other side); any product of two
/// variable-bearing expressions is non-linear and rejected at parse time.
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

  // Per-block coefficient profile `[...]` on one side (F9).  Two cases:
  //   - constant × profile  → scaled profile (still a pure profile),
  //   - profile  × variable → profile attaches to the single variable
  //                           term.
  // A profile times a sum of several terms, or two profiles, is rejected
  // (the profile-to-term mapping would be ambiguous or non-linear).
  auto l_prof = extract_profile(lhs);
  auto r_prof = extract_profile(rhs);
  if (l_prof || r_prof) {
    if (l_prof && r_prof) {
      throw std::invalid_argument(
          "Non-linear product: both sides of '*' are per-block coefficient "
          "profiles; a profile must multiply a single variable term");
    }
    std::vector<double> profile =
        l_prof ? std::move(*l_prof) : std::move(*r_prof);
    // constant × profile → scale the profile, keep it a pure-profile term.
    if (const auto other_const = l_prof ? r_const : l_const) {
      for (auto& v : profile) {
        v *= *other_const;
      }
      return {
          ConstraintTerm {
              .coeff_profile = std::move(profile),
          },
      };
    }
    // profile × (single variable term) → attach the profile to it.
    auto& var_terms = l_prof ? rhs : lhs;
    if (var_terms.size() != 1) {
      throw std::invalid_argument(
          "A per-block coefficient profile must multiply a single variable "
          "term (e.g. `[1,2,3] * generator(\"G1\").generation`); it cannot "
          "multiply a sum of terms or another profile");
    }
    auto& vt = var_terms.front();
    if (!(vt.element || vt.sum_ref || vt.param_name || vt.abs_expr
          || vt.minmax_expr || vt.if_expr))
    {
      throw std::invalid_argument(
          "A per-block coefficient profile must multiply a variable, sum, "
          "or parameter term, not a bare constant");
    }
    // Fold the variable term's scalar coefficient into the profile so the
    // profile carries the full per-block multiplier.
    for (auto& v : profile) {
      v *= vt.coefficient;
    }
    vt.coefficient = 1.0;
    vt.coeff_profile = std::move(profile);
    return std::move(var_terms);
  }

  if (r_const) {
    scale_terms(lhs, *r_const);
    return lhs;
  }
  if (l_const) {
    scale_terms(rhs, *l_const);
    return rhs;
  }

  // `block.duration` × (variable expression): the singleton scalar
  // `block.duration` is resolved per-block at row-assembly, so it cannot be
  // folded into a numeric coefficient at parse time the way a constant is.
  // Instead, record a per-block Δt weight on the variable terms — turning a
  // rate (power/flow) into the energy/volume it contributes over the block,
  // exactly as the native StorageLP balance scales flow columns by
  // `block.duration()`.  This is the ONLY param×variable product allowed.
  const auto l_dur = as_block_duration_coef(lhs);
  const auto r_dur = as_block_duration_coef(rhs);
  if (l_dur && r_dur) {
    throw std::invalid_argument(
        "Non-linear product: `block.duration * block.duration` is not a "
        "linear term");
  }
  if (l_dur || r_dur) {
    // `var_terms` is a reference to the by-value parameter (`lhs`/`rhs`) that
    // is NOT `block.duration`; valid through the `std::move(var_terms)` return.
    auto& var_terms = l_dur ? rhs : lhs;
    const double k = l_dur ? *l_dur : *r_dur;
    for (auto& t : var_terms) {
      if (t.duration_weighted) {
        throw std::invalid_argument(
            "`block.duration` applied twice to the same term");
      }
      t.coefficient *= k;
      t.duration_weighted = true;
    }
    return std::move(var_terms);
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

std::vector<ConstraintTerm> ConstraintParser::Parser::parse_primary()
{
  // Capture the column of the term's anchor token BEFORE any
  // advance() consumes it.  Stored 1-BASED so ``column == 0`` stays
  // free as the "unset" sentinel — see ``ConstraintTerm::column``
  // docstring.  Every ConstraintTerm we construct from here on gets
  // ``term.column = anchor_col`` so resolver-time diagnostics
  // ("unknown generator 'GX'") can report a ``at column N`` source
  // location instead of just the constraint name — task #55, the P1
  // safety win.
  const auto anchor_col = m_current_.start_pos + 1;

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
        .column = anchor_col,
    };
    advance();
    return {
        std::move(t),
    };
  }

  // Per-block coefficient profile `[v0, v1, ...]` (F9).  Stands in for a
  // leading scalar multiplier of a single variable-bearing term.  Parsed
  // here as a pure-profile term (no element/sum/param/wrapper); the
  // multiplicative fold in `multiply_terms` attaches it to the variable
  // term on the other side of the `*`.  A profile may not be combined
  // (added/multiplied) with another profile — that is rejected at fold
  // time.
  if (m_current_.type == TokenType::LBRACKET) {
    return parse_coeff_profile();
  }

  // `sum` — TWO distinct operators selected by the following bracket:
  //   sum{ idx in window } ...   → TIME aggregation  (LBRACE) → TimeAggRef
  //   sum( type(ids).attr )      → ELEMENT aggregation (LPAREN) → SumElementRef
  // See constraint_expr.hpp's grammar block for the full distinction.
  if (m_current_.type == TokenType::IDENT && m_current_.value == "sum") {
    if (m_lexer_.peek().type == TokenType::LBRACE) {
      return parse_time_agg_expr();
    }
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

  // prev(element_ref) — lagged (previous-block) reference; at the first
  // block of a stage it resolves to the cross-phase incoming column.
  if (m_current_.type == TokenType::IDENT && m_current_.value == "prev") {
    return parse_prev_expr();
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
              .column = anchor_col,
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
              .column = anchor_col,
          },
      };
    }
    // Bare identifier → named parameter reference
    ConstraintTerm t {
        .coefficient = 1.0,
        .param_name = std::move(m_current_.value),
        .column = anchor_col,
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

std::vector<ConstraintTerm> ConstraintParser::Parser::parse_coeff_profile()
{
  // Current token is '['.  Parse a comma-separated list of (optionally
  // signed) numeric literals, with an optional trailing comma, e.g.
  // `[1, 2, 3]` or `[1.5, -0.5,]`.
  const auto open_col = m_current_.start_pos;
  advance();  // consume '['

  std::vector<double> profile;
  while (m_current_.type != TokenType::RBRACKET) {
    double sign = 1.0;
    // Allow a unary +/- in front of each profile entry.
    while (m_current_.type == TokenType::PLUS
           || m_current_.type == TokenType::MINUS)
    {
      if (m_current_.type == TokenType::MINUS) {
        sign = -sign;
      }
      advance();
    }
    if (m_current_.type != TokenType::NUMBER) {
      error_at_current(
          std::format(
              "expected number in coefficient profile, got '{}'",
              m_current_.value.empty() ? "end of input" : m_current_.value),
          "a profile is a bracketed list of numbers, e.g. [1, 2, 3]");
    }
    profile.push_back(sign * std::stod(m_current_.value));
    advance();
    if (m_current_.type == TokenType::COMMA) {
      advance();  // consume ',' (trailing comma tolerated)
    } else if (m_current_.type != TokenType::RBRACKET) {
      error_at_current(
          std::format(
              "expected ',' or ']' in coefficient profile, got '{}'",
              m_current_.value.empty() ? "end of input" : m_current_.value),
          "separate profile entries with ',' and close with ']'");
    }
  }
  advance();  // consume ']'

  if (profile.empty()) {
    error_at(open_col,
             "empty coefficient profile '[]'",
             "a profile must list at least one number, e.g. [1, 2, 3]");
  }

  // Encode as a pure-profile term: no element/sum/param/wrapper, but
  // `coeff_profile` set.  `coefficient` is left at its default 1.0 so the
  // scalar path (when this profile is *not* multiplied by a variable, an
  // error caught at fold time) stays well-defined.
  return {
      ConstraintTerm {
          .coeff_profile = std::move(profile),
      },
  };
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

std::vector<ConstraintTerm> ConstraintParser::Parser::parse_time_agg_expr()
{
  // Current token is `sum`; the next is `{` (checked by the caller).
  //   sum{ idx in window } [dur[idx] *] inner_expr
  const auto sum_col = m_current_.start_pos;
  advance();  // consume `sum`
  expect(TokenType::LBRACE, "use `sum{ idx in stage|day } ...`");

  // Bound index name (e.g. `b`).
  if (m_current_.type != TokenType::IDENT) {
    error_at_current(
        std::format(
            "expected an index name after `sum{{`, got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "use `sum{ b in stage } ...`");
  }
  TimeAggRef agg;
  agg.index_name = std::move(m_current_.value);
  advance();

  // `in`
  if (m_current_.type != TokenType::IDENT || m_current_.value != "in") {
    error_at_current(std::format("expected `in` after the index name, got '{}'",
                                 m_current_.value.empty() ? "end of input"
                                                          : m_current_.value),
                     "use `sum{ b in stage } ...`");
  }
  advance();

  // Time window: `stage` | `day`.
  if (m_current_.type != TokenType::IDENT) {
    error_at_current(
        std::format(
            "expected a time window (`stage` or `day`), got '{}'",
            m_current_.value.empty() ? "end of input" : m_current_.value),
        "valid windows: stage, day");
  }
  if (m_current_.value == "stage") {
    agg.window = TimeWindow::Stage;
  } else if (m_current_.value == "day") {
    agg.window = TimeWindow::Day;
  } else {
    error_at_current(std::format("unknown time window '{}'", m_current_.value),
                     "valid windows: stage, day");
  }
  advance();

  expect(TokenType::RBRACE, "close the index set with '}'");

  // Optional `dur[idx] *` prefix → duration (Δt) weighting (energy form).
  // Required for RATE variables (flow [m³/s]) so the time-sum is energy.
  if (m_current_.type == TokenType::IDENT && m_current_.value == "dur") {
    advance();
    expect(TokenType::LBRACKET, "use `dur[<index>] * ...`");
    if (m_current_.type != TokenType::IDENT) {
      error_at_current(
          std::format(
              "expected the bound index inside `dur[...]`, got '{}'",
              m_current_.value.empty() ? "end of input" : m_current_.value),
          std::format("use `dur[{}]`", agg.index_name));
    }
    if (m_current_.value != agg.index_name) {
      error_at_current(
          std::format("`dur[{}]` does not match the bound index `{}`",
                      m_current_.value,
                      agg.index_name),
          std::format("use `dur[{}]`", agg.index_name));
    }
    advance();
    expect(TokenType::RBRACKET, "close `dur[` with ']'");
    expect(TokenType::STAR, "`dur[idx]` must multiply the inner expression");
    agg.weight = TimeAggWeight::Duration;
  }

  // Inner linear expression — aggregated over the window's blocks.
  agg.inner = parse_mul_expr();
  if (agg.inner.empty()) {
    error_at(sum_col,
             "empty inner expression in `sum{...}`",
             "supply a linear expression, e.g. `sum{b in stage} "
             "generator('g').generation`");
  }
  // A time-agg over a pure constant carries no LP columns — reject so the
  // author doesn't silently get a constant masquerading as an aggregation.
  if (extract_constant(agg.inner)) {
    error_at(sum_col,
             "`sum{...}` over a constant has no variables to aggregate",
             "the inner expression must reference a per-block variable");
  }

  ConstraintTerm term;
  term.coefficient = 1.0;
  term.time_agg =
      std::make_shared<const TimeAggRef>(TimeAggRef {std::move(agg)});
  term.column = sum_col + 1;
  return {
      std::move(term),
  };
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
    const auto& s = m_current_.value;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
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

// ── prev(element_ref): lagged (previous-block) reference ───────────────────

std::vector<ConstraintTerm> ConstraintParser::Parser::parse_prev_expr()
{
  // Current token is `prev`.  Mirrors parse_state_expr: a single element
  // reference, no nesting.  The resolver lowers it to the previous block's
  // column within the stage (or, at the first block, the cross-phase
  // incoming column).
  const auto prev_col = m_current_.start_pos;
  advance();
  expect(TokenType::LPAREN, "use `prev(<element_ref>)`");

  if (m_current_.type != TokenType::IDENT || !is_element_type(m_current_.value))
  {
    error_at(prev_col,
             std::format(
                 "prev(...) expects a single element reference, "
                 "got '{}'",
                 m_current_.value.empty() ? "end of input" : m_current_.value),
             "use `prev(<element>(\"id\").attr)`, e.g. "
             "prev(decision_variable(\"vol\").value)");
  }
  auto type_name = std::move(m_current_.value);
  auto ref = parse_element_ref(std::move(type_name));
  ref.prev_wrapped = true;

  expect(TokenType::RPAREN, "close `prev(` with ')'");

  return {
      ConstraintTerm {
          .coefficient = 1.0,
          .element = std::move(ref),
      },
  };
}

// ── F7: min/max(arg1, arg2, ...) ───────────────────────────────────────────

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
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
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
        if (end >= start) {
          range.values.reserve(range.values.size()
                               + static_cast<size_t>(end - start) + 1);
          for (int i = start; i <= end; ++i) {
            range.values.push_back(i);
          }
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
      if (end >= start) {
        range.values.reserve(static_cast<size_t>(end - start) + 1);
        for (int i = start; i <= end; ++i) {
          range.values.push_back(i);
        }
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
