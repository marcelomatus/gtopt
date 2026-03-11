/**
 * @file      benders_cut.cpp
 * @brief     Modular Benders cut construction and handling – implementation
 * @date      2026-03-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the cut-creation building blocks declared in benders_cut.hpp.
 * The free functions moved here were originally part of sddp_solver.cpp.
 */

#include <cmath>
#include <format>
#include <span>

#include <gtopt/benders_cut.hpp>
#include <gtopt/fmap.hpp>

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

auto build_benders_cut(ColIndex alpha_col,
                       std::span<const StateVarLink> links,
                       std::span<const double> reduced_costs,
                       double objective_value,
                       std::string_view name) -> SparseRow
{
  auto row = SparseRow {
      .name = std::string(name),
      .lowb = objective_value,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = 1.0;

  for (const auto& link : links) {
    const auto rc = reduced_costs[link.dependent_col];
    row[link.source_col] = -rc;
    row.lowb -= rc * link.trial_value;
  }

  return row;
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
  const auto sup = li.add_col({}, 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sup, penalty);

  const auto sdn = li.add_col({}, 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sdn, penalty);

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
      static_cast<Index>(phase),
      static_cast<Index>(dep),
      link.source_low,
      link.source_upp,
      static_cast<Index>(link.source_phase));

  return RelaxedVarInfo {
      .relaxed = true,
      .sup_col = sup,
      .sdn_col = sdn,
  };
}

auto elastic_filter_solve(const LinearInterface& li,
                          std::span<const StateVarLink> links,
                          double penalty,
                          const SolverOptions& opts)
    -> std::optional<ElasticSolveResult>
{
  // Clone the LP – modifications to the clone don't touch the original
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
                           std::string_view name)
    -> std::optional<FeasibilityCutResult>
{
  auto elastic = elastic_filter_solve(li, links, penalty, opts);
  if (!elastic.has_value()) {
    return std::nullopt;
  }

  auto cut = build_benders_cut(alpha_col,
                               links,
                               elastic->clone.get_col_cost(),
                               elastic->clone.get_obj_value(),
                               name);

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

  const auto& dep_sol = elastic.clone.get_col_sol();
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

}  // namespace gtopt
