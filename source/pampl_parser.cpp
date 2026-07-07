/**
 * @file      pampl_parser.cpp
 * @brief     Implementation of the pseudo-AMPL (.pampl) constraint file parser
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/pampl_parser.hpp>

namespace gtopt
{

namespace
{

// ── Character helpers ────────────────────────────────────────────────────────

bool is_ident_start(char c)
{
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_cont(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// ── Low-level scanner ────────────────────────────────────────────────────────

/**
 * @brief Minimal scanner for PAMPL header tokens only.
 *
 * The scanner is only responsible for tokenizing the optional header
 * (`[inactive] constraint NAME ["desc"] :`).  The constraint expression
 * itself is extracted as a raw substring (everything up to the next `;`) and
 * passed verbatim to ConstraintParser.
 */
class Scanner
{
public:
  explicit Scanner(std::string_view src) noexcept
      : m_src_(src)
  {
  }

  [[nodiscard]] std::size_t pos() const noexcept { return m_pos_; }
  [[nodiscard]] bool at_end() const noexcept { return m_pos_ >= m_src_.size(); }

  /// Skip whitespace and `#` / `//` line comments.
  void skip_ws_comments()
  {
    while (!at_end()) {
      const char c = m_src_[m_pos_];
      if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        ++m_pos_;
        continue;
      }
      // Line comment: # ...
      if (c == '#') {
        while (!at_end() && m_src_[m_pos_] != '\n') {
          ++m_pos_;
        }
        continue;
      }
      // Line comment: // ...
      if (c == '/' && m_pos_ + 1 < m_src_.size() && m_src_[m_pos_ + 1] == '/') {
        while (!at_end() && m_src_[m_pos_] != '\n') {
          ++m_pos_;
        }
        continue;
      }
      break;
    }
  }

  /// Peek at the next non-ws character without advancing.
  [[nodiscard]] char peek_char()
  {
    skip_ws_comments();
    return at_end() ? '\0' : m_src_[m_pos_];
  }

  /// Read an identifier; returns empty if current position is not ident_start.
  [[nodiscard]] std::string read_ident()
  {
    skip_ws_comments();
    if (at_end() || !is_ident_start(m_src_[m_pos_])) {
      return {};
    }
    const std::size_t start = m_pos_;
    while (!at_end() && is_ident_cont(m_src_[m_pos_])) {
      ++m_pos_;
    }
    return std::string {m_src_.substr(start, m_pos_ - start)};
  }

  /// Read a quoted string literal (`"..."` or `'...'`).  Returns the
  /// unquoted content, or empty if the next token is not a string.
  [[nodiscard]] std::string read_string()
  {
    skip_ws_comments();
    if (at_end()) {
      return {};
    }
    const char quote = m_src_[m_pos_];
    if (quote != '"' && quote != '\'') {
      return {};
    }
    ++m_pos_;  // skip opening quote
    std::string result;
    while (!at_end() && m_src_[m_pos_] != quote) {
      if (m_src_[m_pos_] == '\\' && m_pos_ + 1 < m_src_.size()) {
        ++m_pos_;  // skip backslash
      }
      result += m_src_[m_pos_++];
    }
    if (at_end()) {
      throw std::invalid_argument("PAMPL: unterminated string literal");
    }
    ++m_pos_;  // skip closing quote
    return result;
  }

  /// Consume a specific character; return true if it matched.
  bool consume(char expected)
  {
    skip_ws_comments();
    if (!at_end() && m_src_[m_pos_] == expected) {
      ++m_pos_;
      return true;
    }
    return false;
  }

  /// Read a numeric literal (integer or decimal, possibly negative).
  /// Skips leading whitespace/comments, then reads [-]digits[.digits].
  [[nodiscard]] double read_number()
  {
    skip_ws_comments();
    bool negative = false;
    if (!at_end() && m_src_[m_pos_] == '-') {
      negative = true;
      ++m_pos_;
      skip_ws_comments();
    }
    const std::size_t start = m_pos_;
    while (!at_end()
           && (std::isdigit(static_cast<unsigned char>(m_src_[m_pos_])) != 0
               || m_src_[m_pos_] == '.'))
    {
      ++m_pos_;
    }
    if (m_pos_ == start) {
      throw std::invalid_argument("PAMPL: expected number");
    }
    const double val =
        std::stod(std::string {m_src_.substr(start, m_pos_ - start)});
    return negative ? -val : val;
  }

  /// Return the rest of the source from current position up to (not
  /// including) the next `;`, advancing past the `;`.  Throws if no `;`
  /// is found before end of input.
  [[nodiscard]] std::string read_until_semicolon()
  {
    const std::size_t start = m_pos_;
    while (!at_end() && m_src_[m_pos_] != ';') {
      ++m_pos_;
    }
    if (at_end()) {
      throw std::invalid_argument(
          "PAMPL: missing ';' at end of constraint expression");
    }
    std::string expr {m_src_.substr(start, m_pos_ - start)};
    ++m_pos_;  // consume ';'
    return expr;
  }

