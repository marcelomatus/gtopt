/**
 * @file      benders_cut.cpp
 * @brief     Modular Benders cut construction and handling – implementation
 * @date      2026-03-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the cut-creation building blocks declared in benders_cut.hpp.
 * The free functions moved here were originally part of sddp_solver.cpp.
 * The BendersCut class implementation is also here.
 */

#include <cmath>
#include <format>
#include <span>

#include <gtopt/benders_cut.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/work_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Optimality cut ─────────────────────────────────────────────────────────

void propagate_trial_values(std::span<StateVarLink> links,
                            std::span<const double> source_solution,
                            LinearInterface& target_li) noexcept
{
  for (auto& link : links) {
    link.trial_value = source_solution[link.source_col];
    target_li.set_col_low(link.dependent_col, link.trial_value);
    target_li.set_col_upp(link.dependent_col, link.trial_value);
  }
}

void propagate_trial_values_row_dual(std::span<StateVarLink> links,
                                     std::span<const double> source_solution,
                                     LinearInterface& target_li) noexcept
{
  for (auto& link : links) {
    link.trial_value = source_solution[link.source_col];

    // Keep the column at its physical bounds (not fixed).
    target_li.set_col_low(link.dependent_col, link.source_low);
    target_li.set_col_upp(link.dependent_col, link.source_upp);

    if (link.coupling_row != RowIndex {unknown_index}) {
      // Reuse existing coupling row — just update the RHS.
      target_li.set_row_low(link.coupling_row, link.trial_value);
      target_li.set_row_upp(link.coupling_row, link.trial_value);

      SPDLOG_TRACE("row_dual propagation: col {} = {:.4f} (updated row {})",
                   link.dependent_col,
                   link.trial_value,
                   link.coupling_row);
    } else {
      // First call: add explicit coupling constraint x_dep = trial_value.
      auto coupling = SparseRow {
          .name = {},
          .lowb = link.trial_value,
          .uppb = link.trial_value,
      };
      coupling[link.dependent_col] = 1.0;

      link.coupling_row = target_li.add_row(coupling);

      SPDLOG_TRACE(
          "row_dual propagation: col {} = {:.4f} via new coupling row {}",
          link.dependent_col,
          link.trial_value,
          link.coupling_row);
    }
  }
}

