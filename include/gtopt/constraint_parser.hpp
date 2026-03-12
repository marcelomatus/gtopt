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
 * auto expr = ConstraintParser::parse(
 *     "gen_limit",
 *     R"(generator("TORO").generation + generator("uid:23").generation <= 300,
 *        for(stage in {4,5,6}, block in 1..30))");
 *
 * // expr.terms[0].element->element_id == "TORO"
 * // expr.terms[1].element->element_id == "uid:23"
 * // expr.rhs == 300.0
 * // expr.domain.stages.values == {4, 5, 6}
 * // expr.domain.blocks.values == {1, 2, ..., 30}
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
 * expression string into a ConstraintExpr AST.
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
    void skip_whitespace() noexcept;
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
    explicit Parser(Lexer lexer) noexcept;
    [[nodiscard]] ConstraintExpr parse_constraint();

  private:
    void advance();
    void expect(TokenType type);
    [[nodiscard]] bool match(TokenType type);
    [[nodiscard]] static bool is_element_type(const std::string& name);

    [[nodiscard]] std::vector<ConstraintTerm> parse_expr();
    [[nodiscard]] ConstraintTerm parse_term(bool negate);
    [[nodiscard]] ElementRef parse_element_ref(std::string type_name);
    [[nodiscard]] ConstraintDomain parse_for_clause();
    [[nodiscard]] IndexRange parse_index_set();

    Lexer m_lexer_;
    Token m_current_;
  };
};

}  // namespace gtopt
