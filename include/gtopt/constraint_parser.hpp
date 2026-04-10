/**
 * @file      constraint_parser.hpp
 * @brief     Parser for user constraint expressions (AMPL-inspired syntax)
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * This module provides a recursive-descent parser that converts AMPL-inspired
 * constraint expression strings into ConstraintExpr AST objects.
 *
 * ### Example usage
 *
 * ```cpp
 * // Single element references
 * auto expr = ConstraintParser::parse(
 *     "gen_limit",
 *     R"(generator("TORO").generation + generator("uid:23").generation <= 300,
 *        for(stage in {4,5,6}, block in 1..30))");
 *
 * // AMPL-style sum aggregation
 * auto sum_expr = ConstraintParser::parse(
 *     "total_gen",
 *     R"(sum(generator("G1","G2","G3").generation) <= 500)");
 *
 * // Sum over all elements of a type
 * auto all_expr = ConstraintParser::parse(
 *     "system_cap",
 *     R"(sum(generator(all).generation) <= 1000)");
 * ```
 */

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <gtopt/constraint_expr.hpp>

namespace gtopt
{

/**
 * @brief Parser-time diagnostic error.
 *
 * Inherits from `std::invalid_argument` so existing `CHECK_THROWS_AS`
 * fixtures keep working.  In addition to the formatted message (which
 * includes a caret pointer and an optional hint), exposes the raw
 * column position and the one-line hint for programmatic inspection.
 */
class ConstraintParseError : public std::invalid_argument
{
public:
  ConstraintParseError(const std::string& formatted,
                       std::string message,
                       std::string hint,
                       std::size_t column) noexcept
      : std::invalid_argument(formatted)
      , m_message_(std::move(message))
      , m_hint_(std::move(hint))
      , m_column_(column)
  {
  }

  [[nodiscard]] const std::string& message() const noexcept
  {
    return m_message_;
  }
  [[nodiscard]] const std::string& hint() const noexcept { return m_hint_; }
  [[nodiscard]] std::size_t column() const noexcept { return m_column_; }

private:
  std::string m_message_;
  std::string m_hint_;
  std::size_t m_column_ {0};
};

/**
 * @brief Parser for AMPL-inspired user constraint expressions
 *
 * Provides static parse() methods that tokenize and parse a constraint
 * expression string into a ConstraintExpr AST.  Supports comments
 * (`#` or `//` to end-of-line), element references, sum aggregation,
 * and domain clauses.
 */
class ConstraintParser
{
public:
  /**
   * @brief Parse a constraint expression with an optional name
   * @param name Human-readable constraint name
   * @param expression Constraint expression string
   * @return Parsed ConstraintExpr AST
   * @throws std::invalid_argument on syntax errors
   */
  [[nodiscard]] static ConstraintExpr parse(std::string_view name,
                                            std::string_view expression);

  /**
   * @brief Parse a constraint expression (unnamed)
   * @param expression Constraint expression string
   * @return Parsed ConstraintExpr AST
   * @throws std::invalid_argument on syntax errors
   */
  [[nodiscard]] static ConstraintExpr parse(std::string_view expression);

private:
  // ── Token types and lexer ─────────────────────────────────────────────

  enum class TokenType : std::uint8_t
  {
    END,
    NUMBER,
    STRING,
    IDENT,
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    DOT,
    STAR,
    SLASH,
    PLUS,
    MINUS,
    LEQ,
    GEQ,
    EQ,  ///< `=` or `==`
    LT,  ///< `<`
    GT,  ///< `>`
    NE,  ///< `!=` or `<>`
    COMMA,
    COLON,  ///< `:` — separates sum id-list from predicate filters
    DOTDOT,
  };

  struct Token
  {
    TokenType type {TokenType::END};
    std::string value {};
    std::size_t start_pos {0};  // byte offset into the source buffer
  };

  class Lexer
  {
  public:
    explicit Lexer(std::string_view input) noexcept;
    [[nodiscard]] Token next();
    [[nodiscard]] Token peek();
    [[nodiscard]] std::string_view source() const noexcept { return m_input_; }
    [[nodiscard]] std::size_t position() const noexcept { return m_pos_; }