private:
  std::string_view m_src_;
  std::size_t m_pos_ {0};
};

// ── Param value parsing ──────────────────────────────────────────────────────

// ── Scalar arithmetic evaluator for param values ────────────────────────────
//
// Lets a ``param`` value be a small arithmetic expression rather than a bare
// literal, so derived constants document their own derivation, e.g.
//   param fuel_cap_penalty = 1000 / 24 / 7;   # daily-budget per-hour rate
// Grammar (standard precedence, left-associative):
//   expr   := term (('+' | '-') term)*
//   term   := factor (('*' | '/') factor)*
//   factor := NUMBER | IDENT | '(' expr ')'
// ``read_number`` already consumes a leading sign and scientific notation,
// so ``1e-5`` is a single literal while ``a - b`` is a subtraction.  An
// ``IDENT`` factor resolves to the scalar value of a param declared EARLIER
// in the file (``param a = 1800; param b = a / 7;``).
double resolve_scalar_param(const std::vector<UserParam>& params,
                            std::string_view ref)
{
  const auto it = std::ranges::find_if(
      params, [&ref](const UserParam& p) { return p.name == ref; });
  if (it != params.end()) {
    if (const auto& value = it->value) {
      return *value;
    }
  }
  throw std::invalid_argument(
      std::format("PAMPL: value references unknown or non-scalar "
                  "param '{}'",
                  ref));
}

double parse_value_expr(Scanner& sc, const std::vector<UserParam>& params);

double parse_value_factor(Scanner& sc, const std::vector<UserParam>& params)
{
  sc.skip_ws_comments();
  const char c = sc.peek_char();
  // Unary sign on a factor (covers ``-(a/4)`` and ``-ident`` where
  // ``read_number`` — which only eats ``-<digits>`` — cannot).
  if (c == '-') {
    sc.consume('-');
    return -parse_value_factor(sc, params);
  }
  if (c == '+') {
    sc.consume('+');
    return parse_value_factor(sc, params);
  }
  if (c == '(') {
    sc.consume('(');
    const double inner = parse_value_expr(sc, params);
    if (!sc.consume(')')) {
      throw std::invalid_argument(
          "PAMPL: expected ')' to close param value sub-expression");
    }
    return inner;
  }
  if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
    return resolve_scalar_param(params, sc.read_ident());
  }
  return sc.read_number();
}

double parse_value_term(Scanner& sc, const std::vector<UserParam>& params)
{
  double acc = parse_value_factor(sc, params);
  while (true) {
    sc.skip_ws_comments();
    const char op = sc.peek_char();
    if (op == '*') {
      sc.consume('*');
      acc *= parse_value_factor(sc, params);
    } else if (op == '/') {
      sc.consume('/');
      acc /= parse_value_factor(sc, params);
    } else {
      break;
    }
  }
  return acc;
}

double parse_value_expr(Scanner& sc, const std::vector<UserParam>& params)
{
  double acc = parse_value_term(sc, params);
  while (true) {
    sc.skip_ws_comments();
    const char op = sc.peek_char();
    if (op == '+') {
      sc.consume('+');
      acc += parse_value_term(sc, params);
    } else if (op == '-') {
      sc.consume('-');
      acc -= parse_value_term(sc, params);
    } else {
      break;
    }
  }
  return acc;
}

