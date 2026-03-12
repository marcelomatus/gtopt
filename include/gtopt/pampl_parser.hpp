/**
 * @file      pampl_parser.hpp
 * @brief     Parser for pseudo-AMPL (.pampl) user constraint files
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * This module provides `PamplParser::parse_file()`, which reads a `.pampl`
 * file and converts it to a vector of `UserConstraint` objects ready to be
 * merged into a `System::user_constraint_array`.
 *
 * ### PAMPL file format
 *
 * A `.pampl` file contains a sequence of constraint definitions, each
 * terminated by a semicolon.  Blank lines and `#` / `//` comments are
 * ignored everywhere.
 *
 * **Formal grammar (pseudo-BNF):**
 * ```
 * pampl_file      := constraint_stmt*
 *
 * constraint_stmt := constraint_hdr? constraint_expr ';'
 *
 * constraint_hdr  := ['inactive'] 'constraint' IDENT [STRING] ':'
 *
 * constraint_expr := <same syntax as UserConstraint::expression>
 *                    e.g. generator('G1').generation + ... <= 300,
 *                         for(stage in {1,2})
 *
 * STRING          := '"' ... '"'  |  '\'' ... '\''
 * IDENT           := [A-Za-z_][A-Za-z0-9_]*
 * ```
 *
 * ### Examples
 *
 * ```pampl
 * # System peak capacity limit
 * constraint gen_pair_limit "Combined generation limit for G1 and G2":
 *   generator('G1').generation + generator('G2').generation <= 300,
 *   for(stage in {1,2,3}, block in 1..24);
 *
 * # Inactive during tuning
 * inactive constraint flow_check:
 *   line('L1').flow <= 200;
 *
 * # No header — uid is auto-assigned
 * sum(generator(all).generation) <= 1000;
 * ```
 *
 * ### UID assignment
 *
 * If the file is merged into a system that already has constraints with UIDs
 * 1–N, pass `start_uid = N + 1` so there are no collisions.  When no header
 * is present the name is auto-generated as `"uc_<uid>"`.
 */

#pragma once

#include <string_view>
#include <vector>

#include <gtopt/user_constraint.hpp>

namespace gtopt
{

/**
 * @brief Parser for pseudo-AMPL constraint files (.pampl)
 *
 * Reads a `.pampl` text file (or string) and produces a list of
 * `UserConstraint` objects.  Call `parse_file()` to parse from a filesystem
 * path, or `parse()` to parse from an in-memory string.
 */
class PamplParser
{
public:
  /**
   * @brief Parse a `.pampl` file into `UserConstraint` objects.
   *
   * @param filepath   Path to the `.pampl` file.
   * @param start_uid  First UID to assign (default 1); increment by 1 for
   *                   each constraint so UIDs are unique.
   * @return Vector of parsed `UserConstraint` objects.
   * @throws std::runtime_error  if the file cannot be opened.
   * @throws std::invalid_argument  on syntax errors.
   */
  [[nodiscard]] static std::vector<UserConstraint> parse_file(
      std::string_view filepath, Uid start_uid = Uid {1});

  /**
   * @brief Parse a `.pampl` string into `UserConstraint` objects.
   *
   * @param source    The PAMPL source text.
   * @param start_uid First UID to assign (default 1).
   * @return Vector of parsed `UserConstraint` objects.
   * @throws std::invalid_argument  on syntax errors.
   */
  [[nodiscard]] static std::vector<UserConstraint> parse(
      std::string_view source, Uid start_uid = Uid {1});
};

}  // namespace gtopt
