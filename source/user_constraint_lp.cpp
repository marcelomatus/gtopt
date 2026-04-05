/**
 * @file      user_constraint_lp.cpp
 * @brief     LP element implementation for user-defined constraints
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <format>

#include <gtopt/constraint_expr.hpp>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/element_column_resolver.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/planning_enums.hpp>
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

[[nodiscard]] bool in_range(const IndexRange& range, int uid)
{
  if (range.is_all) {
    return true;
  }
  return std::ranges::contains(range.values, uid);
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
  if (!in_range(domain.scenarios, static_cast<int>(scenario.uid()))) {
    return true;
  }
  if (!in_range(domain.stages, static_cast<int>(stage.uid()))) {
    return true;
  }

  const auto& uc = user_constraint();

  // Build param map for named parameter resolution
  const auto param_map = build_param_map(sc.system().system());
  const auto mode = sc.options().constraint_mode();
  const bool is_debug = (mode == ConstraintMode::debug);
  const bool is_strict = (mode == ConstraintMode::strict || is_debug);

  BIndexHolder<RowIndex> block_rows;
  map_reserve(block_rows, stage.blocks().size());

  for (const auto& block : stage.blocks()) {
    if (!in_range(domain.blocks, static_cast<int>(block.uid()))) {
      continue;
    }

    SparseRow row;
    row.name = std::format("{}_s{}_t{}_b{}",
                           uc.name,
                           static_cast<int>(scenario.uid()),
                           static_cast<int>(stage.uid()),
                           static_cast<int>(block.uid()));

    bool has_vars = false;
    double param_shift = 0.0;
    for (const auto& term : expr.terms) {
      if (term.element) {
        // Try LP variable first, then fall back to data parameter
        if (auto resolved = resolve_single_col(
                sc, scenario, stage, block, *term.element, lp))
        {
          row[resolved->col] += term.coefficient;
          has_vars = true;
          if (is_debug) {
            spdlog::info(
                "  user_constraint '{}' block {}: "
                "col {} coeff={} scale={}",
                uc.name,
                static_cast<int>(block.uid()),
                static_cast<int>(resolved->col),
                term.coefficient,
                resolved->scale);
          }
        } else if (auto pval = resolve_single_param(
                       sc, scenario, stage, block, *term.element))
        {
          // Parameter term: accumulate coeff × value for RHS adjustment
          param_shift += term.coefficient * *pval;
          if (is_debug) {
            spdlog::info(
                "  user_constraint '{}' block {}: "
                "data param value={} coeff={}",
                uc.name,
                static_cast<int>(block.uid()),
                *pval,
                term.coefficient);
          }
        }
      } else if (term.sum_ref) {
        const std::size_t before = row.size();
        collect_sum_cols(sc,
                         scenario,
                         stage,
                         block,
                         *term.sum_ref,
                         term.coefficient,
                         row,
                         lp);
        if (row.size() > before) {
          has_vars = true;
          if (is_debug) {
            spdlog::info(
                "  user_constraint '{}' block {}: "
                "sum resolved {} columns",
                uc.name,
                static_cast<int>(block.uid()),
                row.size() - before);
          }
        }
      } else if (term.param_name) {
        // Named parameter reference
        if (auto pval = resolve_param(param_map, *term.param_name, stage)) {
          param_shift += term.coefficient * *pval;
          if (is_debug) {
            spdlog::info(
                "  user_constraint '{}' block {}: "
                "param '{}' = {} coeff={}",
                uc.name,
                static_cast<int>(block.uid()),
                *term.param_name,
                *pval,
                term.coefficient);
          }
        } else if (is_strict) {
          throw std::runtime_error(
              std::format("user_constraint '{}': unknown parameter '{}'",
                          uc.name,
                          *term.param_name));
        } else {
          SPDLOG_WARN(std::format(
              "user_constraint '{}': unknown parameter '{}' — skipping term",
              uc.name,
              *term.param_name));
        }
      }
    }

    if (!has_vars) {
      SPDLOG_DEBUG(
          std::format("user_constraint '{}': no LP columns resolved "
                      "for block {} — skipping",
                      uc.name,
                      static_cast<int>(block.uid())));
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
    apply_constraint_bounds(row, adjusted_expr);
    const auto row_name = row.name;
    const auto row_nterms = row.size();
    const auto row_idx = lp.add_row(std::move(row));
    block_rows[block.uid()] = row_idx;
    if (is_debug) {
      spdlog::info(
          "  user_constraint '{}': row '{}' added (idx={}, "
          "{} terms, param_shift={})",
          uc.name,
          row_name,
          static_cast<int>(row_idx),
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
                   static_cast<int>(scenario.uid()),
                   static_cast<int>(stage.uid()));
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
    out.add_row_dual_raw(ClassName.full_name(), "constraint", pid, m_rows_);
  } else {
    // Power and Energy constraints: standard block_cost_factors scaling
    // (scale_obj / (prob × discount × Δt)).
    // "power"  → dual in $/MW;  "energy" → dual in $/MWh.
    out.add_row_dual(ClassName.full_name(), "constraint", pid, m_rows_);
  }

  return true;
}

}  // namespace gtopt
