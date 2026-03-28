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
#include <gtopt/system_context.hpp>
#include <gtopt/user_constraint.hpp>
#include <gtopt/user_constraint_lp.hpp>
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
    , m_scale_type_(
          parse_constraint_scale_type(uc.constraint_type.value_or("power")))
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
    for (const auto& term : expr.terms) {
      if (term.element) {
        if (auto resolved = resolve_single_col(
                sc, scenario, stage, block, *term.element, lp))
        {
          row[resolved->col] += term.coefficient * resolved->scale;
          has_vars = true;
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

    apply_constraint_bounds(row, expr);
    const auto row_idx = lp.add_row(std::move(row));
    block_rows[block.uid()] = row_idx;
  }

  if (!block_rows.empty()) {
    m_rows_[{scenario.uid(), stage.uid()}] = std::move(block_rows);
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