auto build_benders_cut(ColIndex alpha_col,
                       std::span<const StateVarLink> links,
                       std::span<const double> reduced_costs,
                       double objective_value,
                       std::string_view name,
                       double scale_alpha,
                       double cut_coeff_eps) -> SparseRow
{
  auto row = SparseRow {
      .name = std::string(name),
      .lowb = objective_value,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = scale_alpha;

  for (const auto& link : links) {
    const auto rc = reduced_costs[link.dependent_col];
    if (std::abs(rc) < cut_coeff_eps) {
      continue;
    }
    row[link.source_col] = -rc;
    row.lowb -= rc * link.trial_value;
  }

  return row;
}

auto build_benders_cut_from_row_duals(ColIndex alpha_col,
                                      std::span<const StateVarLink> links,
                                      std::span<const double> row_duals,
                                      double objective_value,
                                      std::string_view name,
                                      double scale_alpha,
                                      double cut_coeff_eps) -> SparseRow
{
  auto row = SparseRow {
      .name = std::string(name),
      .lowb = objective_value,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = scale_alpha;

  for (const auto& link : links) {
    const auto pi = row_duals[link.coupling_row];
    if (std::abs(pi) < cut_coeff_eps) {
      continue;
    }
    row[link.source_col] = -pi;
    row.lowb -= pi * link.trial_value;
  }

  return row;
}

// ─── Cut coefficient filtering and rescaling ────────────────────────────────

void filter_cut_coefficients(SparseRow& row, ColIndex alpha_col, double eps)
{
  if (eps <= 0.0) {
    return;
  }

  // Collect columns to erase (can't modify flat_map while iterating).
  std::vector<ColIndex> to_erase;
  for (auto it = row.cmap.begin(); it != row.cmap.end(); ++it) {
    if (it->first != alpha_col && std::abs(it->second) < eps) {
      to_erase.push_back(it->first);
    }
  }

  for (const auto col : to_erase) {
    row.cmap.erase(col);
  }
}

bool rescale_benders_cut(SparseRow& row,
                         ColIndex alpha_col,
                         double cut_coeff_max)
{
  if (cut_coeff_max <= 0.0) {
    return false;
  }

  // Find the largest absolute coefficient across all state-variable terms
  // (exclude α column — it's a scaling weight, not a state-variable coeff).
  // Collect keys to avoid flat_map iterator proxy issues.
  std::vector<ColIndex> cols;
  cols.reserve(row.cmap.size());

  double max_coeff = 0.0;
  for (auto it = row.cmap.begin(); it != row.cmap.end(); ++it) {
    cols.push_back(it->first);
    if (it->first != alpha_col) {
      max_coeff = std::max(max_coeff, std::abs(it->second));
    }
  }

  if (max_coeff <= cut_coeff_max) {
    return false;  // no rescaling needed
  }

  const auto scale_factor = max_coeff / cut_coeff_max;

  spdlog::warn(
      "rescale_benders_cut: cut '{}' max|coeff|={:.2e} exceeds "
      "threshold {:.2e}, rescaling by {:.2e}",
      row.name,
      max_coeff,
      cut_coeff_max,
      scale_factor);

  // Divide all coefficients (including α) and bounds by scale_factor.
  for (const auto col : cols) {
    row[col] /= scale_factor;
  }
  row.lowb /= scale_factor;
  if (row.uppb != LinearProblem::DblMax && row.uppb != -LinearProblem::DblMax) {
    row.uppb /= scale_factor;
  }

  return true;
}

// ─── Elastic filter ─────────────────────────────────────────────────────────

RelaxedVarInfo relax_fixed_state_variable(LinearInterface& li,
                                          const StateVarLink& link,
                                          [[maybe_unused]] PhaseIndex phase,
                                          double penalty)
{
  const auto dep = link.dependent_col;
  const auto lo = li.get_col_low()[dep];
  const auto hi = li.get_col_upp()[dep];

  if (std::abs(lo - hi) >= 1e-10) {
    return {};
  }

  // Relax to the physical bounds captured from the source column
  li.set_col_low(dep, link.source_low);
  li.set_col_upp(dep, link.source_upp);

  // Penalised slack variables: up (overshoot) and dn (undershoot)
  //
  // When the link carries a per-variable state cost (scost > 0, e.g. from
  // Reservoir::scost in $/hm³), use it instead of the global penalty.
  // The scost is already in $/physical_unit, so we only need to divide
  // by scale_objective (the caller pre-divides `penalty` by scale_obj,
  // so we reconstruct the per-variable penalty the same way).
  //
  // Scale by var_scale: 1 LP unit of slack = var_scale physical units,
  // so the cost per LP unit is base_penalty × var_scale.
  const auto base_penalty = (link.scost > 0.0) ? link.scost : penalty;
  const auto scaled_penalty = base_penalty * link.var_scale;
  const auto sup = li.add_col({}, 0.0, li.infinity());
  li.set_obj_coeff(sup, scaled_penalty);

  const auto sdn = li.add_col({}, 0.0, li.infinity());
  li.set_obj_coeff(sdn, scaled_penalty);

  // dep + sup − sdn = trial_value
  auto elastic = SparseRow {
      .name = {},
      .lowb = link.trial_value,
      .uppb = link.trial_value,
  };
  elastic[dep] = 1.0;
  elastic[sup] = 1.0;
  elastic[sdn] = -1.0;

  li.add_row(elastic);

  SPDLOG_TRACE(
      "SDDP elastic: phase {} col {} relaxed to [{:.2f}, {:.2f}] "
      "(source bounds from phase {})",
      phase,
      dep,
      link.source_low,
      link.source_upp,
      link.source_phase);

  return RelaxedVarInfo {
      .relaxed = true,
      .sup_col = sup,
      .sdn_col = sdn,
  };
}

auto elastic_filter_solve(const LinearInterface& li,
                          std::span<const StateVarLink> links,
                          double penalty,
                          const SolverOptions& opts,
                          std::span<const double> forward_col_sol,
                          std::span<const double> forward_row_dual)
    -> std::optional<ElasticSolveResult>
{
  // Clone the LP – modifications to the clone don't touch the original.
  auto cloned = li.clone();

  ElasticSolveResult result;
  result.link_infos.reserve(links.size());

  bool modified = false;
  for (const auto& link : links) {
    auto info =
        relax_fixed_state_variable(cloned, link, link.target_phase, penalty);
    modified |= info.relaxed;
    result.link_infos.push_back(info);
  }

  if (!modified) {
    return std::nullopt;
  }

  // Apply forward-pass solution as warm-start hint.  The clone now has
  // extra columns (elastic slacks) — the helper pads with zeros.
  if (opts.reuse_basis) {
    cloned.set_warm_start_solution(forward_col_sol, forward_row_dual);
  }

  // Solve the clone with elastic slack variables
  auto r = cloned.resolve(opts);
  if (r.has_value() && cloned.is_optimal()) {
    SPDLOG_TRACE("elastic_filter_solve: solved clone (obj={:.4f})",
                 cloned.get_obj_value());
    result.clone = std::move(cloned);
    return result;
  }
  return std::nullopt;
}

auto build_feasibility_cut(const LinearInterface& li,
                           ColIndex alpha_col,
                           std::span<const StateVarLink> links,
                           double penalty,
                           const SolverOptions& opts,
                           std::string_view name,
                           double scale_alpha)
    -> std::optional<FeasibilityCutResult>
{
  auto elastic = elastic_filter_solve(li, links, penalty, opts);
  if (!elastic.has_value()) {
    return std::nullopt;
  }

  auto cut = build_benders_cut(alpha_col,
                               links,
                               elastic->clone.get_col_cost_raw(),
                               elastic->clone.get_obj_value(),
                               name,
                               scale_alpha);

  return FeasibilityCutResult {
      .cut = std::move(cut),
      .elastic = std::move(*elastic),
  };
}

auto build_multi_cuts(const ElasticSolveResult& elastic,
                      std::span<const StateVarLink> links,
                      std::string_view name_prefix,
                      double slack_tol) -> std::vector<SparseRow>
{
  std::vector<SparseRow> cuts;

  const auto& dep_sol = elastic.clone.get_col_sol_raw();
  const auto& link_infos = elastic.link_infos;
  const std::size_t nlinks = links.size();

  int cut_counter = 0;
  for (std::size_t li_idx = 0; li_idx < nlinks; ++li_idx) {
    const auto& info = link_infos[li_idx];
    if (!info.relaxed) {
      continue;
    }
    const auto& link = links[li_idx];
    const double dep_val = dep_sol[link.dependent_col];

    // sup > 0 ⟹ solution < trial_value ⟹ source ≤ dep_val
    if (info.sup_col != ColIndex {unknown_index}) {
      const double sup_val = dep_sol[info.sup_col];
      if (sup_val > slack_tol) {
        auto ub_cut = SparseRow {
            .name = std::format("{}_ub_{}", name_prefix, cut_counter++),
            .lowb = -LinearProblem::DblMax,
            .uppb = dep_val,
        };
        ub_cut[link.source_col] = 1.0;
        cuts.push_back(std::move(ub_cut));
      }
    }

    // sdn > 0 ⟹ solution > trial_value ⟹ source ≥ dep_val
    if (info.sdn_col != ColIndex {unknown_index}) {
      const double sdn_val = dep_sol[info.sdn_col];
      if (sdn_val > slack_tol) {
        auto lb_cut = SparseRow {
            .name = std::format("{}_lb_{}", name_prefix, cut_counter++),
            .lowb = dep_val,
            .uppb = LinearProblem::DblMax,
        };
        lb_cut[link.source_col] = 1.0;
        cuts.push_back(std::move(lb_cut));
      }
    }
  }

  return cuts;
}

// ─── Cut averaging ──────────────────────────────────────────────────────────

auto average_benders_cut(const std::vector<SparseRow>& cuts,
                         std::string_view name) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() == 1) {
    auto result = cuts.front();
    result.name = std::string(name);
    return result;
  }

  const auto n = static_cast<double>(cuts.size());

  // Collect all column indices that appear in any cut
  flat_map<ColIndex, double> avg_coeffs;
  double avg_rhs = 0.0;

  for (const auto& cut : cuts) {
    avg_rhs += cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      avg_coeffs[col] += coeff;
    }
  }

  auto result = SparseRow {
      .name = std::string(name),
      .lowb = avg_rhs / n,
      .uppb = LinearProblem::DblMax,
  };

  for (const auto& [col, total_coeff] : avg_coeffs) {
    result[col] = total_coeff / n;
  }

  return result;
}

