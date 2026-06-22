/**
 * @file      user_constraint_lp.cpp
 * @brief     LP element implementation for user-defined constraints
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <stdexcept>

#include <gtopt/constraint_expr.hpp>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/element_column_resolver.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>
#include <gtopt/user_constraint_lp.hpp>
#include <gtopt/user_param.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// ── Domain check ─────────────────────────────────────────────────────────────

template<typename T>
  requires(strong::is_strong_type<T>::value
           || requires(const T& t) { value_of(t); })
[[nodiscard]] bool in_range(const IndexRange& range, T value)
{
  if (range.is_all) {
    return true;
  }
  return std::ranges::contains(range.values, value_of(value));
}

// ── Build UserParamMap ───────────────────────────────────────────────────────

[[nodiscard]] UserParamMap build_param_map(const System& system)
{
  UserParamMap map;
  for (const auto& p : system.user_param_array) {
    map[p.name] = p;
  }
  return map;
}

/// Resolve a named parameter to its value for the given stage.
/// Returns nullopt if the name is not found in the map.
[[nodiscard]] std::optional<double> resolve_param(const UserParamMap& params,
                                                  const std::string& name,
                                                  const StageLP& stage)
{
  const auto it = params.find(name);
  if (it == params.end()) {
    return std::nullopt;
  }
  const auto& param = it->second;

  // Monthly-indexed parameter: use stage's calendar month
  if (param.monthly.has_value()) {
    const auto& monthly = *param.monthly;
    const auto month = stage.month();
    if (!month.has_value()) {
      // Fail fast: silently returning january was a hidden source of
      // subtly wrong LPs in constraints like La Invernada that depend
      // on calendar modulation.
      throw std::runtime_error(std::format(
          "UserConstraint monthly parameter '{}' requires stage.month to "
          "be set on stage uid={} but the stage has no calendar month. "
          "Fix: add `.month = MonthType::<jan..dec>` to the Stage in the "
          "Simulation's stage_array, or remove the monthly override from "
          "UserParam '{}'.",
          name,
          stage.uid(),
          name));
    }
    // MonthType is 1-based (january=1), monthly vector is 0-based
    const auto idx = static_cast<std::size_t>(*month) - 1;
    if (idx >= monthly.size()) {
      throw std::runtime_error(std::format(
          "UserConstraint monthly parameter '{}' has {} entries but stage "
          "uid={} has month={} (index {}). Fix: extend the `monthly` "
          "array to 12 entries (january..december) in UserParam '{}'.",
          name,
          monthly.size(),
          stage.uid(),
          std::to_underlying(*month),
          idx,
          name));
    }
    return monthly[idx];
  }

  // Scalar parameter
  return param.value.value_or(0.0);
}

// ── SparseRow bounds from ConstraintExpr ─────────────────────────────────────

void apply_constraint_bounds(SparseRow& row, const ConstraintExpr& expr)
{
  switch (expr.constraint_type) {
    case ConstraintType::LESS_EQUAL:
      row.less_equal(expr.rhs);
      break;
    case ConstraintType::GREATER_EQUAL:
      row.greater_equal(expr.rhs);
      break;
    case ConstraintType::EQUAL:
      row.equal(expr.rhs);
      break;
    case ConstraintType::RANGE:
      row.bound(expr.lower_bound.value_or(-LinearProblem::DblMax),
                expr.upper_bound.value_or(LinearProblem::DblMax));
      break;
  }
}

// ── Row-building helper with F5/F7/F8 support ──────────────────────────────

// LowerCtx bundles the ambient (scenario, stage, block) references and
// the LP the row-builder writes into.  The const-ref members are
// intentional: this struct is a strictly scoped, stack-allocated
// "context bag" whose lifetime never outlives the single call that
// creates it, so the normal concerns about const/ref data members
// (copy/assign/lifetime surprises) do not apply here.  The NOLINT
// below suppresses the corresponding guideline warning.
// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
struct LowerCtx
{
  const SystemContext& sc;
  const ScenarioLP& scenario;
  const StageLP& stage;
  const BlockLP& block;
  const UserParamMap& param_map;
  const UserConstraint& uc;
  ConstraintType ctype {ConstraintType::LESS_EQUAL};
  bool is_debug {false};
  bool is_strict {false};
  LinearProblem& lp;
  /// 0-based position of `block` within the stage's block list.  Selects
  /// the per-block entry of any `ConstraintTerm::coeff_profile` (F9) via
  /// `term.coeff_at(block_ordinal)`.  When a term carries no profile this
  /// is unused and the scalar `coefficient` is applied uniformly.
  std::size_t block_ordinal {0};
  /// Monotonic counter for auxiliary rows / cols emitted while lowering
  /// the current user constraint row.  Threaded through the
  /// `(context, extra)` field of each aux row so abs(x) / min(a,b) /
  /// max(a,b) lowerings produce unique `(class, constraint, uid,
  /// context)` metadata — the eager duplicate detector in
  /// `LinearProblem::add_row` would otherwise fire on the second
  /// aux row, which shares everything but the row contents with the
  /// first.
  int aux_counter {0};
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

/// Sign constraint for an auxiliary lowering column.
enum class AuxSign : std::uint8_t
{
  NonNegative,  ///< `t >= 0`   — used for `abs(x)` (t >= |x|)
  Free,  ///< t free    — used for `min`/`max` auxiliary
};

struct BuildResult
{
  bool has_vars {false};
  double param_shift {0.0};
};

// Evaluate an if-condition atom against the current (scenario, stage, block).
// The operator/value logic lives in `gtopt::detail::eval_if_atom` so it
// can be unit-tested without constructing an LP; this wrapper just
// projects the named coordinate out of the lowering context.
[[nodiscard]] bool eval_if_atom(const LowerCtx& ctx,
                                const IfCondAtom& atom) noexcept
{
  uid_t coord_val = 0;
  switch (atom.coord) {
    case IfCondAtom::Coord::Scenario:
      coord_val = value_of(ctx.scenario.uid());
      break;
    case IfCondAtom::Coord::Stage:
      coord_val = value_of(ctx.stage.uid());
      break;
    case IfCondAtom::Coord::Block:
      coord_val = value_of(ctx.block.uid());
      break;
  }
  return detail::eval_if_atom(atom, coord_val);
}

// Convert the user-constraint row's BlockContext into a
// BlockExContext with a unique `extra` index so multiple aux rows
// don't collide on the metadata-based duplicate detector.  For
// already-extended contexts, the aux counter is folded into the
// existing extra slot; for other variants (StageContext etc.) we
// return the context unchanged — lowering paths that use them
// create at most one aux row per user constraint row, so there's
// nothing to disambiguate.
[[nodiscard]] LpContext make_aux_context(const LpContext& base, int extra)
{
  return std::visit(
      [extra]<typename T>(const T& c) -> LpContext
      {
        if constexpr (std::is_same_v<T, BlockContext>) {
          return BlockExContext {
              std::get<0>(c),
              std::get<1>(c),
              std::get<2>(c),
              extra,
          };
        } else if constexpr (std::is_same_v<T, BlockExContext>) {
          // Multiple user-constraint block rows may share the same
          // source (scenario, stage, block, outer_extra); we fold
          // the aux counter into the low bits to keep uniqueness.
          return BlockExContext {
              std::get<0>(c),
              std::get<1>(c),
              std::get<2>(c),
              (std::get<3>(c) * 1000) + extra,
          };
        }
        return T {c};
      },
      base);
}

// Fresh SparseRow that inherits identity from `row` but receives a
// unique `(context, extra)` slot from `ctx.aux_counter` so repeated
// aux-row emissions during a single lowering do not collide on the
// metadata-based duplicate detector in `LinearProblem::add_row`.
[[nodiscard]] SparseRow make_aux_row(LowerCtx& ctx, const SparseRow& row)
{
  SparseRow aux;
  aux.class_name = row.class_name;
  aux.constraint_name = row.constraint_name;
  aux.variable_uid = row.variable_uid;
  aux.context = make_aux_context(row.context, ++ctx.aux_counter);
  return aux;
}

// Allocate a new auxiliary column named @p var_name for the user
// constraint at the current block context.  `AuxSign::NonNegative`
// yields a `t >= 0` column (used by `abs(x)` to satisfy `t >= |x|`);
// `AuxSign::Free` yields an unbounded column (used by `min`/`max`).
[[nodiscard]] ColIndex add_aux_col(LowerCtx& ctx,
                                   const SparseRow& row,
                                   std::string_view var_name,
                                   AuxSign sign)
{
  SparseCol col;
  col.lowb = (sign == AuxSign::NonNegative) ? 0.0 : -LinearProblem::DblMax;
  col.uppb = LinearProblem::DblMax;
  col.class_name = UserConstraint::class_name.full_name();
  col.variable_name = var_name;
  col.variable_uid = row.variable_uid;
  col.context = row.context;
  return ctx.lp.add_col(std::move(col));
}

// Negate every coefficient in @p cmap in-place.  Small helper to
// dedupe the `for (k,v) : cmap { negated[k] = -v; }` pattern that
// appears in the abs and min lowerings.  Uses `std::views::values`
// so the same code works across every `flat_map` backend gtopt may
// use (boost, std::flat_map, std::map) regardless of whether the
// iterator dereferences to a proxy pair or a plain one.
void negate_cmap(SparseRow::cmap_t& cmap) noexcept
{
  for (double& v : std::views::values(cmap)) {
    v = -v;
  }
}

// Short, user-facing spelling of a ConstraintType for error messages.
[[nodiscard]] constexpr std::string_view ctype_to_symbol(
    ConstraintType ctype) noexcept
{
  switch (ctype) {
    case ConstraintType::LESS_EQUAL:
      return "<=";
    case ConstraintType::GREATER_EQUAL:
      return ">=";
    case ConstraintType::EQUAL:
      return "=";
    case ConstraintType::RANGE:
      return "range";
  }
  return "?";
}

// Reject nested nonlinear wrappers inside abs/min/max arguments (v1).
void reject_nested_wrappers(const LowerCtx& ctx,
                            const std::vector<ConstraintTerm>& terms,
                            std::string_view outer)
{
  for (const auto& t : terms) {
    if (t.abs_expr || t.minmax_expr || t.if_expr) {
      throw std::runtime_error(
          std::format("user_constraint '{}': nested abs/min/max/if inside "
                      "{}(...) is not supported",
                      ctx.uc.name,
                      outer));
    }
  }
}

// Join a list of candidate names into a comma-separated string for an
// error hint, e.g. {"g1","g2"} → "g1, g2".  Returns empty for an empty
// list so the caller can omit the hint entirely.
[[nodiscard]] std::string join_candidates(
    const std::vector<std::string_view>& names)
{
  std::string out;
  for (const auto& n : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += n;
  }
  return out;
}

// Build the informative error message for a genuinely-undefined element
// reference inside a user constraint.  Names the UC, the offending
// `class("id").attr` term and block, says what is wrong, and — where
// cheap — appends a "did you mean ...?" hint (closest registered element
// names of the same class) or a "valid attributes are ..." hint (the
// attributes actually registered as LP variables on that element).  The
// caller wraps the returned string in `std::runtime_error` so gtopt
// exits non-zero.
[[nodiscard]] std::string make_unresolved_element_error(const LowerCtx& ctx,
                                                        const ElementRef& ref,
                                                        std::size_t column = 0)
{
  // Does the element NAME/UID resolve at all?  If not → unknown element
  // name; suggest the nearest registered names.  If it does → the name is
  // fine but the attribute is not a registered LP variable / parameter →
  // suggest the valid attribute list.
  const auto uid_opt =
      ctx.sc.lookup_ampl_element_uid(ref.element_type, ref.element_id);

  std::string what;
  std::string hint;
  if (!uid_opt.has_value()) {
    what =
        std::format("unknown {} name '{}'", ref.element_type, ref.element_id);
    const auto cands =
        ctx.sc.ampl_element_name_candidates(ref.element_type, ref.element_id);
    if (!cands.empty()) {
      hint = std::format(" — did you mean: {}?", join_candidates(cands));
    }
  } else {
    what = std::format("unknown attribute '{}' on {} '{}'",
                       ref.attribute,
                       ref.element_type,
                       ref.element_id);
    const auto attrs =
        ctx.sc.ampl_attribute_candidates(ref.element_type, *uid_opt);
    if (!attrs.empty()) {
      hint = std::format(" — valid attributes are: {}", join_candidates(attrs));
    }
  }

  // Source-location prefix (task #55 — P1).  Parser stores the
  // column 1-based on the term (see ``ConstraintTerm::column``), so
  // we use it verbatim.  ``column == 0`` (unset — term not built by
  // ``parse_primary``, e.g. internal lowering of abs/min/max into
  // auxiliary slack columns) omits the location so the message stays
  // clean.
  std::string location;
  if (column > 0) {
    location = std::format(" at column {}", column);
  }

  return std::format(
      "user_constraint '{}'{}: cannot resolve element reference "
      "'{}({}).{}' (block {}) — {}{}",
      ctx.uc.name,
      location,
      ref.element_type,
      ref.element_id,
      ref.attribute,
      ctx.block.uid(),
      what,
      hint);
}

// Recursive row-builder.  Handles element / sum / param terms directly
// and lowers `abs`, `min`/`max`, `if` wrappers into auxiliary columns
// and helper rows on the fly.
BuildResult build_row_from_terms(LowerCtx& ctx,
                                 const std::vector<ConstraintTerm>& terms,
                                 double outer_coef,
                                 SparseRow& row)
{
  BuildResult out;

  for (const auto& term : terms) {
    // Resolve the term's per-block coefficient: when the term carries a
    // `coeff_profile` (F9), `coeff_at` selects the entry for this block's
    // ordinal within the stage; otherwise it returns the scalar
    // `coefficient`.  Backward-compatible — scalar terms are unchanged.
    const double coef = outer_coef * term.coeff_at(ctx.block_ordinal);

    if (term.element) {
      const std::size_t before_col = row.size();
      const auto res = resolve_col_to_row(ctx.sc,
                                          ctx.scenario,
                                          ctx.stage,
                                          ctx.block,
                                          *term.element,
                                          coef,
                                          row,
                                          ctx.lp);
      if (res.emitted) {
        out.has_vars = true;
        // Fold any AMPL offset (Option C demand: load = col + lmax)
        // onto the RHS via the existing `param_shift` accumulator —
        // `apply_constraint_bounds` later subtracts param_shift from
        // the RHS, which is exactly what offset_shift needs too.
        out.param_shift += res.offset_shift;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "element '{}.{}' added {} col(s), coeff={}, offset_shift={}",
              ctx.uc.name,
              ctx.block.uid(),
              term.element->element_type,
              term.element->attribute,
              row.size() - before_col,
              coef,
              res.offset_shift);
        }
      } else if (auto pval = resolve_single_param(
                     ctx.sc, ctx.scenario, ctx.stage, ctx.block, *term.element))
      {
        out.param_shift += coef * *pval;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "data param value={} coeff={}",
              ctx.uc.name,
              ctx.block.uid(),
              *pval,
              coef);
        }
      } else if (const auto suppression = ctx.sc.find_ampl_suppression(
                     term.element->element_type, term.element->attribute);
                 suppression.has_value() || res.element_known)
      {
        // ── The DEFINED silent-zero cases ──────────────────────────────
        // All legitimately contribute 0 to the LHS — they are NOT
        // errors and must stay silent regardless of constraint_mode:
        //
        //  (a) suppressed-by-mode: the class or (class, attribute) pair
        //      was explicitly marked unavailable by the active planning
        //      mode (e.g. `line` under `use_single_bus`, `bus.theta`
        //      when Kirchhoff is disabled).  The user configured the
        //      mode; the reference is not a typo, it simply does not
        //      apply.
        //
        //  (b) element-known-but-offline: the element IS registered as
        //      an LP variable somewhere (res.element_known == true) but
        //      has no LP column for THIS specific (scenario, stage,
        //      block) — typically a generator with `pmax = 0` on this
        //      block (GeneratorLP omits the column), or a generator
        //      with `pmax = 0` on EVERY block such that no column is
        //      created at all (LP-attr-dormant — element_known is set
        //      via `find_ampl_class_attribute` in that sub-case).  The
        //      underlying variable is implicitly 0, so the term
        //      contributes 0 and the constraint stays well-formed
        //      without per-block expressions.  This was the root cause
        //      of the CEN PCP weekly LNG +229% over-dispatch fix (per-
        //      block case) and the strict-resolver throw on PANGUE_U1
        //      reservoir constraints (all-horizon case).
        //
        // Critically, this is the ONLY leniency: an UNKNOWN /
        // UNDEFINED reference (res.element_known == false AND not
        // suppressed) falls through to the unconditional throw below.
        // Traced at DEBUG so diagnostics are available when needed; the
        // reason is folded into a single string so this branch carries
        // exactly one trace statement (no clone-able if/else split).
        const std::string_view reason = suppression.has_value()
            ? std::string_view {"suppressed by planning mode"}
            : std::string_view {
                  "no LP column for this block "
                  "(inactive: pmax=0 or similar)"};
        SPDLOG_DEBUG(
            "user_constraint '{}': dropping term '{}({}).{}' (block {}) — "
            "{} — term contributes 0 to LHS",
            ctx.uc.name,
            term.element->element_type,
            term.element->element_id,
            term.element->attribute,
            ctx.block.uid(),
            reason);
        (void)reason;  // unused when SPDLOG_DEBUG compiles out (CIFast)
      } else {
        // The element ref resolved neither as an LP column (no matching
        // variable in the AMPL registry) nor as a data parameter — a
        // genuine *undefined* reference: a typo, a deleted element, or
        // an attribute that is not a registered LP variable / parameter
        // on this element.  This is the most insidious failure mode:
        // silently skipping the term makes the constraint vacuously
        // satisfied while the LP still solves.  It is therefore a hard
        // error UNCONDITIONALLY — `constraint_mode` (normal/strict/debug)
        // only governs verbosity for *defined* situations, never whether
        // a genuinely-undefined reference is tolerated.  Note this branch
        // is reached only when `res.element_known == false`; the
        // element-registered-but-offline (pmax==0) case is the
        // `res.element_known` branch above and stays a silent 0.
        throw std::runtime_error(
            make_unresolved_element_error(ctx, *term.element, term.column));
      }
      continue;
    }

    if (term.sum_ref) {
      const std::size_t before = row.size();
      const double sum_offset_shift = collect_sum_cols(ctx.sc,
                                                       ctx.scenario,
                                                       ctx.stage,
                                                       ctx.block,
                                                       *term.sum_ref,
                                                       coef,
                                                       row,
                                                       ctx.lp);
      if (row.size() > before) {
        out.has_vars = true;
        // Fold the per-element offset sum (Option C demand etc.)
        // onto the RHS via the existing param_shift mechanism.
        out.param_shift += sum_offset_shift;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "sum resolved {} columns (offset_shift={})",
              ctx.uc.name,
              ctx.block.uid(),
              row.size() - before,
              sum_offset_shift);
        }
      }
      continue;
    }

    if (term.param_name) {
      // Bind the parameter name to a local up front so subsequent uses
      // never re-dereference the optional (keeps the optional-access
      // checker happy without a NOLINT, which is forbidden for this
      // diagnostic).
      const std::string& param_name = *term.param_name;
      if (auto pval = resolve_param(ctx.param_map, param_name, ctx.stage)) {
        out.param_shift += coef * *pval;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "param '{}' = {} coeff={}",
              ctx.uc.name,
              ctx.block.uid(),
              param_name,
              *pval,
              coef);
        }
      } else {
        // An undefined named parameter is a genuine *undefined* reference
        // — throw UNCONDITIONALLY regardless of `constraint_mode` (silent
        // skipping would make the term vanish and quietly distort the
        // constraint).  Append the list of parameters that ARE defined in
        // the system's `user_param_array` as a hint.
        std::vector<std::string_view> defined;
        defined.reserve(ctx.param_map.size());
        for (const auto& [pname, _] : ctx.param_map) {
          defined.push_back(pname);
        }
        std::string hint;
        if (!defined.empty()) {
          hint = std::format(" — defined parameters are: {}",
                             join_candidates(defined));
        } else {
          hint =
              " — no user parameters are defined (add one to the system's "
              "user_param_array, or reference an element attribute instead)";
        }
        throw std::runtime_error(
            std::format("user_constraint '{}': unknown parameter '{}'{}",
                        ctx.uc.name,
                        param_name,
                        hint));
      }
      continue;
    }

    if (term.abs_expr) {
      // abs(x) is a convex upper envelope — same convexity rules as max.
      if (!detail::check_convexity(ConvexKind::UpperEnvelope, ctx.ctype, coef))
      {
        throw std::runtime_error(
            std::format("user_constraint '{}': non-convex use of abs(...) — "
                        "'c*abs(x)' requires c>0 with '<=' or c<0 with '>=' "
                        "(got c={}, constraint='{}')",
                        ctx.uc.name,
                        coef,
                        ctype_to_symbol(ctx.ctype)));
      }
      reject_nested_wrappers(ctx, term.abs_expr->inner, "abs");

      // Resolve inner linear expression into a local row.  This
      // local row is never added to the LP, so it does not need a
      // unique aux context — copy without bumping the counter to
      // avoid wasting indices.
      SparseRow inner_row;
      inner_row.class_name = row.class_name;
      inner_row.constraint_name = row.constraint_name;
      inner_row.variable_uid = row.variable_uid;
      inner_row.context = row.context;
      auto inner_res =
          build_row_from_terms(ctx, term.abs_expr->inner, 1.0, inner_row);

      if (inner_row.cmap.empty()) {
        // Inner reduced to a pure constant — contribute |c| directly.
        out.param_shift += coef * std::abs(inner_res.param_shift);
        continue;
      }

      const auto t_idx = add_aux_col(ctx, row, "abs_aux", AuxSign::NonNegative);

      // Row 1: inner_vars − t ≤ −inner_shift
      SparseRow r1 = make_aux_row(ctx, row);
      r1.cmap = inner_row.cmap;
      r1.cmap[t_idx] = -1.0;
      r1.less_equal(-inner_res.param_shift);
      (void)ctx.lp.add_row(std::move(r1));

      // Row 2: −inner_vars − t ≤ inner_shift
      SparseRow r2 = make_aux_row(ctx, row);
      r2.cmap = inner_row.cmap;
      negate_cmap(r2.cmap);
      r2.cmap[t_idx] = -1.0;
      r2.less_equal(inner_res.param_shift);
      (void)ctx.lp.add_row(std::move(r2));

      row.cmap[t_idx] += coef;
      out.has_vars = true;
      if (ctx.is_debug) {
        spdlog::info(
            "  user_constraint '{}' block {}: abs(...) lowered "
            "(aux col={}, inner_shift={}, coef={})",
            ctx.uc.name,
            ctx.block.uid(),
            value_of(t_idx),
            inner_res.param_shift,
            coef);
      }
      continue;
    }

    if (term.minmax_expr) {
      const bool is_max = term.minmax_expr->kind == MinMaxKind::Max;
      const auto kind =
          is_max ? ConvexKind::UpperEnvelope : ConvexKind::LowerEnvelope;
      if (!detail::check_convexity(kind, ctx.ctype, coef)) {
        const std::string_view op_name = is_max ? "max" : "min";
        const std::string_view ok_op = is_max ? "<=" : ">=";
        const std::string_view bad_op = is_max ? ">=" : "<=";
        throw std::runtime_error(
            std::format("user_constraint '{}': non-convex use of {}(...) — "
                        "'c*{}(...)' requires c>0 with '{}' or c<0 with '{}' "
                        "(got c={}, constraint='{}')",
                        ctx.uc.name,
                        op_name,
                        op_name,
                        ok_op,
                        bad_op,
                        coef,
                        ctype_to_symbol(ctx.ctype)));
      }

      const auto t_idx =
          add_aux_col(ctx, row, is_max ? "max_aux" : "min_aux", AuxSign::Free);

      for (const auto& arg : term.minmax_expr->args) {
        reject_nested_wrappers(ctx, arg, is_max ? "max" : "min");

        SparseRow ar = make_aux_row(ctx, row);
        auto arg_res = build_row_from_terms(ctx, arg, 1.0, ar);

        if (is_max) {
          // arg_vars + arg_shift ≤ t  ⟹  arg_vars − t ≤ −arg_shift
          ar.cmap[t_idx] = -1.0;
          ar.less_equal(-arg_res.param_shift);
        } else {
          // t ≤ arg_vars + arg_shift  ⟹  −arg_vars + t ≤ arg_shift
          negate_cmap(ar.cmap);
          ar.cmap[t_idx] = 1.0;
          ar.less_equal(arg_res.param_shift);
        }
        (void)ctx.lp.add_row(std::move(ar));
      }

      row.cmap[t_idx] += coef;
      out.has_vars = true;
      if (ctx.is_debug) {
        spdlog::info(
            "  user_constraint '{}' block {}: {}(...) lowered "
            "(aux col={}, {} arg(s), coef={})",
            ctx.uc.name,
            ctx.block.uid(),
            is_max ? "max" : "min",
            value_of(t_idx),
            term.minmax_expr->args.size(),
            coef);
      }
      continue;
    }

    if (term.if_expr) {
      const bool cond = std::ranges::all_of(
          term.if_expr->cond,
          [&](const IfCondAtom& atom) { return eval_if_atom(ctx, atom); });
      const auto& branch =
          cond ? term.if_expr->then_branch : term.if_expr->else_branch;
      auto sub = build_row_from_terms(ctx, branch, coef, row);
      if (sub.has_vars) {
        out.has_vars = true;
      }
      out.param_shift += sub.param_shift;
      if (ctx.is_debug) {
        std::string_view branch_label = "then";
        if (!cond) {
          branch_label = term.if_expr->else_branch.empty() ? "empty" : "else";
        }
        spdlog::info("  user_constraint '{}' block {}: if-cond {} → {} branch",
                     ctx.uc.name,
                     ctx.block.uid(),
                     cond ? "true" : "false",
                     branch_label);
      }
      continue;
    }

    // Pure constant term (no element/sum/param/wrapper).  Normally
    // folded into RHS at parse time, but may survive inside an if-branch
    // that still resolves to pure constants.
    out.param_shift += coef;
  }

  return out;
}

}  // anonymous namespace

// ── UserConstraintLP implementation ─────────────────────────────────────────

namespace
{

/// Resolve a soft constraint's slack cost for one block.
///
/// The caller supplies the base `penalty` scalar stored on the
/// `UserConstraint` and the `PenaltyClass` parsed at construction; this
/// helper folds in any per-block unit conversion so that the returned
/// value can be plugged directly into the slack column's LP objective
/// coefficient.
///
/// - `PenaltyClass::Raw` — identity (backwards-compatible).
/// - `PenaltyClass::HydroFlow` — `penalty` is $/m³; multiply by
///   `block_duration_hours × 3600` to obtain $/(m³/s), mirroring
///   `source/flow_right_lp.cpp:74` so soft-flow `UserConstraint` rows
///   compose uniformly with element-level `hydro_fail_cost` pricing.
[[nodiscard]] constexpr Real resolve_block_soft_penalty(
    Real base_penalty,
    PenaltyClass penalty_class,
    Real block_duration_hours) noexcept
{
  switch (penalty_class) {
    case PenaltyClass::Raw:
      return base_penalty;
    case PenaltyClass::HydroFlow:
      return base_penalty * block_duration_hours * 3600.0;
  }
  return base_penalty;  // unreachable (switch is exhaustive)
}

}  // namespace

UserConstraintLP::UserConstraintLP(const UserConstraint& uc, InputContext& ic)
    : ObjectLP<UserConstraint>(uc, ic, Element::class_name)
    , m_scale_type_(enum_from_name<ConstraintScaleType>(
                        uc.constraint_type.value_or("power"))
                        .value_or(ConstraintScaleType::Power))
    , m_rhs_(ic, Element::class_name, id(), std::move(object().rhs))
{
  // Parse `penalty_class` at construction so the hot per-block loop can
  // dispatch on a plain enum.  An unrecognised value is a hard error —
  // silently falling back to `raw` was the exact class of bug that the
  // 2026-04 fail-fast audit was meant to eliminate.
  if (uc.penalty_class.has_value() && !uc.penalty_class->empty()) {
    const auto parsed = enum_from_name<PenaltyClass>(*uc.penalty_class);
    if (!parsed.has_value()) {
      throw std::runtime_error(
          std::format("user_constraint '{}': unknown penalty_class '{}' — "
                      "valid values are: raw, hydro_flow",
                      uc.name,
                      *uc.penalty_class));
    }
    m_penalty_class_ = *parsed;
  }

  // Parse the time-granularity `scope` at construction so the build path can
  // dispatch on a plain enum.  Unset ⇒ Block (legacy behaviour).  An
  // unrecognised value is a hard error (same fail-fast policy as
  // `penalty_class`) — a silent fallback to Block would quietly turn a
  // coarse-scope constraint into a per-block one.
  if (uc.scope.has_value() && !uc.scope->empty()) {
    const auto parsed = enum_from_name<ConstraintScope>(*uc.scope);
    if (!parsed.has_value()) {
      throw std::runtime_error(
          std::format("user_constraint '{}': unknown scope '{}' — "
                      "valid values are: block, stage, phase, global",
                      uc.name,
                      *uc.scope));
    }
    m_scope_ = *parsed;
  }

  if (!uc.expression.empty()) {
    try {
      m_expr_ = ConstraintParser::parse(uc.name, uc.expression);
    } catch (const std::exception& ex) {
      // A malformed expression is a genuine *undefined* situation: the
      // constraint the user wrote cannot be turned into LP rows.  Silently
      // skipping it (the historical behaviour) made the LP vacuously omit a
      // constraint the user demanded — the most insidious failure mode.
      // Re-throw with the UC name + the parser's caret/hint diagnostic so
      // gtopt exits non-zero with a fully informative message.  This is
      // unconditional: `constraint_mode` only governs verbosity for genuine
      // *unresolved-reference* situations at row-assembly time, never whether
      // a parse error is tolerated.
      throw std::runtime_error(
          std::format("user_constraint '{}': expression parse error: {}\n"
                      "  expression: {}\n"
                      "  fix the expression syntax (see the user-constraint "
                      "documentation)",
                      uc.name,
                      ex.what(),
                      uc.expression));
    }
  }

  // Resolve the user-supplied slack column label (the AMPL-style
  // ``var slack_<NAME>;`` declaration in PAMPL, or an explicit JSON
  // ``slack_name`` field).  Stable storage on the LP instance lets us
  // hand a ``string_view`` to ``SparseCol::variable_name`` without
  // worrying about lifetime — the strings live as long as this LP.
  //
  // For EQUALITY constraints the LP creates a pair of slack columns
  // (``_pos`` / ``_neg``); we synthesise the matching suffixed labels
  // here once so the hot per-block loop in ``add_to_lp`` just reaches
  // into the member without any allocation.  One-sided constraints
  // (LE / GE) use ``m_slack_label_`` directly.
  if (uc.slack_name.has_value() && !uc.slack_name->empty()) {
    m_slack_label_ = *uc.slack_name;
    m_slack_pos_label_ = m_slack_label_ + "_pos";
    m_slack_neg_label_ = m_slack_label_ + "_neg";
  }
}

bool UserConstraintLP::add_to_lp(const SystemContext& sc,
                                 const ScenarioLP& scenario,
                                 const StageLP& stage,
                                 LinearProblem& lp)
{
  if (!m_expr_.has_value()) {
    return true;
  }

  // Phase/global-scoped constraints are built ONCE per (scene, phase) cell by
  // the planning passes (`add_to_phase_lp` / `add_to_global_lp`), NOT in this
  // per-(scenario, stage) operational sweep.  Skip them here so they are not
  // double-emitted.
  if (scope_is_planning(m_scope_)) {
    return true;
  }

  if (!is_active(stage)) {
    return true;
  }

  const auto& expr = *m_expr_;
  const auto& domain = expr.domain;

  // Check domain filters for this (scenario, stage)
  if (!in_range(domain.scenarios, scenario.uid())) {
    return true;
  }
  if (!in_range(domain.stages, stage.uid())) {
    return true;
  }

  const auto& uc = user_constraint();

  // Stage-scope: emit ONE LP row per (scenario, stage), keyed at the
  // stage's first in-domain block, instead of one row per block.  Per-block
  // references inside the expression resolve at that representative block;
  // a stage-level constraint typically references stage-level attributes
  // (eini/efin/capainst) — which share the same LP column across all
  // blocks — or wraps per-block variables in a `sum{b in stage}`
  // time-aggregator (piece-4 step 2).  Built here in the operational sweep
  // (not the planning pass) because it is still per-(scenario, stage).
  if (m_scope_ == ConstraintScope::Stage) {
    for (const auto& block : stage.blocks()) {
      if (!in_range(domain.blocks, block.uid())) {
        continue;
      }
      const auto row_ctx = make_stage_context(scenario.uid(), stage.uid());
      return build_coarse_row(sc, scenario, stage, block, row_ctx, lp);
    }
    return true;  // no in-domain block in this stage
  }

  // Build param map for named parameter resolution
  const auto param_map = build_param_map(sc.system().system());
  const auto mode = sc.options().constraint_mode();
  const bool is_debug = (mode == ConstraintMode::debug);
  const bool is_strict = (mode == ConstraintMode::strict || is_debug);

  // Soft-constraint sugar: when `penalty` is set on the underlying
  // UserConstraint, the LP row gets one or two auto-created slack
  // columns folded in so the constraint can be relaxed at that
  // per-unit cost.  RANGE form is not yet supported.
  //
  // ``directive`` (when present) wins over the scalar ``penalty``: the
  // typed-schema constraint family decides the soft tier so policy lives
  // in one auditable place instead of regex-detection + hardcoded
  // ladders inside the plexos2gtopt converter.  Falls back to the
  // scalar when the directive omits ``penalty`` (e.g. ``DailyBudget``
  // intentionally leaves the cost choice to the constraint author).
  const auto penalty = uc.directive.has_value()
      ? uc.directive->effective_penalty(uc.penalty).value_or(0.0)
      : uc.penalty.value_or(0.0);
  const bool is_soft = penalty > 0.0;
  if (is_soft && expr.constraint_type == ConstraintType::RANGE) {
    throw std::runtime_error(
        std::format("user_constraint '{}': `penalty` is not supported on RANGE "
                    "(`lower <= expr <= upper`) constraints — split into two "
                    "one-sided constraints instead",
                    uc.name));
  }
  const bool soft_needs_neg =
      is_soft && expr.constraint_type == ConstraintType::EQUAL;

  // Resolved slack column labels.  When the underlying ``UserConstraint``
  // ships a ``slack_name`` (set by JSON or by a PAMPL ``var slack_<NAME>;``
  // declaration) we use the per-UC label so CPLEX logs and LP dumps name
  // the slack column after the constraint instead of the generic
  // ``slack``.  Output stays aggregated at the class level — the AMPL
  // registry and the parquet writer still use the static names, so the
  // ``UserConstraint/slack_sol.parquet`` schema is unchanged.
  const std::string_view slack_col_name =
      m_slack_label_.empty() ? SlackName : std::string_view {m_slack_label_};
  const std::string_view slack_pos_col_name = m_slack_pos_label_.empty()
      ? SlackPosName
      : std::string_view {m_slack_pos_label_};
  const std::string_view slack_neg_col_name = m_slack_neg_label_.empty()
      ? SlackNegName
      : std::string_view {m_slack_neg_label_};

  // Δt-weighting flag for `daily_sum`: an "energy" scale hint makes each
  // block contribute `coeff · Δt_b · col_b` so the daily LHS is energy
  // [MWh]; "power"/"raw" leave the sum unweighted (a per-day count).
  const bool daily_energy = (m_scale_type_ == ConstraintScaleType::Energy);

  BIndexHolder<RowIndex> block_rows;
  BIndexHolder<ColIndex> block_slack_cols;
  BIndexHolder<ColIndex> block_slack_neg_cols;
  map_reserve(block_rows, stage.blocks().size());
  if (is_soft) {
    map_reserve(block_slack_cols, stage.blocks().size());
    if (soft_needs_neg) {
      map_reserve(block_slack_neg_cols, stage.blocks().size());
    }
  }

  // ── daily_sum path ───────────────────────────────────────────────────────
  // Accumulate the (optionally Δt-weighted) per-block terms into a running
  // `day_row` and flush ONE LP row per 24 h day.  A day ends when the
  // cumulative block duration crosses a 24 h boundary, or at the stage's
  // last in-domain block (blocks are chronological and never straddle a
  // day on the real data, so cumulative-duration detection is exact).  A
  // `daily_cycle`-style collapsed stage (stage.duration() > 24 representing
  // a single day) flushes once at its final block; a stage ≤ 24 h is a
  // single window = the whole stage.
  // Directive can promote a non-daily-sum constraint to the daily-sum
  // path (``DailyBudget`` always; ``MaxStartsWindow`` with
  // ``window_hours = 24``).  The underlying ``uc.daily_sum`` field is
  // not mutated — the override is purely a build-time decision.
  const bool daily_sum_effective = uc.daily_sum.value_or(false)
      || (uc.directive.has_value() && uc.directive->implies_daily_sum());

  if (daily_sum_effective) {
    constexpr double kDayHours = 24.0;

    // Index of the last in-domain block, used to force a final flush.
    std::optional<BlockUid> last_in_domain;
    for (const auto& block : stage.blocks()) {
      if (in_range(domain.blocks, block.uid())) {
        last_in_domain = block.uid();
      }
    }
    if (!last_in_domain.has_value()) {
      return true;  // nothing in-domain — no rows
    }

    // A fresh, identity-stamped accumulator row for one day window.  A
    // factory (rather than reusing a single object via reset) keeps the
    // post-flush state obviously moved-from + reassigned, so no
    // use-after-move can arise across loop iterations.
    auto fresh_day_row = [&]() -> SparseRow
    {
      SparseRow r;
      r.class_name = uc.name;
      r.constraint_name = ConstraintName;
      r.variable_uid = uid();
      return r;
    };

    SparseRow day_row = fresh_day_row();
    bool day_has_vars = false;
    double day_param_shift = 0.0;
    double day_hours = 0.0;
    BlockUid day_end_block {};  // day-ending block for this window
    bool day_open = false;

    // 0-based block ordinal within the stage (see the per-block path);
    // selects the entry of any per-block `coeff_profile` (F9).
    std::size_t block_ordinal = 0;
    for (const auto& block : stage.blocks()) {
      const std::size_t this_ordinal = block_ordinal++;
      if (!in_range(domain.blocks, block.uid())) {
        continue;
      }

      const double dt = block.duration();
      const double weight = daily_energy ? dt : 1.0;

      // Open the window on the first contributing block — its context
      // carries the day-row's metadata; the day-ending block becomes
      // the storage / dual-output key below.
      if (!day_open) {
        day_row.context =
            make_block_context(scenario.uid(), stage.uid(), block.uid());
        day_open = true;
      }
      day_end_block = block.uid();

      // Build this block's terms into a scratch row, then merge with the
      // per-block weight into the running day_row.
      SparseRow block_row;
      block_row.class_name = uc.name;
      block_row.constraint_name = ConstraintName;
      block_row.variable_uid = uid();
      block_row.context =
          make_block_context(scenario.uid(), stage.uid(), block.uid());

      LowerCtx lctx {
          .sc = sc,
          .scenario = scenario,
          .stage = stage,
          .block = block,
          .param_map = param_map,
          .uc = uc,
          .ctype = expr.constraint_type,
          .is_debug = is_debug,
          .is_strict = is_strict,
          .lp = lp,
          .block_ordinal = this_ordinal,
      };
      const auto build_res =
          build_row_from_terms(lctx, expr.terms, 1.0, block_row);
      if (build_res.has_vars) {
        day_has_vars = true;
        for (const auto& [col, coeff] : block_row.cmap) {
          day_row.cmap[col] += weight * coeff;
        }
      }
      // param_shift is folded into the day's RHS accumulation with the
      // same per-block weight as the LHS terms.
      day_param_shift += weight * build_res.param_shift;
      day_hours += dt;

      // Day-ending block: cumulative duration reaches a 24 h window, or
      // this is the stage's last in-domain block.
      const bool boundary = day_hours >= kDayHours - 1e-9;
      const bool is_last = (block.uid() == *last_in_domain);
      if (!boundary && !is_last) {
        continue;
      }

      if (!day_has_vars) {
        SPDLOG_DEBUG(
            "user_constraint '{}': no LP columns resolved for day ending "
            "at block {} — skipping",
            uc.name,
            day_end_block);
        day_row = fresh_day_row();
        day_has_vars = false;
        day_param_shift = 0.0;
        day_hours = 0.0;
        day_open = false;
        continue;
      }

      // RHS for the day-row: the per-(stage, block) override at the
      // day-ending block when present, else the expression scalar —
      // same precedence as the per-block path.
      auto adjusted_expr = expr;
      if (m_rhs_.has_value()) {
        if (const auto rhs_override = m_rhs_.optval(stage.uid(), day_end_block))
        {
          adjusted_expr.rhs = *rhs_override;
          if (adjusted_expr.lower_bound) {
            *adjusted_expr.lower_bound = *rhs_override;
          }
          if (adjusted_expr.upper_bound) {
            *adjusted_expr.upper_bound = *rhs_override;
          }
        }
      }
      adjusted_expr.rhs -= day_param_shift;
      if (adjusted_expr.lower_bound) {
        *adjusted_expr.lower_bound -= day_param_shift;
      }
      if (adjusted_expr.upper_bound) {
        *adjusted_expr.upper_bound -= day_param_shift;
      }

      // Soft-slack folding: ONE slack column per day-row (not per block),
      // keyed at the day-ending block.  The conversion mirrors the
      // per-block path but uses the whole day's duration for HydroFlow.
      if (is_soft) {
        const auto day_penalty =
            resolve_block_soft_penalty(penalty, m_penalty_class_, day_hours);
        const auto day_ctx =
            make_block_context(scenario.uid(), stage.uid(), day_end_block);
        const auto slack_col = lp.add_col(SparseCol {
            .cost = day_penalty,
            .class_name = Element::class_name.full_name(),
            .variable_name =
                soft_needs_neg ? slack_pos_col_name : slack_col_name,
            .variable_uid = uid(),
            .context = day_ctx,
        });
        double pos_coeff = 0.0;
        switch (expr.constraint_type) {
          case ConstraintType::LESS_EQUAL:
            pos_coeff = -1.0;
            break;
          case ConstraintType::GREATER_EQUAL:
          case ConstraintType::EQUAL:
            pos_coeff = +1.0;
            break;
          case ConstraintType::RANGE:
            break;  // rejected at the top of add_to_lp
        }
        day_row.cmap[slack_col] = pos_coeff;
        block_slack_cols[day_end_block] = slack_col;

        if (soft_needs_neg) {
          const auto slack_neg_col = lp.add_col(SparseCol {
              .cost = day_penalty,
              .class_name = Element::class_name.full_name(),
              .variable_name = slack_neg_col_name,
              .variable_uid = uid(),
              .context = day_ctx,
          });
          day_row.cmap[slack_neg_col] = -1.0;
          block_slack_neg_cols[day_end_block] = slack_neg_col;
        }
      }

      apply_constraint_bounds(day_row, adjusted_expr);
      const auto row_nterms = day_row.size();
      auto debug_row_name = is_debug
          ? LabelMaker {LpNamesLevel::all}.make_row_label(day_row)
          : std::string {};
      const auto row_idx = lp.add_row(std::move(day_row));
      block_rows[day_end_block] = row_idx;
      if (is_debug) {
        spdlog::info(
            "  user_constraint '{}': daily row '{}' added (idx={}, "
            "{} terms, day_hours={}, param_shift={})",
            uc.name,
            debug_row_name,
            row_idx,
            row_nterms,
            day_hours,
            day_param_shift);
      }

      // Start a fresh window.  `day_row` was just moved into `add_row`;
      // reassign it from the factory so the moved-from state never leaks
      // into the next iteration.
      day_row = fresh_day_row();
      day_has_vars = false;
      day_param_shift = 0.0;
      day_hours = 0.0;
      day_open = false;
    }

    if (!block_rows.empty()) {
      m_rows_[{scenario.uid(), stage.uid()}] = std::move(block_rows);
    }
    if (is_soft && !block_slack_cols.empty()) {
      static constexpr auto ampl_name = Element::class_name.snake_case();
      const auto st_key = std::tuple {scenario.uid(), stage.uid()};
      sc.add_ampl_variable(ampl_name,
                           uid(),
                           soft_needs_neg ? SlackPosName : SlackName,
                           scenario,
                           stage,
                           block_slack_cols);
      m_slack_cols_[st_key] = std::move(block_slack_cols);
      if (soft_needs_neg && !block_slack_neg_cols.empty()) {
        sc.add_ampl_variable(ampl_name,
                             uid(),
                             SlackNegName,
                             scenario,
                             stage,
                             block_slack_neg_cols);
        m_slack_neg_cols_[st_key] = std::move(block_slack_neg_cols);
      }
    }
    return true;
  }

  // 0-based ordinal of the current block within the stage's full block
  // list (incremented for every block, not just in-domain ones) so a
  // per-block `coeff_profile` indexes consistently with the stage's
  // chronological block order — matching the positional `rhs [...]`
  // schedule semantics.
  std::size_t block_ordinal = 0;
  for (const auto& block : stage.blocks()) {
    const std::size_t this_ordinal = block_ordinal++;
    if (!in_range(domain.blocks, block.uid())) {
      continue;
    }

    SparseRow row;
    row.class_name = uc.name;
    row.constraint_name = ConstraintName;
    // Labelled rows / auxiliary cols inheriting this row's uid must
    // carry a concrete `variable_uid` — `add_aux_col` propagates
    // `row.variable_uid` to each `abs_aux` / `max_aux` / `min_aux`
    // column, and an `unknown_uid = -1` there serialises as
    // `UserConstraint_abs_aux_-1_…`, rejected by
    // `LinearProblem::add_col`'s validator (post-2026-04-23) and by
    // CoinLpIO's LP-writer name check.
    row.variable_uid = uid();
    row.context = make_block_context(scenario.uid(), stage.uid(), block.uid());

    LowerCtx lctx {
        .sc = sc,
        .scenario = scenario,
        .stage = stage,
        .block = block,
        .param_map = param_map,
        .uc = uc,
        .ctype = expr.constraint_type,
        .is_debug = is_debug,
        .is_strict = is_strict,
        .lp = lp,
        .block_ordinal = this_ordinal,
    };

    const auto build_res = build_row_from_terms(lctx, expr.terms, 1.0, row);
    const bool has_vars = build_res.has_vars;
    const double param_shift = build_res.param_shift;

    if (!has_vars) {
      SPDLOG_DEBUG(
          "user_constraint '{}': no LP columns resolved "
          "for block {} — skipping",
          uc.name,
          block.uid());
      continue;
    }

    // Move parameter terms to the RHS: vars + params [op] rhs
    // becomes vars [op] rhs - params
    auto adjusted_expr = expr;
    // ``UserConstraint.rhs`` (TB-schedule field on the source object,
    // stored in ``m_rhs_``) overrides the scalar parsed from the inline
    // ``<op> NUMBER`` at the tail of ``expression``.  When the schedule
    // returns ``std::nullopt`` for the current (stage, block) — or when
    // the source ``UserConstraint`` left ``rhs`` unset entirely — we
    // keep the expression's scalar.  Scalar JSON RHS therefore stays
    // backward-compatible (a ``double`` constant on the schedule is
    // simply a per-cell value matching the expression's scalar).
    if (m_rhs_.has_value()) {
      if (const auto rhs_override = m_rhs_.optval(stage.uid(), block.uid())) {
        adjusted_expr.rhs = *rhs_override;
        if (adjusted_expr.lower_bound) {
          *adjusted_expr.lower_bound = *rhs_override;
        }
        if (adjusted_expr.upper_bound) {
          *adjusted_expr.upper_bound = *rhs_override;
        }
      }
    }
    adjusted_expr.rhs -= param_shift;
    if (adjusted_expr.lower_bound) {
      *adjusted_expr.lower_bound -= param_shift;
    }
    if (adjusted_expr.upper_bound) {
      *adjusted_expr.upper_bound -= param_shift;
    }

    // Soft-constraint relaxation — fold one or two slack columns into
    // the row before sealing it.  Each slack carries `cost = block_penalty`,
    // is non-negative, and is registered through `add_ampl_variable`
    // so other constraints / reports can reference it by the canonical
    // names `slack`, `slack_pos`, `slack_neg`.
    //
    // `block_penalty` is resolved inside the block loop (not hoisted)
    // because `PenaltyClass::HydroFlow` converts $/m³ → $/(m³/s) via
    // `× duration[h] × 3600`, and block duration is per-block.
    if (is_soft) {
      const auto block_penalty = resolve_block_soft_penalty(
          penalty, m_penalty_class_, block.duration());
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), block.uid());
      const auto slack_col = lp.add_col(SparseCol {
          .cost = block_penalty,
          .class_name = Element::class_name.full_name(),
          .variable_name = soft_needs_neg ? slack_pos_col_name : slack_col_name,
          .variable_uid = uid(),
          .context = block_ctx,
      });
      // LE: row gets `−1·slack` so the optimizer can absorb overage.
      // GE: row gets `+1·slack` so the optimizer can absorb shortfall.
      // EQ (slack_pos): row gets `+1·slack_pos`, paired with slack_neg.
      double pos_coeff = 0.0;
      switch (expr.constraint_type) {
        case ConstraintType::LESS_EQUAL:
          pos_coeff = -1.0;
          break;
        case ConstraintType::GREATER_EQUAL:
        case ConstraintType::EQUAL:
          pos_coeff = +1.0;
          break;
        case ConstraintType::RANGE:
          // Already rejected at the top of add_to_lp.
          break;
      }
      row.cmap[slack_col] = pos_coeff;
      block_slack_cols[block.uid()] = slack_col;

      if (soft_needs_neg) {
        const auto slack_neg_col = lp.add_col(SparseCol {
            .cost = block_penalty,
            .class_name = Element::class_name.full_name(),
            .variable_name = slack_neg_col_name,
            .variable_uid = uid(),
            .context = block_ctx,
        });
        row.cmap[slack_neg_col] = -1.0;
        block_slack_neg_cols[block.uid()] = slack_neg_col;
      }
    }

    apply_constraint_bounds(row, adjusted_expr);
    const auto row_nterms = row.size();
    // Compute row_name lazily only when debug logging is enabled — otherwise
    // we pay the label formatting cost for a string that is immediately
    // dropped.  Using LpNamesLevel::all ensures the debug line is
    // always populated even when solver row labels are disabled.
    auto debug_row_name = is_debug
        ? LabelMaker {LpNamesLevel::all}.make_row_label(row)
        : std::string {};
    const auto row_idx = lp.add_row(std::move(row));
    block_rows[block.uid()] = row_idx;
    if (is_debug) {
      spdlog::info(
          "  user_constraint '{}': row '{}' added (idx={}, "
          "{} terms, param_shift={})",
          uc.name,
          debug_row_name,
          row_idx,
          row_nterms,
          param_shift);
    }
  }

  if (!block_rows.empty()) {
    const auto n_rows = block_rows.size();
    m_rows_[{scenario.uid(), stage.uid()}] = std::move(block_rows);
    if (is_debug) {
      spdlog::info("user_constraint '{}': {} rows for s{}_t{}",
                   uc.name,
                   n_rows,
                   scenario.uid(),
                   stage.uid());
    }
  }

  // Publish auto-created slack columns to the AMPL registry so other
  // user constraints / reports can reference them, and stash the per-
  // (scenario, stage) holders for `add_to_output`.
  if (is_soft && !block_slack_cols.empty()) {
    static constexpr auto ampl_name = Element::class_name.snake_case();
    const auto st_key = std::tuple {scenario.uid(), stage.uid()};
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         soft_needs_neg ? SlackPosName : SlackName,
                         scenario,
                         stage,
                         block_slack_cols);
    m_slack_cols_[st_key] = std::move(block_slack_cols);

    if (soft_needs_neg && !block_slack_neg_cols.empty()) {
      sc.add_ampl_variable(ampl_name,
                           uid(),
                           SlackNegName,
                           scenario,
                           stage,
                           block_slack_neg_cols);
      m_slack_neg_cols_[st_key] = std::move(block_slack_neg_cols);
    }
  }

  return true;
}

bool UserConstraintLP::build_coarse_row(const SystemContext& sc,
                                        const ScenarioLP& scenario,
                                        const StageLP& stage,
                                        const BlockLP& block,
                                        const LpContext& row_ctx,
                                        LinearProblem& lp)
{
  // Shared builder for the COARSE-scope paths (`stage`/`phase`/`global`).
  // It resolves the (already domain-checked) expression at the supplied
  // representative (scenario, stage, block), folds in the RHS override and
  // an optional soft slack, stamps the row with @p row_ctx (so a `stage`
  // row carries `StageContext` and a `phase`/`global` row carries
  // `PhaseContext` — never colliding with per-block rows nor with each
  // other in `LinearProblem::add_row`'s metadata dedup), and records it
  // under the (scenario, stage) key at @p block so `add_to_output` reads it
  // uniformly with the per-block path.  Per-block element references inside
  // the expression resolve at the representative block; per-block variables
  // that must be time-aggregated are wrapped in a `sum{...}` time-aggregator
  // (piece-4 step 2).
  if (!m_expr_.has_value()) {
    return true;
  }
  const auto& expr = *m_expr_;
  const auto& uc = user_constraint();

  const auto param_map = build_param_map(sc.system().system());
  const auto mode = sc.options().constraint_mode();
  const bool is_debug = (mode == ConstraintMode::debug);
  const bool is_strict = (mode == ConstraintMode::strict || is_debug);

  const auto penalty = uc.directive.has_value()
      ? uc.directive->effective_penalty(uc.penalty).value_or(0.0)
      : uc.penalty.value_or(0.0);
  const bool is_soft = penalty > 0.0;
  if (is_soft && expr.constraint_type == ConstraintType::RANGE) {
    throw std::runtime_error(
        std::format("user_constraint '{}': `penalty` is not supported on RANGE "
                    "(`lower <= expr <= upper`) constraints — split into two "
                    "one-sided constraints instead",
                    uc.name));
  }
  const bool soft_needs_neg =
      is_soft && expr.constraint_type == ConstraintType::EQUAL;

  const std::string_view slack_col_name =
      m_slack_label_.empty() ? SlackName : std::string_view {m_slack_label_};
  const std::string_view slack_pos_col_name = m_slack_pos_label_.empty()
      ? SlackPosName
      : std::string_view {m_slack_pos_label_};
  const std::string_view slack_neg_col_name = m_slack_neg_label_.empty()
      ? SlackNegName
      : std::string_view {m_slack_neg_label_};

  SparseRow row;
  row.class_name = uc.name;
  row.constraint_name = ConstraintName;
  row.variable_uid = uid();
  row.context = row_ctx;

  LowerCtx lctx {
      .sc = sc,
      .scenario = scenario,
      .stage = stage,
      .block = block,
      .param_map = param_map,
      .uc = uc,
      .ctype = expr.constraint_type,
      .is_debug = is_debug,
      .is_strict = is_strict,
      .lp = lp,
      .block_ordinal = 0,
  };

  const auto build_res = build_row_from_terms(lctx, expr.terms, 1.0, row);
  if (!build_res.has_vars) {
    SPDLOG_DEBUG(
        "user_constraint '{}': no LP columns resolved for coarse-scope row "
        "(rep block {}) — skipping",
        uc.name,
        block.uid());
    return true;
  }

  // Move parameter terms to the RHS, honouring the per-(stage, block)
  // override at the representative block when present.
  auto adjusted_expr = expr;
  if (m_rhs_.has_value()) {
    if (const auto rhs_override = m_rhs_.optval(stage.uid(), block.uid())) {
      adjusted_expr.rhs = *rhs_override;
      if (adjusted_expr.lower_bound) {
        *adjusted_expr.lower_bound = *rhs_override;
      }
      if (adjusted_expr.upper_bound) {
        *adjusted_expr.upper_bound = *rhs_override;
      }
    }
  }
  adjusted_expr.rhs -= build_res.param_shift;
  if (adjusted_expr.lower_bound) {
    *adjusted_expr.lower_bound -= build_res.param_shift;
  }
  if (adjusted_expr.upper_bound) {
    *adjusted_expr.upper_bound -= build_res.param_shift;
  }

  BIndexHolder<ColIndex> block_slack_cols;
  BIndexHolder<ColIndex> block_slack_neg_cols;

  if (is_soft) {
    const auto block_penalty =
        resolve_block_soft_penalty(penalty, m_penalty_class_, block.duration());
    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto slack_col = lp.add_col(SparseCol {
        .cost = block_penalty,
        .class_name = Element::class_name.full_name(),
        .variable_name = soft_needs_neg ? slack_pos_col_name : slack_col_name,
        .variable_uid = uid(),
        .context = block_ctx,
    });
    double pos_coeff = 0.0;
    switch (expr.constraint_type) {
      case ConstraintType::LESS_EQUAL:
        pos_coeff = -1.0;
        break;
      case ConstraintType::GREATER_EQUAL:
      case ConstraintType::EQUAL:
        pos_coeff = +1.0;
        break;
      case ConstraintType::RANGE:
        break;  // rejected above
    }
    row.cmap[slack_col] = pos_coeff;
    block_slack_cols[block.uid()] = slack_col;

    if (soft_needs_neg) {
      const auto slack_neg_col = lp.add_col(SparseCol {
          .cost = block_penalty,
          .class_name = Element::class_name.full_name(),
          .variable_name = slack_neg_col_name,
          .variable_uid = uid(),
          .context = block_ctx,
      });
      row.cmap[slack_neg_col] = -1.0;
      block_slack_neg_cols[block.uid()] = slack_neg_col;
    }
  }

  apply_constraint_bounds(row, adjusted_expr);
  const auto row_idx = lp.add_row(std::move(row));

  BIndexHolder<RowIndex> block_rows;
  block_rows[block.uid()] = row_idx;
  m_rows_[{scenario.uid(), stage.uid()}] = std::move(block_rows);

  if (is_soft && !block_slack_cols.empty()) {
    static constexpr auto ampl_name = Element::class_name.snake_case();
    const auto st_key = std::tuple {scenario.uid(), stage.uid()};
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         soft_needs_neg ? SlackPosName : SlackName,
                         scenario,
                         stage,
                         block_slack_cols);
    m_slack_cols_[st_key] = std::move(block_slack_cols);
    if (soft_needs_neg && !block_slack_neg_cols.empty()) {
      sc.add_ampl_variable(ampl_name,
                           uid(),
                           SlackNegName,
                           scenario,
                           stage,
                           block_slack_neg_cols);
      m_slack_neg_cols_[st_key] = std::move(block_slack_neg_cols);
    }
  }

  return true;
}

bool UserConstraintLP::add_to_phase_lp(const SystemContext& sc,
                                       const SceneLP& scene,
                                       const PhaseLP& phase,
                                       LinearProblem& lp)
{
  // Only `phase`-scope is built here; `global` routes through
  // `add_to_global_lp`.  Everything else (block/stage) was already built in
  // the per-(scenario, stage) operational sweep (`add_to_lp`).
  if (m_scope_ != ConstraintScope::Phase) {
    return true;
  }
  return build_phase_cell_row(sc, scene, phase, lp);
}

bool UserConstraintLP::add_to_global_lp(const SystemContext& sc,
                                        const SceneLP& scene,
                                        const PhaseLP& phase,
                                        LinearProblem& lp)
{
  // Only `global`-scope is built here.  Runs in the global sweep, which the
  // planning pass executes after the phase sweep so a global row can
  // reference state columns the phase sweep registered.
  if (m_scope_ != ConstraintScope::Global) {
    return true;
  }
  return build_phase_cell_row(sc, scene, phase, lp);
}

bool UserConstraintLP::build_phase_cell_row(const SystemContext& sc,
                                            const SceneLP& scene,
                                            const PhaseLP& phase,
                                            LinearProblem& lp)
{
  // Planning-level (phase/global) rows carry a `PhaseContext` stamped with
  // the cell's (scene, phase) UIDs plus this element's own uid as the
  // discriminator, so distinct global rows in the same cell never collide in
  // `LinearProblem::add_row`'s metadata dedup.
  const LpContext row_ctx = make_phase_context(scene.uid(), phase.uid(), uid());
  // Build ONE LP row per (scene, phase) cell for `phase`/`global`-scoped
  // constraints.  The representative point is the cell's LAST stage / LAST
  // block under the FIRST in-domain scenario — matching the FCF terminal-cut
  // shape (a single row anchored at the end of the horizon on a scalar
  // quantity).  Per-block references resolve at that terminal block; coarse
  // rows over per-block variables wrap them in `sum{...}` (piece-4 step 2).
  if (!m_expr_.has_value()) {
    return true;
  }
  const auto& expr = *m_expr_;
  const auto& domain = expr.domain;

  const auto& stages = phase.stages();
  if (stages.empty()) {
    return true;
  }
  // Last in-domain stage with at least one in-domain block.
  for (const auto& stage : std::ranges::reverse_view(stages)) {
    if (!is_active(stage) || !in_range(domain.stages, stage.uid())) {
      continue;
    }
    const BlockLP* rep_block = nullptr;
    for (const auto& block : std::ranges::reverse_view(stage.blocks())) {
      if (in_range(domain.blocks, block.uid())) {
        rep_block = &block;
        break;
      }
    }
    if (rep_block == nullptr) {
      continue;
    }
    for (const auto& scenario : scene.scenarios()) {
      if (!in_range(domain.scenarios, scenario.uid())) {
        continue;
      }
      // One row for the whole cell: first in-domain scenario wins.
      return build_coarse_row(sc, scenario, stage, *rep_block, row_ctx, lp);
    }
  }
  return true;  // nothing in-domain in this cell
}

bool UserConstraintLP::add_to_output(OutputContext& out) const
{
  if (m_rows_.empty()) {
    return true;
  }

  const Id pid {uid(), user_constraint().name};

  if (m_scale_type_ == ConstraintScaleType::Raw) {
    // Raw / unitless constraints: scale by discount factor only
    // (scale_obj / discount[t]), no probability and no block duration.
    out.add_row_dual_raw(
        Element::class_name.full_name(), ConstraintName, pid, m_rows_);
  } else {
    // Power and Energy constraints: standard block_cost_factors scaling
    // (scale_obj / (prob × discount × Δt)).
    // "power"  → dual in $/MW;  "energy" → dual in $/MWh.
    out.add_row_dual(
        Element::class_name.full_name(), ConstraintName, pid, m_rows_);
  }

  // Auto-created slack columns from soft-constraint sugar.  The
  // visible-slack contract requires both the primal value and the
  // realized cost to land in the standard output stream.
  const auto slack_name = m_slack_neg_cols_.empty() ? SlackName : SlackPosName;
  if (!m_slack_cols_.empty()) {
    out.add_col_sol(
        Element::class_name.full_name(), slack_name, pid, m_slack_cols_);
    out.add_col_cost(
        Element::class_name.full_name(), slack_name, pid, m_slack_cols_);
  }
  if (!m_slack_neg_cols_.empty()) {
    out.add_col_sol(
        Element::class_name.full_name(), SlackNegName, pid, m_slack_neg_cols_);
    out.add_col_cost(
        Element::class_name.full_name(), SlackNegName, pid, m_slack_neg_cols_);
  }

  return true;
}

}  // namespace gtopt
