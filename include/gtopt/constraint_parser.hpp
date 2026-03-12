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

#include <string>
#include <string_view>

#include <gtopt/constraint_expr.hpp>

namespace gtopt
{

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
    PLUS,
    MINUS,
    LEQ,
    GEQ,
    EQ,
    COMMA,
    DOTDOT,
  };

  struct Token
  {
    TokenType type {TokenType::END};
    std::string value {};
  };

  class Lexer
  {
  public:
    explicit Lexer(std::string_view input) noexcept;
    [[nodiscard]] Token next();
    [[nodiscard]] Token peek();

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
    explicit Parser(Lexer lexer);
    [[nodiscard]] ConstraintExpr parse_constraint();

  private:
    void advance();
    void expect(TokenType type);
    [[nodiscard]] bool match(TokenType type);
    [[nodiscard]] static bool is_element_type(const std::string& name);

    [[nodiscard]] std::vector<ConstraintTerm> parse_expr();
    [[nodiscard]] ConstraintTerm parse_term(bool negate);
    [[nodiscard]] ElementRef parse_element_ref(std::string type_name);
    [[nodiscard]] ConstraintTerm parse_sum_expr(double sign);
    [[nodiscard]] ConstraintDomain parse_for_clause();
    [[nodiscard]] IndexRange parse_index_set();

    Lexer m_lexer_;
    Token m_current_;
  };

  /// @brief Strip comments from expression before parsing
  [[nodiscard]] static std::string strip_comments(std::string_view input);
};

}  // namespace gtopt
