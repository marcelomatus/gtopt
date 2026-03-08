/**
 * @file      sddp_solver.cpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) solver implementation
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the forward/backward iterative decomposition.  See
 * sddp_solver.hpp for the algorithm description and the free-function
 * building blocks declared there.
 */

#include <cmath>
#include <format>
#include <span>

#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Free-function building blocks ──────────────────────────────────────────

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
                       const std::string& name) -> SparseRow
{
  SparseRow row;
  row.name = name;
  row[alpha_col] = 1.0;

  double rhs = objective_value;
  for (const auto& link : links) {
    const auto rc = reduced_costs[link.dependent_col];
    row[link.source_col] = -rc;
    rhs -= rc * link.trial_value;
  }

  row.lowb = rhs;
  row.uppb = LinearProblem::DblMax;
  return row;
}

bool relax_fixed_state_variable(LinearInterface& li,
                                const StateVarLink& link,
                                PhaseIndex phase,
                                double penalty)
{
  const auto dep = link.dependent_col;
  const auto lo = li.get_col_low()[dep];
  const auto hi = li.get_col_upp()[dep];

  if (std::abs(lo - hi) >= 1e-10) {
    return false;
  }

  // Relax to the physical bounds captured from the source column
  li.set_col_low(dep, link.source_low);
  li.set_col_upp(dep, link.source_upp);

  const auto pi = static_cast<Index>(phase);
  const auto ci = static_cast<Index>(dep);

  // Penalised slack variables: up (overshoot) and dn (undershoot)
  const auto sup = li.add_col(
      std::format("elastic_up_ph{}_c{}", pi, ci), 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sup, penalty);

  const auto sdn = li.add_col(
      std::format("elastic_dn_ph{}_c{}", pi, ci), 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sdn, penalty);

  // dep + sup − sdn = trial_value
  SparseRow elastic;
  elastic.name = std::format("elastic_ph{}_c{}", pi, ci);
  elastic[dep] = 1.0;
  elastic[sup] = 1.0;
  elastic[sdn] = -1.0;
  elastic.lowb = link.trial_value;
  elastic.uppb = link.trial_value;

  li.add_row(elastic);

  SPDLOG_DEBUG(
      "SDDP elastic: phase {} col {} relaxed to [{:.2f}, {:.2f}] "
      "(source bounds from phase {})",
      pi,
      ci,
      link.source_low,
      link.source_upp,
      static_cast<Index>(link.source_phase));
  return true;
}

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

SDDPSolver::SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts) noexcept
    : m_planning_lp_(planning_lp)
    , m_options_(opts)
{
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPSolver::initialize_alpha_variables(SceneIndex scene)
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  m_phase_states_.resize(num_phases);

  // Add α (future-cost) variable to every phase except the last
  for (Index pi = 0; pi < num_phases - 1; ++pi) {
    auto& state = m_phase_states_[PhaseIndex {pi}];
    auto& li = planning_lp().system(scene, PhaseIndex {pi}).linear_interface();

    state.alpha_col = li.add_col(std::format("sddp_alpha_phase_{}", pi),
                                 m_options_.alpha_min,
                                 m_options_.alpha_max);
    li.set_obj_coeff(state.alpha_col, 1.0);
  }

  // Last phase: no future cost
  m_phase_states_[PhaseIndex {num_phases - 1}].alpha_col =
      ColIndex {unknown_index};
}

void SDDPSolver::collect_state_variable_links(SceneIndex scene)
{
  const auto& sim = planning_lp().simulation();
  const auto num_phases = static_cast<Index>(sim.phases().size());

  for (Index pi = 0; pi < num_phases; ++pi) {
    const auto phase = PhaseIndex {pi};
    auto& state = m_phase_states_[phase];

    // Read column bounds from the source phase LP
    const auto& src_li = planning_lp().system(scene, phase).linear_interface();
    const auto col_lo = src_li.get_col_low();
    const auto col_hi = src_li.get_col_upp();

    for (const auto& [key, svar] : sim.state_variables(scene, phase)) {
      for (const auto& dep : svar.dependent_variables()) {
        if (dep.phase_index() != PhaseIndex {pi + 1}
            || dep.scene_index() != scene)
        {
          continue;
        }

        state.outgoing_links.push_back(StateVarLink {
            .source_col = svar.col(),
            .dependent_col = dep.col(),
            .source_phase = phase,
            .target_phase = dep.phase_index(),
            .source_low = col_lo[svar.col()],
            .source_upp = col_hi[svar.col()],
        });
      }
    }

    SPDLOG_DEBUG("SDDP: phase {} has {} outgoing state-variable links",
                 pi,
                 state.outgoing_links.size());
  }
}

// ── Forward pass ────────────────────────────────────────────────────────────