/// Parse `param name = value;` or `param name[month] = [v1, ..., v12];`.
/// ``params`` holds the params declared earlier in the file so a value
/// expression may reference them (``param b = a / 7;``).
UserParam parse_param(Scanner& sc, const std::vector<UserParam>& params)
{
  UserParam param;

  param.name = sc.read_ident();
  if (param.name.empty()) {
    throw std::invalid_argument(
        "PAMPL: expected identifier (param name) after 'param'");
  }

  // Check for [month] indexing
  bool is_monthly = false;
  sc.skip_ws_comments();
  if (sc.peek_char() == '[') {
    sc.consume('[');
    const std::string index_dim = sc.read_ident();
    if (index_dim != "month") {
      throw std::invalid_argument(std::format(
          "PAMPL: unsupported index dimension '{}' (only 'month' is "
          "supported)",
          index_dim));
    }
    if (!sc.consume(']')) {
      throw std::invalid_argument(
          "PAMPL: expected ']' after 'month' in param declaration");
    }
    is_monthly = true;
  }

  // Expect '='
  if (!sc.consume('=')) {
    throw std::invalid_argument(
        std::format("PAMPL: expected '=' after param name '{}'", param.name));
  }

  if (is_monthly) {
    // Parse [v1, v2, ..., v12]
    sc.skip_ws_comments();
    if (!sc.consume('[')) {
      throw std::invalid_argument(std::format(
          "PAMPL: expected '[' for monthly values of param '{}'", param.name));
    }

    std::vector<Real> values;
    while (true) {
      sc.skip_ws_comments();
      if (sc.peek_char() == ']') {
        sc.consume(']');
        break;
      }
      values.push_back(parse_value_expr(sc, params));
      sc.skip_ws_comments();
      sc.consume(',');  // optional trailing comma
    }

    if (values.size() != 12) {
      throw std::invalid_argument(
          std::format("PAMPL: param '{}' has {} monthly values (expected 12)",
                      param.name,
                      values.size()));
    }
    param.monthly = std::move(values);
  } else {
    // Scalar value — arithmetic expression over literals + earlier params
    // (e.g. ``1000 / 24 / 7`` or ``a / 7``).
    param.value = parse_value_expr(sc, params);
  }

  // Expect ';'
  sc.skip_ws_comments();
  if (!sc.consume(';')) {
    throw std::invalid_argument(std::format(
        "PAMPL: expected ';' after param '{}' declaration", param.name));
  }

  return param;
}

// ── PAMPL parse logic ────────────────────────────────────────────────────────

