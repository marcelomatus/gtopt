/**
 * @file      constraint_expr.hpp
 * @brief     AST types for parsed user constraint expressions
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * This module defines the Abstract Syntax Tree (AST) types used to represent
 * parsed user constraint expressions. The AST is produced by the
 * ConstraintParser and consumed by the LP construction layer to add
 * user-defined constraints to the LinearProblem.
 *
 * ### Supported element types and attributes
 *
 * | Element type | Attributes                                    |
 * |-------------|-----------------------------------------------|
 * | generator   | generation, cost                              |
 * | demand      | load, fail                                    |
 * | line        | flow, flowp, flown, lossp, lossn              |
 * | battery     | energy, charge, discharge                     |
 * | converter   | charge, discharge                             |
 * | reservoir   | volume                                        |
 * | bus         | theta                                         |
 * | waterway    | flow                                          |
 * | turbine     | generation                                    |
 *
 * ### Grammar (pseudo-BNF)
 *
 * ```
 * constraint   := expr comp_op expr [',' for_clause]
 *              |  number comp_op expr comp_op number [',' for_clause]
 *
 * expr         := term (('+' | '-') term)*
 *
 * term         := [number '*'] element_ref
 *              |  [number '*'] sum_expr
 *              |  ['-'] number
 *
 * element_ref  := element_type '(' string ')' '.' attribute
 *
 * sum_expr     := 'sum' '(' element_type '(' string_list ')' '.'
 *                 attribute ')'
 *              |  'sum' '(' element_type '(' 'all' [',''type''=''string'] ')'
 * '.' attribute ')'
 *
 * string_list  := string (',' string)*
 *
 * element_type := 'generator' | 'demand' | 'line' | 'battery'
 *              |  'converter' | 'reservoir' | 'bus'
 *              |  'waterway'  | 'turbine'
 *
 * attribute    := IDENT
 *
 * comp_op      := '<=' | '>=' | '='
 *
 * for_clause   := 'for' '(' index_spec (',' index_spec)* ')'
 *
 * index_spec   := index_dim ('in' | '=') index_set
 *
 * index_dim    := 'scenario' | 'stage' | 'block'
 *
 * index_set    := 'all'
 *              |  '{' index_values '}'
 *              |  number '..' number
 *              |  number
 *
 * index_values := index_value (',' index_value)*
 *
 * index_value  := number
 *              |  number '..' number
 *
 * comment      := '#' <any>* EOL
 *              |  '//' <any>* EOL
 * ```
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/linear_parser.hpp>

namespace gtopt
{

/**
 * @brief Reference to a gtopt element and one of its LP attributes
 *
 * Examples:
 *   - generator("TORO").generation  → {element_type="generator",
 *                                       element_id="TORO",
 *                                       attribute="generation"}
 *   - demand("uid:5").load          → {element_type="demand",
 *                                       element_id="uid:5",
 *                                       attribute="load"}
 */
struct ElementRef
{
  std::string element_type {};  ///< "generator", "demand", "line", etc.
  std::string element_id {};  ///< name or "uid:N" reference
  std::string attribute {};  ///< LP attribute: "generation", "load", etc.
};

/**
 * @brief Aggregation over multiple elements of the same type
 *
 * Represents `sum(element_type("id1","id2",...).attribute)` or
 * `sum(element_type(all).attribute)` — the AMPL-style `sum{g in SET}`.
 * An optional `type_filter` restricts the sum to elements whose `type` field
 * matches the given string (case-sensitive).
 *
 * Examples:
 *   - sum(generator("G1","G2").generation)
 *       → {element_type="generator", element_ids={"G1","G2"},
 *          all_elements=false, attribute="generation"}
 *   - sum(generator(all).generation)
 *       → {element_type="generator", element_ids={},
 *          all_elements=true, attribute="generation"}
 *   - sum(generator(all, type="hydro").generation)
 *       → {element_type="generator", element_ids={},
 *          all_elements=true, type_filter="hydro", attribute="generation"}
 */
struct SumElementRef
{
  std::string element_type {};  ///< "generator", "demand", "line", etc.
  std::vector<std::string> element_ids {};  ///< Element names/UIDs to sum
  bool all_elements {false};  ///< true = sum over all elements of the type
  std::optional<std::string>
      type_filter {};  ///< Optional type tag filter (matches element.type)
  std::string attribute {};  ///< LP attribute to aggregate
};

/**
 * @brief Specifies a set of index values for a time dimension
 *
 * Three forms:
 *   - is_all = true: matches every index in the dimension
 *   - values non-empty: explicit list of indices (e.g., {1,3,5})
 *   - Empty values + !is_all: invalid (default-constructed)
 */
struct IndexRange
{
  bool is_all {true};  ///< If true, matches every index
  std::vector<int> values {};  ///< Explicit index values (1-based)
};

/**
 * @brief Domain over which a constraint is instantiated
 *
 * Each dimension defaults to "all" when not explicitly specified.
 */
struct ConstraintDomain
{
  IndexRange scenarios {};  ///< Scenario indices (default: all)
  IndexRange stages {};  ///< Stage indices (default: all)
  IndexRange blocks {};  ///< Block indices (default: all)
};

/**
 * @brief A single term in a constraint expression
 *
 * Either:
 *   - coefficient × element reference (single variable term)
 *   - coefficient × sum reference (aggregation term)
 *   - standalone coefficient (constant term, both nullopt)
 */
struct ConstraintTerm
{
  double coefficient {1.0};  ///< Scalar multiplier
  std::optional<ElementRef> element {};  ///< Single element (nullopt = none)
  std::optional<SumElementRef>
      sum_ref {};  ///< Sum aggregation (nullopt = none)
};

/**
 * @brief Complete parsed constraint expression
 *
 * Represents a constraint of one of these forms:
 *   - lhs_terms ≤ rhs
 *   - lhs_terms ≥ rhs
 *   - lhs_terms = rhs
 *   - lower_bound ≤ lhs_terms ≤ upper_bound (RANGE)
 *
 * plus an optional domain restriction.
 */
struct ConstraintExpr
{
  std::string name {};  ///< Constraint name (from UserConstraint::name)
  std::vector<ConstraintTerm> terms {};  ///< Variable and constant terms (LHS)
  ConstraintType constraint_type {};  ///< Comparison operator
  double rhs {0.0};  ///< Right-hand side (non-range constraints)
  std::optional<double> lower_bound {};  ///< Lower bound (RANGE only)
  std::optional<double> upper_bound {};  ///< Upper bound (RANGE only)
  ConstraintDomain domain {};  ///< Index domain specification
};

}  // namespace gtopt