auto weighted_average_benders_cut(const std::vector<SparseRow>& cuts,
                                  const std::vector<double>& weights,
                                  std::string_view name) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() != weights.size()) {
    SPDLOG_WARN(
        "weighted_average_benders_cut: cuts.size()={} != weights.size()={}, "
        "returning empty cut",
        cuts.size(),
        weights.size());
    return {};
  }

  // Compute the total weight for normalisation
  double total_weight = 0.0;
  for (const double w : weights) {
    total_weight += w;
  }

  if (total_weight <= 0.0) {
    return {};
  }

  // Single-cut shortcut (avoid unnecessary work)
  if (cuts.size() == 1) {
    auto result = cuts.front();
    result.name = std::string(name);
    return result;
  }

  flat_map<ColIndex, double> avg_coeffs;
  double avg_rhs = 0.0;

  for (std::size_t i = 0; i < cuts.size(); ++i) {
    const auto& cut = cuts[i];
    const double w = weights[i] / total_weight;
    avg_rhs += w * cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      avg_coeffs[col] += w * coeff;
    }
  }

  auto result = SparseRow {
      .name = std::string(name),
      .lowb = avg_rhs,
      .uppb = LinearProblem::DblMax,
  };

  for (const auto& [col, coeff] : avg_coeffs) {
    result[col] = coeff;
  }

  return result;
}

