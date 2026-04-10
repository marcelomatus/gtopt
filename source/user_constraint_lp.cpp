/**
 * @file      user_constraint_lp.cpp
 * @brief     LP element implementation for user-defined constraints
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <ranges>

#include <gtopt/constraint_expr.hpp>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/element_column_resolver.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/planning_enums.hpp>
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
  requires(strong::is_strong_type<T>::value)
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
    if (const auto month = stage.month()) {
      // MonthType is 1-based (january=1), monthly vector is 0-based
      const auto idx = static_cast<std::size_t>(*month) - 1;
      if (idx < monthly.size()) {
        return monthly[idx];
      }
    }
    // No month on stage: use january (index 0) as fallback
    return monthly.empty() ? 0.0 : monthly[0];
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

// Fresh SparseRow that inherits identity/context from `row`.
[[nodiscard]] SparseRow make_aux_row(const SparseRow& row)
{
  SparseRow aux;
  aux.class_name = row.class_name;
  aux.constraint_name = row.constraint_name;
  aux.variable_uid = row.variable_uid;
  aux.context = row.context;
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
  col.class_name = UserConstraintLP::ClassName.full_name();
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

// Recursive row-builder.  Handles element / sum / param terms directly
// and lowers `abs`, `min`/`max`, `if` wrappers into auxiliary columns
// and helper rows on the fly.
// NOLINTNEXTLINE(misc-no-recursion)
BuildResult build_row_from_terms(LowerCtx& ctx,
                                 const std::vector<ConstraintTerm>& terms,
                                 double outer_coef,
                                 SparseRow& row)
{
  BuildResult out;

  for (const auto& term : terms) {
    const double coef = outer_coef * term.coefficient;

    if (term.element) {
      const std::size_t before_col = row.size();
      if (resolve_col_to_row(ctx.sc,
                             ctx.scenario,
                             ctx.stage,
                             ctx.block,
                             *term.element,
                             coef,
                             row,
                             ctx.lp))
      {
        out.has_vars = true;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "element '{}.{}' added {} col(s), coeff={}",
              ctx.uc.name,
              ctx.block.uid(),
              term.element->element_type,
              term.element->attribute,
              row.size() - before_col,
              coef);
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
      } else if (ctx.is_strict) {
        // The element ref resolved neither as an LP column (no
        // matching variable in the AMPL registry) nor as a data
        // parameter — most commonly a typo, an inactive element, or
        // a missing element.  Silent skipping here is the most
        // insidious failure mode: the constraint becomes vacuously
        // satisfied while the LP still solves.
        throw std::runtime_error(std::format(
            "user_constraint '{}': cannot resolve element reference "
            "'{}({}).{}' (block {}) — element is missing or inactive, "
            "or attribute is not a registered LP variable or parameter",
            ctx.uc.name,
            term.element->element_type,
            term.element->element_id,
            term.element->attribute,
            ctx.block.uid()));
      } else {
        SPDLOG_WARN(std::format(
            "user_constraint '{}': cannot resolve element reference "
            "'{}({}).{}' (block {}) — skipping term",
            ctx.uc.name,
            term.element->element_type,
            term.element->element_id,
            term.element->attribute,
            ctx.block.uid()));
      }
      continue;
    }

    if (term.sum_ref) {
      const std::size_t before = row.size();
      collect_sum_cols(ctx.sc,
                       ctx.scenario,
                       ctx.stage,
                       ctx.block,
                       *term.sum_ref,
                       coef,
                       row,
                       ctx.lp);
      if (row.size() > before) {
        out.has_vars = true;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "sum resolved {} columns",
              ctx.uc.name,
              ctx.block.uid(),
              row.size() - before);
        }
      }
      continue;
    }

    if (term.param_name) {
      if (auto pval = resolve_param(ctx.param_map, *term.param_name, ctx.stage))
      {
        out.param_shift += coef * *pval;
        if (ctx.is_debug) {
          spdlog::info(
              "  user_constraint '{}' block {}: "
              "param '{}' = {} coeff={}",
              ctx.uc.name,
              ctx.block.uid(),
              *term.param_name,
              *pval,
              coef);
        }
      } else if (ctx.is_strict) {
        throw std::runtime_error(
            std::format("user_constraint '{}': unknown parameter '{}'",
                        ctx.uc.name,
                        *term.param_name));
      } else {
        SPDLOG_WARN(std::format(
            "user_constraint '{}': unknown parameter '{}' — skipping term",
            ctx.uc.name,
            *term.param_name));
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

      // Resolve inner linear expression into a local row.
      SparseRow inner_row = make_aux_row(row);
      auto inner_res =
          build_row_from_terms(ctx, term.abs_expr->inner, 1.0, inner_row);

      if (inner_row.cmap.empty()) {
        // Inner reduced to a pure constant — contribute |c| directly.
        out.param_shift += coef * std::abs(inner_res.param_shift);
        continue;
      }

      // NOLINTNEXTLINE(fuchsia-default-arguments-calls)
      const auto t_idx = add_aux_col(ctx, row, "abs_aux", AuxSign::NonNegative);

      // Row 1: inner_vars − t ≤ −inner_shift
      SparseRow r1 = make_aux_row(row);
      r1.cmap = inner_row.cmap;
      r1.cmap[t_idx] = -1.0;
      r1.less_equal(-inner_res.param_shift);
      (void)ctx.lp.add_row(std::move(r1));

      // Row 2: −inner_vars − t ≤ inner_shift
      SparseRow r2 = make_aux_row(row);
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

        SparseRow ar = make_aux_row(row);
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

UserConstraintLP::UserConstraintLP(const UserConstraint& uc, InputContext& ic)
    : ObjectLP<UserConstraint>(uc, ic, ClassName)
    , m_scale_type_(enum_from_name<ConstraintScaleType>(
                        uc.constraint_type.value_or("power"))
                        .value_or(ConstraintScaleType::Power))
{
  if (!uc.expression.empty()) {
    try {
      m_expr_ = ConstraintParser::parse(uc.name, uc.expression);
    } catch (const std::exception& ex) {
      SPDLOG_ERROR(std::format(
          "user_constraint '{}': expression parse error: {} — "
          "check expression syntax and refer to the user-constraint "
          "documentation; constraint will be silently skipped",
          uc.name,
          ex.what()));
      // m_expr_ stays nullopt; add_to_lp will skip this constraint silently
    }
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

  // Build param map for named parameter resolution
  const auto param_map = build_param_map(sc.system().system());
  const auto mode = sc.options().constraint_mode();
  const bool is_debug = (mode == ConstraintMode::debug);
  const bool is_strict = (mode == ConstraintMode::strict || is_debug);

  // Soft-constraint sugar: when `penalty` is set on the underlying
  // UserConstraint, the LP row gets one or two auto-created slack
  // columns folded in so the constraint can be relaxed at that
  // per-unit cost.  RANGE form is not yet supported.
  const auto penalty = uc.penalty.value_or(0.0);
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

  for (const auto& block : stage.blocks()) {
    if (!in_range(domain.blocks, block.uid())) {
      continue;
    }

    SparseRow row;
    row.class_name = uc.name;
    row.constraint_name = ConstraintName;
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
    };

    const auto build_res = build_row_from_terms(lctx, expr.terms, 1.0, row);
    const bool has_vars = build_res.has_vars;
    const double param_shift = build_res.param_shift;

    if (!has_vars) {
      SPDLOG_DEBUG(
          std::format("user_constraint '{}': no LP columns resolved "
                      "for block {} — skipping",
                      uc.name,
                      block.uid()));
      continue;
    }

    // Move parameter terms to the RHS: vars + params [op] rhs
    // becomes vars [op] rhs - params
    auto adjusted_expr = expr;
    adjusted_expr.rhs -= param_shift;
    if (adjusted_expr.lower_bound) {
      *adjusted_expr.lower_bound -= param_shift;
    }
    if (adjusted_expr.upper_bound) {
      *adjusted_expr.upper_bound -= param_shift;
    }

    // Soft-constraint relaxation — fold one or two slack columns into
    // the row before sealing it.  Each slack carries `cost = penalty`,
    // is non-negative, and is registered through `add_ampl_variable`
    // so other constraints / reports can reference it by the canonical
    // names `slack`, `slack_pos`, `slack_neg`.
    if (is_soft) {
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), block.uid());
      const auto slack_col = lp.add_col(SparseCol {
          .cost = penalty,
          .class_name = ClassName.full_name(),
          .variable_name = soft_needs_neg ? SlackPosName : SlackName,
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
          pos_coeff = +1.0;
          break;
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
            .cost = penalty,
            .class_name = ClassName.full_name(),
            .variable_name = SlackNegName,
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
    // dropped.  force_row_label ignores LpNamesLevel so the debug line is
    // always populated even when solver row labels are disabled.
    auto debug_row_name =
        is_debug ? LabelMaker::force_row_label(row) : std::string {};
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
    static const auto ampl_name = std::string {ClassName.snake_case()};
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

bool UserConstraintLP::add_to_output(OutputContext& out) const
{
  if (m_rows_.empty()) {
    return true;
  }

  const Id pid {uid(), user_constraint().name};

  if (m_scale_type_ == ConstraintScaleType::Raw) {
    // Raw / unitless constraints: scale by discount factor only
    // (scale_obj / discount[t]), no probability and no block duration.
    out.add_row_dual_raw(ClassName.full_name(), ConstraintName, pid, m_rows_);
  } else {
    // Power and Energy constraints: standard block_cost_factors scaling
    // (scale_obj / (prob × discount × Δt)).
    // "power"  → dual in $/MW;  "energy" → dual in $/MWh.
    out.add_row_dual(ClassName.full_name(), ConstraintName, pid, m_rows_);
  }

  // Auto-created slack columns from soft-constraint sugar.  The
  // visible-slack contract requires both the primal value and the
  // realized cost to land in the standard output stream.
  const auto slack_name = m_slack_neg_cols_.empty() ? SlackName : SlackPosName;
  if (!m_slack_cols_.empty()) {
    out.add_col_sol(ClassName.full_name(), slack_name, pid, m_slack_cols_);
    out.add_col_cost(ClassName.full_name(), slack_name, pid, m_slack_cols_);
  }
  if (!m_slack_neg_cols_.empty()) {
    out.add_col_sol(
        ClassName.full_name(), SlackNegName, pid, m_slack_neg_cols_);
    out.add_col_cost(
        ClassName.full_name(), SlackNegName, pid, m_slack_neg_cols_);
  }

  return true;
}

}  // namespace gtopt