  private:
    void skip_whitespace_and_comments() noexcept;
    [[nodiscard]] Token scan_number();
    [[nodiscard]] Token scan_string();
    [[nodiscard]] Token scan_ident();

    std::string_view m_input_;
    std::size_t m_pos_ {0};
  };

  // ── Recursive-descent parser ──────────────────────────────────────────

  class Parser
  {
  public:
    Parser(Lexer lexer, std::string_view raw_source);
    [[nodiscard]] ConstraintExpr parse_constraint();

  private:
    void advance();
    void expect(TokenType type);
    void expect(TokenType type, std::string_view hint);
    [[nodiscard]] bool match(TokenType type);
    [[nodiscard]] static bool is_element_type(const std::string& name);
    [[nodiscard]] static bool is_singleton_class(const std::string& name);

    [[noreturn]] void error_at(std::size_t column,
                               const std::string& message,
                               std::string_view hint = {}) const;
    [[noreturn]] void error_at_current(const std::string& message,
                                       std::string_view hint = {}) const;

    [[nodiscard]] static std::string_view token_type_name(
        TokenType type) noexcept;

    [[nodiscard]] std::vector<ConstraintTerm> parse_add_expr();
    [[nodiscard]] std::vector<ConstraintTerm> parse_mul_expr();
    [[nodiscard]] std::vector<ConstraintTerm> parse_unary();
    [[nodiscard]] std::vector<ConstraintTerm> parse_primary();
    [[nodiscard]] ElementRef parse_element_ref(std::string type_name);
    [[nodiscard]] ConstraintTerm parse_sum_expr();
    /// Parse `abs(linear_expr)` (F5).  Called when the current token is
    /// the `abs` identifier.  Rejects nested `abs(abs(...))`.
    [[nodiscard]] std::vector<ConstraintTerm> parse_abs_expr();
    /// Parse `state(element_ref)` (Phase 1e).  Called when the current
    /// token is the `state` identifier.  The argument must be a single
    /// element reference (no sums, no nesting).  Sets `state_wrapped`
    /// on the resulting `ElementRef`.
    [[nodiscard]] std::vector<ConstraintTerm> parse_state_expr();
    /// Parse `max(arg1, arg2, ...)` or `min(arg1, arg2, ...)` (F7).
    /// Called when the current token is `max` or `min`.
    [[nodiscard]] std::vector<ConstraintTerm> parse_minmax_expr(
        MinMaxKind kind);
    /// Parse `if <cond> then (<expr>) [ else (<expr>) ]` (F8).
    /// Called when the current token is `if`.
    [[nodiscard]] std::vector<ConstraintTerm> parse_if_expr();
    /// Parse the condition inside an `if` expression (conjunction of
    /// atoms on loop coordinates).
    [[nodiscard]] std::vector<IfCondAtom> parse_if_cond();
    /// Parse a single `<coord> op <value>` or `<coord> in { … }` atom.
    [[nodiscard]] IfCondAtom parse_if_cond_atom();
    /// Parse an integer literal from a NUMBER token into a `uid_t`
    /// (the signed 32-bit backing type of all strong uids).
    [[nodiscard]] uid_t parse_if_cond_integer(const std::string& s);
    /// Parse a conjunction of predicates inside `sum(...: pred and pred)`.
    /// Fills `sum_ref.filters` and advances past the predicate list.
    void parse_sum_predicates(SumElementRef& sum_ref);
    /// Parse one predicate `attr op value` inside a sum filter.
    [[nodiscard]] SumPredicate parse_one_predicate();
    [[nodiscard]] ConstraintDomain parse_for_clause();
    [[nodiscard]] IndexRange parse_index_set();

    Lexer m_lexer_;
    Token m_current_;
    std::string_view m_raw_source_;  // original expression for diagnostics
  };

  /// @brief Strip comments from expression before parsing
  [[nodiscard]] static std::string strip_comments(std::string_view input);
};

}  // namespace gtopt