auto accumulate_benders_cuts(const std::vector<SparseRow>& cuts,
                             std::string_view name) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() == 1) {
    auto result = cuts.front();
    result.name = std::string(name);
    return result;
  }

  // Accumulate (sum) all cuts: no division by count or weight normalisation
  flat_map<ColIndex, double> sum_coeffs;
  double sum_rhs = 0.0;

  for (const auto& cut : cuts) {
    sum_rhs += cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      sum_coeffs[col] += coeff;
    }
  }

  auto result = SparseRow {
      .name = std::string(name),
      .lowb = sum_rhs,
      .uppb = LinearProblem::DblMax,
  };

  for (const auto& [col, coeff] : sum_coeffs) {
    result[col] = coeff;
  }

  return result;
}

// ─── BendersCut class ────────────────────────────────────────────────────────

auto BendersCut::elastic_filter_solve(const LinearInterface& li,
                                      std::span<const StateVarLink> links,
                                      double penalty,
                                      const SolverOptions& opts,
                                      std::span<const double> forward_col_sol,
                                      std::span<const double> forward_row_dual)
    -> std::optional<ElasticSolveResult>
{
  if (m_pool_ == nullptr) {
    // No pool available: delegate directly to the free function.
    auto result = gtopt::elastic_filter_solve(
        li, links, penalty, opts, forward_col_sol, forward_row_dual);
    if (result.has_value()) {
      m_infeasible_cut_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  // Clone the LP – modifications to the clone don't touch the original.
  auto cloned = li.clone();

  ElasticSolveResult result;
  result.link_infos.reserve(links.size());

  bool modified = false;
  for (const auto& link : links) {
    auto info =
        relax_fixed_state_variable(cloned, link, link.target_phase, penalty);
    modified |= info.relaxed;
    result.link_infos.push_back(info);
  }

  if (!modified) {
    return std::nullopt;
  }

  // Apply forward-pass solution as warm-start hint (pads extra cols/rows)
  if (opts.reuse_basis) {
    cloned.set_warm_start_solution(forward_col_sol, forward_row_dual);
  }

  // Transfer ownership of the cloned LP to a shared_ptr captured by value
  // in the pool task lambda.  This guarantees the clone's lifetime extends
  // until after pool.get() returns, regardless of when the task is scheduled.
  auto cloned_sp = std::make_shared<LinearInterface>(std::move(cloned));

  // Submit the LP solve to the work pool so that the pool's scheduling and
  // monitoring infrastructure (CPU load, active workers) observe the solve.
  // The calling thread blocks on the future until the solve completes.
  auto fut = m_pool_->submit([cloned_sp, opts]() -> std::expected<int, Error>
                             { return cloned_sp->resolve(opts); });

  bool solved = false;
  if (fut.has_value()) {
    auto r = fut.value().get();
    solved = r.has_value() && cloned_sp->is_optimal();
  } else {
    // Pool submission failed (e.g. pool shut down): fall back to direct solve.
    SPDLOG_WARN(
        "BendersCut::elastic_filter_solve: pool submit failed, "
        "falling back to direct solve");
    auto r = cloned_sp->resolve(opts);
    solved = r.has_value() && cloned_sp->is_optimal();
  }

  if (solved) {
    m_infeasible_cut_count_.fetch_add(1, std::memory_order_relaxed);
    SPDLOG_TRACE(
        "BendersCut::elastic_filter_solve: solved clone via pool "
        "(obj={:.4f}), total_infeasible_cuts={}",
        cloned_sp->get_obj_value(),
        m_infeasible_cut_count_.load(std::memory_order_relaxed));
    result.clone = std::move(*cloned_sp);
    return result;
  }

  return std::nullopt;
}

auto BendersCut::build_feasibility_cut(const LinearInterface& li,
                                       ColIndex alpha_col,
                                       std::span<const StateVarLink> links,
                                       double penalty,
                                       const SolverOptions& opts,
                                       std::string_view name,
                                       double scale_alpha)
    -> std::optional<FeasibilityCutResult>
{
  auto elastic = this->elastic_filter_solve(li, links, penalty, opts);
  if (!elastic.has_value()) {
    return std::nullopt;
  }

  auto cut = build_benders_cut(alpha_col,
                               links,
                               elastic->clone.get_col_cost_raw(),
                               elastic->clone.get_obj_value(),
                               name,
                               scale_alpha);

  return FeasibilityCutResult {
      .cut = std::move(cut),
      .elastic = std::move(*elastic),
  };
}

}  // namespace gtopt