auto SDDPSolver::forward_pass(SceneIndex scene, const SolverOptions& opts)
    -> std::expected<double, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  double total_opex = 0.0;

  for (Index pi = 0; pi < num_phases; ++pi) {
    const auto phase = PhaseIndex {pi};
    auto& li = planning_lp().system(scene, phase).linear_interface();
    auto& state = m_phase_states_[phase];

    // Propagate state variables from previous phase
    if (pi > 0) {
      auto& prev = m_phase_states_[PhaseIndex {pi - 1}];
      const auto& prev_sol = planning_lp()
                                 .system(scene, PhaseIndex {pi - 1})
                                 .linear_interface()
                                 .get_col_sol();
      propagate_trial_values(prev.outgoing_links, prev_sol, li);
    }

    // Solve this phase
    auto result = li.resolve(opts);
    if (!result.has_value() || !li.is_optimal()) {
      if (apply_elastic_filter(scene, phase, opts)) {
        result = li.resolve(opts);
      }
      if (!result.has_value() || !li.is_optimal()) {
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message =
                std::format("SDDP forward pass failed at phase {} (status {})",
                            pi,
                            li.get_status()),
        });
      }
    }

    // Operating cost = objective minus α contribution
    const auto obj = li.get_obj_value();
    const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
        ? li.get_col_sol()[state.alpha_col]
        : 0.0;
    state.forward_objective = obj - alpha_val;
    total_opex += state.forward_objective;

    SPDLOG_DEBUG("SDDP forward: phase {} obj={:.4f} alpha={:.4f} opex={:.4f}",
                 pi,
                 obj,
                 alpha_val,
                 state.forward_objective);
  }

  return total_opex;
}

// ── Backward pass ───────────────────────────────────────────────────────────

auto SDDPSolver::backward_pass(SceneIndex scene, const SolverOptions& opts)
    -> std::expected<int, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  int total_cuts = 0;

  for (Index pi = num_phases - 1; pi >= 1; --pi) {
    auto& target_li =
        planning_lp().system(scene, PhaseIndex {pi}).linear_interface();

    const auto src_phase = PhaseIndex {pi - 1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = m_phase_states_[src_phase];

    // Build and add Benders cut
    auto cut =
        build_benders_cut(src_state.alpha_col,
                          src_state.outgoing_links,
                          target_li.get_col_cost(),
                          target_li.get_obj_value(),
                          std::format("sddp_cut_ph{}_{}", pi, total_cuts));

    src_li.add_row(cut);
    ++total_cuts;

    SPDLOG_DEBUG(
        "SDDP backward: cut for phase {} rhs={:.4f}", pi - 1, cut.lowb);

    // Re-solve source so upstream cuts see updated duals
    if (pi > 1) {
      if (auto r = src_li.resolve(opts); !r.has_value() || !src_li.is_optimal())
      {
        SPDLOG_WARN("SDDP backward: phase {} re-solve issue", pi - 1);
      }
    }
  }

  return total_cuts;
}

// ── Elastic filter ──────────────────────────────────────────────────────────

bool SDDPSolver::apply_elastic_filter(
    SceneIndex scene,
    PhaseIndex phase,
    [[maybe_unused]] const SolverOptions& opts)
{
  if (phase == PhaseIndex {0}) {
    return false;
  }

  auto& li = planning_lp().system(scene, phase).linear_interface();
  const auto prev = PhaseIndex {static_cast<Index>(phase) - 1};
  const auto& prev_state = m_phase_states_[prev];

  bool modified = false;
  for (const auto& link : prev_state.outgoing_links) {
    modified |=
        relax_fixed_state_variable(li, link, phase, m_options_.elastic_penalty);
  }
  return modified;
}

// ── Main solve loop ─────────────────────────────────────────────────────────

auto SDDPSolver::solve(const SolverOptions& lp_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  const auto& sim = planning_lp().simulation();

  if (sim.scenes().empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "No scenes in simulation",
    });
  }
  if (sim.phases().size() < 2) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "SDDP requires at least 2 phases",
    });
  }

  const auto scene = SceneIndex {0};

  // Bootstrap: solve all phases to establish baseline and state links
  if (auto r = planning_lp().resolve(); !r.has_value()) {
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::format("Initial PlanningLP solve failed: {}",
                               r.error().message),
    });
  }

  if (!m_initialized_) {
    initialize_alpha_variables(scene);
    collect_state_variable_links(scene);
    m_initialized_ = true;
  }

  std::vector<SDDPIterationResult> results;
  results.reserve(m_options_.max_iterations);

  for (int iter = 1; iter <= m_options_.max_iterations; ++iter) {
    SDDPIterationResult ir {
        .iteration = iter,
    };

    // Forward pass → upper bound
    auto fwd = forward_pass(scene, lp_opts);
    if (!fwd.has_value()) {
      return std::unexpected(std::move(fwd.error()));
    }
    ir.upper_bound = *fwd;

    // Lower bound = phase 0 objective (includes α)
    ir.lower_bound = planning_lp()
                         .system(scene, PhaseIndex {0})
                         .linear_interface()
                         .get_obj_value();

    // Backward pass → Benders cuts
    auto bwd = backward_pass(scene, lp_opts);
    if (!bwd.has_value()) {
      return std::unexpected(std::move(bwd.error()));
    }
    ir.cuts_added = *bwd;

    // Convergence check
    const auto denom = std::max(1.0, std::abs(ir.upper_bound));
    ir.gap = (ir.upper_bound - ir.lower_bound) / denom;
    ir.converged = (ir.gap < m_options_.convergence_tol);

    SPDLOG_INFO("SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} cuts={}{}",
                iter,
                ir.lower_bound,
                ir.upper_bound,
                ir.gap,
                ir.cuts_added,
                ir.converged ? " [CONVERGED]" : "");

    results.push_back(ir);

    if (ir.converged) {
      break;
    }
  }

  return results;
}

}  // namespace gtopt