PamplParseResult do_parse(std::string_view source, Uid start_uid)
{
  Scanner sc(source);
  PamplParseResult result;
  // Hot-lookup mirror of ``result.declared_vars`` so the constraint
  // emitter can find a matching ``slack_<NAME>`` declaration in O(1).
  // Kept local: the public surface stays the ordered vector on
  // PamplParseResult, which preserves declaration order for downstream
  // audit / dump consumers.
  std::unordered_set<std::string> declared_vars_set;

  Uid next_uid = start_uid;

  while (true) {
    sc.skip_ws_comments();
    if (sc.at_end()) {
      break;
    }

    // ── Optional header: [inactive] constraint NAME ["desc"] : ──────────────
    //    Or: param NAME [= value | [month] = [...]] ;
    //    Or: var NAME [, NAME]* ;
    bool active = true;
    std::string name;
    std::string description;
    std::optional<double> penalty;
    std::optional<std::vector<double>> rhs_sched;
    bool has_header = false;

    // Save position so we can rewind if "inactive"/"constraint" are not found
    const std::size_t saved_pos = sc.pos();

    // Try to read a header keyword
    const std::string first_word = sc.read_ident();

    if (first_word == "param") {
      // Parameter declaration
      result.params.push_back(parse_param(sc, result.params));
      continue;
    }

    if (first_word == "var") {
      // AMPL-style free-variable declaration:
      //   var <ident> [, <ident>]* ;
      // The declared names are captured in ``result.declared_vars`` so a
      // downstream audit can enumerate them; by naming convention
      // ``var slack_<NAME>;`` also seeds ``UserConstraint::slack_name``
      // for the matching constraint at the end of this loop, giving the
      // auto-created soft-slack column a user-controlled LP-internal
      // label.  No LP variable is added by the declaration itself —
      // only soft constraints (``penalty > 0``) produce slack columns,
      // and they do so unconditionally via the
      // ``UserConstraintLP::add_to_lp`` slack-folding path.
      bool saw_ident = false;
      while (true) {
        sc.skip_ws_comments();
        if (sc.at_end() || sc.peek_char() == ';') {
          break;
        }
        const std::string ident = sc.read_ident();
        if (ident.empty()) {
          throw std::invalid_argument("PAMPL: expected identifier after 'var'");
        }
        saw_ident = true;
        if (declared_vars_set.insert(ident).second) {
          result.declared_vars.push_back(ident);
        }
        sc.skip_ws_comments();
        if (sc.peek_char() == ',') {
          sc.consume(',');
          continue;
        }
        break;
      }
      if (!saw_ident) {
        throw std::invalid_argument(
            "PAMPL: 'var' declaration requires at least one identifier");
      }
      sc.skip_ws_comments();
      if (!sc.consume(';')) {
        throw std::invalid_argument(
            "PAMPL: expected ';' to terminate 'var' declaration");
      }
      continue;
    }

    if (first_word == "inactive") {
      // Must be followed by "constraint"
      const std::string second_word = sc.read_ident();
      if (second_word != "constraint") {
        throw std::invalid_argument(
            std::format("PAMPL: expected 'constraint' after 'inactive', "
                        "got '{}'",
                        second_word));
      }
      active = false;
      has_header = true;
    } else if (first_word == "constraint") {
      has_header = true;
    } else {
      // Not a header – rewind and treat as bare expression
      // (The Scanner has no rewind method, but we can reconstruct the
      // expression by re-reading from saved_pos.)
      // Re-create scanner from saved position:
      sc = Scanner {source.substr(saved_pos)};
      // (next_uid and result are still valid)
    }

    if (has_header) {
      // Read constraint name (mandatory after 'constraint')
      name = sc.read_ident();
      if (name.empty()) {
        throw std::invalid_argument(
            "PAMPL: expected identifier (constraint name) after 'constraint'");
      }

      // Optional description string
      sc.skip_ws_comments();
      if (!sc.at_end() && (sc.peek_char() == '"' || sc.peek_char() == '\'')) {
        description = sc.read_string();
      }

      // Optional header clauses between the name (or description) and the
      // mandatory colon.  Two clauses are recognised, in any order:
      //
      //   penalty <value-expr>
      //       Soft constraint with a per-unit slack cost (mirrors
      //       UserConstraint.penalty).  Absent ⇒ hard constraint.  The value
      //       is the same arithmetic-expression grammar used for param
      //       values, so it may be a literal, a param reference, or
      //       arithmetic over both (``penalty soft_floor_penalty``,
      //       ``penalty 1000/24/7``).
      //
      //   rhs [v0, v1, ..., vK]
      //       Per-block (scheduled) RHS override (mirrors UserConstraint.rhs
      //       in its TB-matrix form ``[[v0, ..., vK]]``).  Each element is a
      //       param-value expression so derived/named constants work.  When
      //       present, the scalar RHS parsed from the inline ``<op> NUMBER``
      //       tail of the expression is the per-block fallback for blocks the
      //       schedule does not cover.  Example:
      //         constraint ramp_cap rhs [40, 40, 60]: ... <= 0;
      //
      //   constraint NAME ["desc"] [penalty 500] [rhs [...]] :
      while (true) {
        sc.skip_ws_comments();
        if (sc.at_end() || sc.peek_char() == ':') {
          break;
        }
        const std::string kw = sc.read_ident();
        if (kw == "penalty") {
          if (penalty.has_value()) {
            throw std::invalid_argument(std::format(
                "PAMPL: duplicate 'penalty' clause on constraint '{}'", name));
          }
          penalty = parse_value_expr(sc, result.params);
        } else if (kw == "rhs") {
          if (rhs_sched.has_value()) {
            throw std::invalid_argument(std::format(
                "PAMPL: duplicate 'rhs' clause on constraint '{}'", name));
          }
          sc.skip_ws_comments();
          if (!sc.consume('[')) {
            throw std::invalid_argument(std::format(
                "PAMPL: expected '[' after 'rhs' on constraint '{}'", name));
          }
          std::vector<double> values;
          while (true) {
            sc.skip_ws_comments();
            if (sc.peek_char() == ']') {
              sc.consume(']');
              break;
            }
            values.push_back(parse_value_expr(sc, result.params));
            sc.skip_ws_comments();
            sc.consume(',');  // optional trailing comma
          }
          if (values.empty()) {
            throw std::invalid_argument(std::format(
                "PAMPL: 'rhs' clause on constraint '{}' has no values", name));
          }
          rhs_sched = std::move(values);
        } else {
          throw std::invalid_argument(std::format(
              "PAMPL: expected 'penalty', 'rhs' or ':' after constraint "
              "name '{}', got '{}'",
              name,
              kw));
        }
      }

      // Mandatory colon
      if (!sc.consume(':')) {
        throw std::invalid_argument(std::format(
            "PAMPL: expected ':' after constraint name '{}'", name));
      }
    }

    // ── Expression up to ';' ─────────────────────────────────────────────────
    std::string expr = sc.read_until_semicolon();

    // Trim leading/trailing whitespace from expression
    const auto expr_start = expr.find_first_not_of(" \t\r\n");
    const auto expr_end = expr.find_last_not_of(" \t\r\n");
    if (expr_start == std::string::npos) {
      // Empty expression (e.g., a bare ';') — skip silently
      continue;
    }
    expr = expr.substr(expr_start, expr_end - expr_start + 1);

    // Auto-generate name if none was provided
    if (name.empty()) {
      name = as_label("uc", next_uid);
    }

    UserConstraint uc;
    uc.uid = next_uid++;
    uc.name = std::move(name);
    uc.expression = std::move(expr);
    if (!active) {
      uc.active = false;
    }
    uc.penalty = penalty;  // unset ⇒ hard; set ⇒ soft slack cost
    if (!description.empty()) {
      uc.description = std::move(description);
    }
    // A scheduled RHS maps onto UserConstraint.rhs as a single-row TB matrix
    // ``[[v0, ..., vK]]`` — the exact shape the JSON path produces — so the
    // same OptTBRealFieldSched / per-block override machinery in
    // UserConstraintLP applies with no parallel code path.
    if (rhs_sched.has_value()) {
      uc.rhs = std::vector<std::vector<double>> {std::move(*rhs_sched)};
    }

    // Slack-name binding by naming convention: when a top-level
    // ``var slack_<NAME>;`` declaration was seen, populate
    // ``uc.slack_name`` so the LP-internal slack column inherits the
    // user-chosen label (CPLEX debug / LP dump readability).  The
    // output schema is unchanged — slack values still land in the
    // aggregated ``UserConstraint/slack_sol.parquet`` keyed by uid.
    if (!declared_vars_set.empty()) {
      const std::string slack_candidate = std::string {"slack_"} + uc.name;
      if (declared_vars_set.contains(slack_candidate)) {
        uc.slack_name = slack_candidate;
      }
    }

    result.constraints.push_back(std::move(uc));
  }

  return result;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

PamplParseResult PamplParser::parse_file(std::string_view filepath,
                                         Uid start_uid)
{
  const std::ifstream file {std::string {filepath}};
  if (!file) {
    throw std::runtime_error(
        std::format("PAMPL: cannot open file '{}'", filepath));
  }
  std::ostringstream buf;
  buf << file.rdbuf();
  return parse(buf.str(), start_uid);
}

PamplParseResult PamplParser::parse(std::string_view source, Uid start_uid)
{
  return do_parse(source, start_uid);
}

}  // namespace gtopt
