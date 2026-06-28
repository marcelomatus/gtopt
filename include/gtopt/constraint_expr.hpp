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
 * | Element type       | Attributes                                          |
 * |-------------------|-----------------------------------------------------|
 * | generator          | generation, capainst (alias: capacity)              |
 * | demand             | load, fail, capainst (alias: capacity)              |
 * | line               | flow, flowp, flown, lossp, lossn,                   |
 * |                    | capainst (alias: capacity)                          |
 * | battery            | energy, charge, discharge, spill (alias: drain),    |
 * |                    | eini, efin, soft_emin,                              |
 * |                    | capainst (alias: capacity)                          |
 * | converter          | flow (= +discharge − charge), charge, discharge     |
 * | reservoir          | volume (alias: energy), extraction,                  |
 * |                    | spill (alias: drain),                               |
 * |                    | eini, efin, soft_emin                               |
 * | bus                | theta (alias: angle)                                |
 * | waterway           | flow                                                |
 * | turbine            | generation                                          |
 * | junction           | drain                                               |
 * | flow               | flow (alias: discharge)                             |
 * | flow_right         | flow, fail                                          |
 * | volume_right       | extraction (aliases: flow, fout), saving             |
 * | seepage            | flow (alias: seepage)                               |
 * | reserve_provision  | up (aliases: uprovision, up_provision),             |
 * |                    | dn (aliases: dprovision, dn_provision, down)        |
 * | reserve_zone       | up (aliases: urequirement, up_requirement),         |
 * |                    | dn (aliases: drequirement, dn_requirement,          |
 * |                    |     down)                                           |
 * | lng_terminal       | energy (tank volume), delivery,                     |
 * |                    | spill (alias: drain), eini, efin, soft_emin         |
 *
 * ### Variable scaling
 *
 * Some LP variables are internally scaled for numerical conditioning.
 * User constraints are written in **physical units**; the resolver
 * automatically applies the correct scale factor to each coefficient so
 * that the LP constraint is dimensionally correct.
 *
 * The resolver uses `LinearProblem::get_col_scale()` to read the scale
 * factor that was stored at variable-creation time, so all current and
 * future scaled variables are handled uniformly.
 *
 * | Variable             | Scale (physical = LP × scale)                  |
 * |---------------------|------------------------------------------------|
 * | reservoir.volume     | energy_scale: auto-computed from capacity      |
 * |                      | (capacity/1000 rounded to next power of 10),   |
 * |                      | or set explicitly via `energy_scale` JSON field |
 * | reservoir.extraction | flow_scale (default 1.0; overridable via       |
 * |                      | `variable_scales` option)                      |
 * | reservoir.spill      | flow_scale (same)                              |
 * | reservoir.eini       | energy_scale (same as reservoir.volume)        |
 * | reservoir.efin       | energy_scale (same as reservoir.volume)        |
 * | reservoir.soft_emin  | energy_scale (same as reservoir.volume)        |
 * | battery.energy       | energy_scale (default 1.0)                     |
 * | battery.charge       | flow_scale   (default 1.0)                     |
 * | battery.discharge    | flow_scale   (default 1.0)                     |
 * | battery.spill        | flow_scale   (default 1.0)                     |
 * | battery.eini         | energy_scale (default 1.0)                     |
 * | battery.efin         | energy_scale (default 1.0)                     |
 * | battery.soft_emin    | energy_scale (default 1.0)                     |
 * | bus.theta            | 1 / scale_theta (default 1/1000)               |
 * | all others           | 1.0 (no scaling)                               |
 *
 * ### Stage-level vs block-level attributes
 *
 * Most attributes are block-level variables (one value per block per stage).
 * Some are stage-level (one value per stage), returned for every block:
 *
 * | Stage-level attributes                                        |
 * |---------------------------------------------------------------|
 * | generator.capainst, demand.capainst, line.capainst            |
 * | battery.capainst, battery.eini, battery.efin, battery.soft_emin |
 * | reservoir.eini, reservoir.efin, reservoir.soft_emin           |
 *
 * Stage-level attributes may appear in any per-block constraint expression;
 * the same LP column is referenced across all blocks in the stage.
 *
 * ### Grammar (pseudo-BNF)
 *
 * ```text
 * constraint   := expr comp_op expr [',' for_clause]
 *              |  number comp_op expr comp_op number [',' for_clause]
 *
 * expr         := term (('+' | '-') term)*
 *
 * term         := [coeff '*'] element_ref
 *              |  [coeff '*'] state_ref
 *              |  [coeff '*'] sum_expr        // ELEMENT aggregation, see below
 *              |  [coeff '*'] time_agg_expr   // TIME aggregation, see below
 *              |  ['-'] number
 *
 * coeff        := number
 *              |  coeff_profile          // F9 per-block coefficient
 *
 * coeff_profile := '[' number (',' number)* [','] ']'
 *              // A bracketed list of per-block coefficients standing in
 *              // for the leading scalar multiplier of a single
 *              // variable-bearing term.  Block ordinal `b` (0-based,
 *              // within the stage's block list) selects entry
 *              // `profile[min(b, size-1)]`; a short profile broadcasts
 *              // its last value.  Example:
 *              //     [1, 2, 3] * generator("G1").generation <= 100
 *              // Mirrors the per-block `rhs [v0, v1, ...]` schedule on
 *              // the LHS.  May appear at most once per term and only as
 *              // the term's leading multiplier.
 *
 * element_ref  := element_type '(' string ')' '.' attribute
 *
 * state_ref    := 'state' '(' element_ref ')'
 *
 * sum_expr     := 'sum' '(' element_type '(' string_list ')' '.'
 *                 attribute ')'
 *              |  'sum' '(' element_type '(' 'all' [ ',' 'type' '=' string ]
 * ')'
 *                 '.' attribute ')'
 *
 * ─── TWO `sum` SPELLINGS (read carefully — they are DIFFERENT operators) ───
 *
 *   ELEMENT aggregation:  sum( type(ids).attr )           — uses PARENTHESES
 *     Sums one attribute over a SET OF ELEMENTS of one type, all evaluated
 *     at the SAME ambient (scenario, stage, block).  AST: `SumElementRef`.
 *     Example:  sum(generator(all).generation) <= 300
 *
 *   TIME aggregation:     sum{ idx in window } inner_expr  — uses BRACES
 *     Sums an inner linear expression over TIME (the blocks of the named
 *     window), letting a coarse-scope (`stage`/`phase`/`global`) row reach
 *     per-block variables.  AST: `TimeAggRef`.  Over RATE variables (flow
 *     [m³/s]) the energy form MUST carry the per-block duration weight —
 *     spell it `sum{b in stage} dur[b] * waterway('w').flow` (the `dur[b]`
 *     prefix sets `weight = duration`).  `daily_sum` is the special case
 *     `sum{b in day}` (count) / `sum{b in day} dur[b]*…` (energy).
 *
 * time_agg_expr := 'sum' '{' IDENT 'in' time_window '}'
 *                  [ 'dur' '[' IDENT ']' '*' ] inner_expr
 *
 * time_window  := 'stage' | 'day'
 *
 * string_list  := string (',' string)*
 *
 * element_type := 'generator' | 'demand' | 'line' | 'battery'
 *              |  'converter' | 'reservoir' | 'bus'
 *              |  'waterway'  | 'turbine'
 *              |  'junction'  | 'flow' | 'seepage'
 *              |  'flow_right' | 'volume_right'
 *              |  'reserve_provision' | 'reserve_zone'
 *              |  'lng_terminal'
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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>
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

  /// True when the user wrote `state(<element_ref>)` (Phase 1e).
  /// In Phase 1 the resolver treats wrapped and unwrapped refs
  /// identically; Phase 2 will require this flag to be set whenever
  /// the resolved column has `AmplVariableKind::StateBacked`.
  bool state_wrapped {false};

  /// True when the user wrote `prev(<element_ref>)` — a lagged reference
  /// to the variable's value in the PREVIOUS block within the stage.  At
  /// the first block of a stage the lag resolves to the cross-phase
  /// INCOMING column (a `block_state` DecisionVariable's `value_in`),
  /// i.e. the previous phase's end-of-phase value pinned by the SDDP
  /// forward pass.  This is what lets a user constraint express a
  /// per-block storage balance `vol[b] = prev(vol) + inflow - release`.
  bool prev_wrapped {false};
};

/**
 * @brief A predicate that filters a `sum(...)` aggregation (F4).
 *
 * Evaluated against the metadata registry populated by each element's
 * `add_to_lp` via `SystemContext::register_ampl_element_metadata`.
 * Strings and numbers are both supported; `In` holds a set-of-literals
 * `in {a, b, c}` form (values stored as strings for now).
 *
 * Predicates are conjoined (AND) — disjunctions are deferred to v2.
 */
struct SumPredicate
{
  enum class Op : std::uint8_t
  {
    Eq,  ///< `=` or `==`
    Ne,  ///< `!=` or `<>`
    Lt,  ///< `<`
    Le,  ///< `<=`
    Gt,  ///< `>`
    Ge,  ///< `>=`
    In,  ///< `in {...}`
  };

  std::string attr {};  ///< metadata key, e.g. "type", "bus", "cap"
  Op op {Op::Eq};
  /// For scalar comparisons either `string` or `number` is set; for
  /// `In` the `set` holds the allowed string literals.
  std::optional<std::string> string_value {};
  std::optional<double> number_value {};
  std::vector<std::string> set_values {};
};

/**
 * @brief Aggregation over multiple elements of the same type
 *
 * Represents `sum(element_type("id1","id2",...).attribute)` or
 * `sum(element_type(all).attribute)` — the AMPL-style `sum{g in SET}`.
 *
 * An optional list of `filters` (F4) restricts the sum to elements whose
 * metadata matches every predicate (AND).  For backward compatibility,
 * the legacy `sum(type(all, type="hydro").attr)` syntax is accepted and
 * lowered to a single `type == "hydro"` predicate in `filters`.
 *
 * Examples:
 *   - sum(generator("G1","G2").generation)
 *       → {element_type="generator", element_ids={"G1","G2"},
 *          all_elements=false, attribute="generation"}
 *   - sum(generator(all).generation)
 *       → {element_type="generator", element_ids={},
 *          all_elements=true, attribute="generation"}
 *   - sum(generator(all: type="hydro" and cap>=50).generation)
 *       → {all_elements=true,
 *          filters=[{"type", Eq, "hydro"}, {"cap", Ge, 50}]}
 */
struct SumElementRef
{
  std::string element_type {};  ///< "generator", "demand", "line", etc.
  std::vector<std::string> element_ids {};  ///< Element names/UIDs to sum
  bool all_elements {false};  ///< true = sum over all elements of the type
  std::vector<SumPredicate> filters {};  ///< F4 filter predicates (AND)
  std::string attribute {};  ///< LP attribute to aggregate
};

/// Time window iterated by a `sum{...}` time-aggregator (piece-4 step 2).
enum class TimeWindow : std::uint8_t
{
  Stage,  ///< all blocks of the representative stage
  Day,  ///< blocks within each 24 h day (the `daily_sum` window)
};

/// Per-block weight applied to each term inside a `sum{...}` time-agg.
///
/// `dur[b]` weighting is LOAD-BEARING for RATE variables (flow [m³/s]):
/// `Σ_b Δt_b · flow_b` is energy [m³], `Σ_b flow_b` is meaningless.  See
/// `feedback_stage_avg_flows`.  `Count` (unweighted) is the right choice
/// for per-day event counts (e.g. `Σ startups ≤ N`), matching unweighted
/// `daily_sum`.
enum class TimeAggWeight : std::uint8_t
{
  Count,  ///< unweighted: Σ_b col_b (per-day / per-stage count)
  Duration,  ///< Δt-weighted: Σ_b Δt_b · col_b (energy)
};

/// @brief `sum{idx in window} [dur[idx] *] inner_expr` — TIME aggregation.
///
/// A NEW operator distinct from element `sum(...)` (`SumElementRef`): this
/// aggregates an inner linear expression over the BLOCKS of `window`,
/// letting a coarse-scope (`stage`/`phase`/`global`) constraint row reach
/// per-block variables.  The resolver iterates the window's blocks,
/// re-resolving `inner` at each block (with that block as the ambient
/// block) and adding `weight_b · inner_b` into the outer row.
///
/// Spelling `sum{b in stage} dur[b] * waterway('w').flow` sets
/// `weight = Duration` (the `dur[b]` prefix); plain `sum{b in stage} ...`
/// is `weight = Count`.
///
/// Body is defined AFTER `ConstraintTerm` (it holds a
/// `std::vector<ConstraintTerm>`); `ConstraintTerm` references it via
/// `std::shared_ptr<const TimeAggRef>` (forward-declared below).
struct TimeAggRef;

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

// Forward declarations for nonlinear wrapper nodes that contain nested
// linear expressions (`std::vector<ConstraintTerm>`).  The wrapper
// bodies are defined *after* `ConstraintTerm` to avoid instantiating
// `std::vector<ConstraintTerm>` with an incomplete element type.
// `ConstraintTerm` therefore holds `std::shared_ptr<const AbsExpr>`
// etc. (opaque pointer to forward-declared type, copy-safe).
struct AbsExpr;
struct MinMaxExpr;
struct IfExpr;

/// Kind of `min`/`max` expression (F7).
enum class MinMaxKind : std::uint8_t
{
  Max,  ///< `max(a, b, c)` — convex upper envelope
  Min,  ///< `min(a, b, c)` — concave lower envelope
};

/// A single atomic comparison for `if`-condition evaluation (F8).
///
/// The left-hand side is a loop coordinate (`scenario`, `stage`, or
/// `block`), the right-hand side a literal integer or a set of
/// integers.  Evaluated at LP-construction time per domain instance.
struct IfCondAtom
{
  enum class Coord : std::uint8_t
  {
    Scenario,
    Stage,
    Block,
  };
  enum class Op : std::uint8_t
  {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    In,
  };
  Coord coord {Coord::Stage};
  Op op {Op::Eq};
  std::optional<uid_t> number {};  ///< RHS for scalar comparisons
  std::vector<uid_t> set_values {};  ///< RHS for `in { … }`
};

/// Shape of a nonlinear wrapper term for convexity checking.
///
/// - `UpperEnvelope` applies to `abs(x)` and `max(x_i)` — both are
///   convex functions.  A term `c · f(x)` inside a constraint is
///   lowerable iff `c > 0` with `<=` (or `c < 0` with `>=`, the
///   algebraically equivalent form).
/// - `LowerEnvelope` applies to `min(x_i)` — concave.  A term
///   `c · f(x)` is lowerable iff `c > 0` with `>=` (or `c < 0` with
///   `<=`).
enum class ConvexKind : std::uint8_t
{
  UpperEnvelope,  ///< abs(x), max(x_i)
  LowerEnvelope,  ///< min(x_i)
};

namespace detail
{

/// Pure convexity check for a single nonlinear wrapper term.
///
/// Returns `true` iff `coef · f(x)` can be safely lowered under the
/// given `ctype` without flipping the feasible set (i.e. the resulting
/// LP is a valid outer approximation, not an inner one).  See
/// `ConvexKind` for the per-shape rules.
///
/// Pure / `noexcept` / `constexpr` so it is trivially unit-testable
/// without instantiating a `LinearProblem`.
[[nodiscard]] constexpr bool check_convexity(ConvexKind kind,
                                             ConstraintType ctype,
                                             double coef) noexcept
{
  if (kind == ConvexKind::UpperEnvelope) {
    return (ctype == ConstraintType::LESS_EQUAL && coef > 0.0)
        || (ctype == ConstraintType::GREATER_EQUAL && coef < 0.0);
  }
  // LowerEnvelope (min)
  return (ctype == ConstraintType::GREATER_EQUAL && coef > 0.0)
      || (ctype == ConstraintType::LESS_EQUAL && coef < 0.0);
}

/// Pure evaluator for a single `IfCondAtom` against a coordinate value.
///
/// The caller is responsible for extracting the integer value of the
/// coord that `atom.coord` names (scenario/stage/block uid) and passing
/// it here.  This split keeps the evaluator free of any LP context,
/// making it trivial to unit-test and `constexpr`/`noexcept`.
[[nodiscard]] constexpr bool eval_if_atom(const IfCondAtom& atom,
                                          uid_t value) noexcept
{
  switch (atom.op) {
    case IfCondAtom::Op::Eq:
      return atom.number.has_value() && value == *atom.number;
    case IfCondAtom::Op::Ne:
      return atom.number.has_value() && value != *atom.number;
    case IfCondAtom::Op::Lt:
      return atom.number.has_value() && value < *atom.number;
    case IfCondAtom::Op::Le:
      return atom.number.has_value() && value <= *atom.number;
    case IfCondAtom::Op::Gt:
      return atom.number.has_value() && value > *atom.number;
    case IfCondAtom::Op::Ge:
      return atom.number.has_value() && value >= *atom.number;
    case IfCondAtom::Op::In:
      return std::ranges::contains(atom.set_values, value);
  }
  return false;
}

}  // namespace detail

/**
 * @brief A single term in a constraint expression
 *
 * Either:
 *   - coefficient × element reference (single variable term)
 *   - coefficient × sum reference (aggregation term)
 *   - coefficient × named parameter (resolved at LP time)
 *   - coefficient × abs(linear) wrapper (F5)
 *   - coefficient × min/max(args…) wrapper (F7)
 *   - coefficient × if-then-else wrapper (F8)
 *   - standalone coefficient (constant term, all nullopt)
 *
 * The wrapper payloads (`AbsExpr`, `MinMaxExpr`, `IfExpr`) are stored
 * by `std::shared_ptr<const …>` so that `ConstraintTerm` remains
 * default-copyable even though those types recursively contain
 * `std::vector<ConstraintTerm>`.  Sharing is fine because the AST is
 * immutable after parsing.
 */
struct ConstraintTerm
{
  double coefficient {1.0};  ///< Scalar multiplier
  std::optional<ElementRef> element {};  ///< Single element (nullopt = none)
  std::optional<SumElementRef>
      sum_ref {};  ///< Sum aggregation (nullopt = none)
  std::optional<std::string>
      param_name {};  ///< Named user parameter (nullopt = none)
  std::shared_ptr<const AbsExpr> abs_expr {};  ///< `abs(...)` wrapper (F5)
  std::shared_ptr<const MinMaxExpr>
      minmax_expr {};  ///< `min`/`max` wrapper (F7)
  std::shared_ptr<const IfExpr>
      if_expr {};  ///< `if ... then ... else ...` (F8)
  std::shared_ptr<const TimeAggRef>
      time_agg {};  ///< `sum{idx in window} ...` TIME aggregation (piece-4)

  /// Optional **per-block coefficient profile** (F9).
  ///
  /// When set, the term's effective coefficient at block ordinal `b`
  /// (0-based position within the stage's block list) is
  /// `coeff_profile[min(b, size()-1)]` — the last entry broadcasts to
  /// every trailing block, so a profile shorter than the stage's block
  /// count still resolves.  The `coefficient` scalar is folded into the
  /// profile at parse time (see `scale_terms`), so a profile is always
  /// the authoritative per-block value when present.
  ///
  /// Authored in the grammar as a bracketed list standing in for the
  /// leading scalar multiplier of a single variable-bearing term, e.g.
  /// `[1, 2, 3] * generator("G1").generation`.  Mirrors the per-block
  /// `UserConstraint.rhs` schedule on the LHS.  Unset ⇒ the scalar
  /// `coefficient` is used uniformly across all blocks (backward
  /// compatible).
  std::optional<std::vector<double>> coeff_profile {};

  /// Resolve the effective coefficient for the given 0-based block
  /// ordinal within the stage.  Returns the profile entry (clamped to
  /// the last when the ordinal runs past the profile) when a per-block
  /// profile is set, otherwise the scalar `coefficient`.
  [[nodiscard]] constexpr double coeff_at(
      std::size_t block_ordinal) const noexcept
  {
    if (coeff_profile && !coeff_profile->empty()) {
      const auto idx = std::min(block_ordinal, coeff_profile->size() - 1);
      return (*coeff_profile)[idx];
    }
    return coefficient;
  }

  /// Stored 1-BASED column of the term's anchor token in the source
  /// expression string, populated by the parser when the term is
  /// constructed.  Surfaced in resolver diagnostics so a stale
  /// reference reports a ``at column N`` source location instead of
  /// just the constraint name (task #55, the P1 safety win).
  ///
  /// Convention: the anchor is the token that names the referenced
  /// entity — the element-type identifier for ``element`` /
  /// ``sum_ref`` terms (``generator(...)``), the identifier for bare
  /// ``param_name`` terms.  ``0`` means UNSET (term constructed by
  /// code that doesn't track positions — e.g. internal lowering for
  /// ``abs`` / ``min`` / ``max`` slack columns).  Stored 1-based so
  /// the sentinel doesn't collide with the legitimate byte-offset-0
  /// (= column 1) case; ``format_error_with_caret`` and
  /// ``make_unresolved_element_error`` use the value verbatim.
  std::size_t column {0};
};

/**
 * @brief `abs(linear_expression)` term (F5).
 *
 * The inner expression is an arbitrary linear combination stored as a
 * vector of `ConstraintTerm`s.  Lowering allocates one auxiliary
 * non-negative variable `t` per occurrence and emits two rows enforcing
 * `t >= |inner|`; the outer row substitutes `coefficient * t` for
 * `coefficient * abs(inner)`.
 *
 * Nested `abs(abs(...))` is rejected at parse time.  Convexity of the
 * outer constraint is enforced at lowering time: only `<=` with
 * positive coefficient (or `>=` with negative coefficient) is accepted.
 */
struct AbsExpr
{
  std::vector<ConstraintTerm> inner {};  ///< Linear expression inside abs(...)
};

/**
 * @brief `max(arg1, arg2, ...)` / `min(arg1, arg2, ...)` term (F7).
 *
 * Each `arg` is itself a linear combination.  Lowering allocates one
 * auxiliary variable `t` per occurrence and emits one row per argument
 * (`arg_i − t ≤ 0` for max, `t − arg_i ≤ 0` for min).  The outer row
 * substitutes `coefficient * t` for `coefficient * max/min(args)`.
 *
 * Convexity:
 *   - `max(...) ≤ k` or `c·max(...) + ... ≤ k` with `c > 0`.
 *   - `min(...) ≥ k` or `c·min(...) + ... ≥ k` with `c > 0`.
 *   - Opposite-direction uses with negative coefficients are equivalent
 *     and also accepted.  All other combinations are rejected.
 */
struct MinMaxExpr
{
  MinMaxKind kind {MinMaxKind::Max};
  std::vector<std::vector<ConstraintTerm>> args {};
};

/**
 * @brief Data-only conditional expression (F8).
 *
 * `if <cond> then (<then_branch>) [ else (<else_branch>) ]`
 *
 * The condition is a conjunction (AND) of atomic coordinate comparisons
 * evaluated per (scenario, stage, block) domain instance at LP
 * construction time.  The selected branch's terms are substituted into
 * the surrounding row; the unselected branch is dropped for that
 * instance.  If no `else` branch is provided, the condition selects
 * between the `then` branch and an empty expression.
 */
struct IfExpr
{
  std::vector<IfCondAtom> cond {};  ///< Conjunction (AND) of atoms
  std::vector<ConstraintTerm> then_branch {};
  std::vector<ConstraintTerm> else_branch {};
};

/// @brief `sum{idx in window} [dur[idx] *] inner_expr` — TIME aggregation.
///
/// See the forward declaration above for the full semantics.  Defined here
/// (after `ConstraintTerm`) because it owns a `std::vector<ConstraintTerm>`;
/// `ConstraintTerm::time_agg` holds it via `std::shared_ptr<const …>`.
struct TimeAggRef
{
  std::string index_name {};  ///< bound block index (e.g. "b")
  TimeWindow window {TimeWindow::Stage};
  TimeAggWeight weight {TimeAggWeight::Count};
  std::vector<ConstraintTerm> inner {};  ///< inner linear expression
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

// ── Time-window granularity (G11 no-silent-collapse guard) ──────────────────
//
// A coarse-scope constraint (`stage`/`phase`/`global`) builds ONE
// representative LP row, so a `sum{...}` time-aggregation window FINER than
// the scope silently aggregates only the representative sub-unit and drops
// the rest (e.g. `stage` scope + `day` window keeps only the rep block's
// day → mid-stage days vanish).  These helpers let the LP layer detect that
// lossy combination and reject it instead of producing a wrong LP.
//
// Granularity rank — SMALLER is FINER.  `Day` (0) is finer than `Stage` (1).
[[nodiscard]] constexpr int time_window_granularity(TimeWindow window) noexcept
{
  switch (window) {
    case TimeWindow::Day:
      return 0;
    case TimeWindow::Stage:
      return 1;
  }
  return 1;  // unreachable (switch is exhaustive)
}

// Human-readable spelling of a `TimeWindow` for diagnostics.
[[nodiscard]] constexpr std::string_view time_window_name(
    TimeWindow window) noexcept
{
  switch (window) {
    case TimeWindow::Day:
      return "day";
    case TimeWindow::Stage:
      return "stage";
  }
  return "?";  // unreachable
}

namespace detail
{

// Recursively scan @p terms for the FINEST `TimeAggRef` window present,
// folding the running minimum (finest) into @p best.  Returns the finest
// window found across this term list and every nested wrapper
// (abs/min/max/if branches and nested time-aggregations).  `std::nullopt`
// means the (sub)expression contains no time-aggregation at all.
//
// Declared here and defined after the wrapper bodies so it can recurse into
// `AbsExpr` / `MinMaxExpr` / `IfExpr` / `TimeAggRef`, all of which own a
// `std::vector<ConstraintTerm>`.
[[nodiscard]] std::optional<TimeWindow> finest_time_window_in_terms(
    const std::vector<ConstraintTerm>& terms,
    std::optional<TimeWindow> best = std::nullopt) noexcept;

inline std::optional<TimeWindow> finest_time_window_in_terms(
    const std::vector<ConstraintTerm>& terms,
    std::optional<TimeWindow> best) noexcept
{
  const auto fold = [&best](TimeWindow w)
  {
    if (!best.has_value()
        || time_window_granularity(w) < time_window_granularity(*best))
    {
      best = w;
    }
  };

  for (const auto& t : terms) {
    if (t.time_agg) {
      fold(t.time_agg->window);
      best = finest_time_window_in_terms(t.time_agg->inner, best);
    }
    if (t.abs_expr) {
      best = finest_time_window_in_terms(t.abs_expr->inner, best);
    }
    if (t.minmax_expr) {
      for (const auto& arg : t.minmax_expr->args) {
        best = finest_time_window_in_terms(arg, best);
      }
    }
    if (t.if_expr) {
      best = finest_time_window_in_terms(t.if_expr->then_branch, best);
      best = finest_time_window_in_terms(t.if_expr->else_branch, best);
    }
  }
  return best;
}

}  // namespace detail

// Finest `TimeWindow` used anywhere in @p expr (across all wrappers /
// nested aggregations), or `std::nullopt` when the expression contains no
// `sum{...}` time-aggregation.  Used by the LP layer to compare against the
// constraint scope and reject lossy fine-window-under-coarse-scope combos.
[[nodiscard]] inline std::optional<TimeWindow> expr_finest_time_window(
    const ConstraintExpr& expr) noexcept
{
  return detail::finest_time_window_in_terms(expr.terms);
}

}  // namespace gtopt
