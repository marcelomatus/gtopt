/**
 * @file      sddp_solver.cpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) solver implementation
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the forward/backward iterative decomposition.  See
 * sddp_solver.hpp for the algorithm description.
 */

#include <algorithm>
#include <cmath>
#include <format>

#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Constructor ─────────────────────────────────────────────────────────────

SDDPSolver::SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts) noexcept
    : m_planning_lp_(planning_lp)
    , m_options_(std::move(opts))
{
}

// ─── Initialisation helpers ──────────────────────────────────────────────────

void SDDPSolver::initialize_alpha_variables(SceneIndex scene_index)
{
  const auto& phases = m_planning_lp_.simulation().phases();
  const auto num_phases = static_cast<Index>(phases.size());

  m_phase_states_.resize(num_phases);

  // Add α (future-cost) variable to every phase except the last
  for (Index pi = 0; pi < num_phases - 1; ++pi) {
    auto& state = m_phase_states_[PhaseIndex {pi}];
    auto& sys = m_planning_lp_.system(scene_index, PhaseIndex {pi});

    const auto alpha_name = std::format("sddp_alpha_phase_{}", pi);
    state.alpha_col = sys.linear_interface().add_col(
        alpha_name, m_options_.alpha_min, m_options_.alpha_max);

    // α enters the objective with coefficient 1.0
    sys.linear_interface().set_obj_coeff(state.alpha_col, 1.0);
  }

  // Last phase has no future-cost variable
  m_phase_states_[PhaseIndex {num_phases - 1}].alpha_col =
      ColIndex {unknown_index};
}

void SDDPSolver::collect_state_variable_links(SceneIndex scene_index)
{
  // Walk the SimulationLP state variable map for this scene.
  // For each state variable in phase p that has dependents in phase p+1,
  // record a StateVarLink.
  const auto& sim = m_planning_lp_.simulation();
  const auto& phases = sim.phases();
  const auto num_phases = static_cast<Index>(phases.size());

  for (Index pi = 0; pi < num_phases; ++pi) {
    const auto phase_index = PhaseIndex {pi};
    auto& state = m_phase_states_[phase_index];

    const auto& svar_map = sim.state_variables(scene_index, phase_index);

    for (const auto& [key, svar] : svar_map) {
      for (const auto& dep : svar.dependent_variables()) {
        // Only consider dependents in the immediately next phase
        if (dep.phase_index() != PhaseIndex {pi + 1}) {
          continue;
        }
        if (dep.scene_index() != scene_index) {
          continue;
        }

        state.outgoing_links.push_back(StateVarLink {
            .source_col = svar.col(),
            .dependent_col = dep.col(),
            .source_phase = phase_index,
            .target_phase = dep.phase_index(),
        });
      }
    }

    SPDLOG_DEBUG("SDDP: phase {} has {} outgoing state-variable links",
                 pi,
                 state.outgoing_links.size());
  }
}

// ─── Forward pass ────────────────────────────────────────────────────────────

auto SDDPSolver::forward_pass(SceneIndex scene_index,
                              const SolverOptions& lp_opts)
    -> std::expected<double, Error>
{
  const auto& phases = m_planning_lp_.simulation().phases();
  const auto num_phases = static_cast<Index>(phases.size());
  double total_operating_cost = 0.0;

  for (Index pi = 0; pi < num_phases; ++pi) {
    const auto phase_index = PhaseIndex {pi};
    auto& sys = m_planning_lp_.system(scene_index, phase_index);
    auto& li = sys.linear_interface();
    auto& state = m_phase_states_[phase_index];

    // Fix dependent state variables from previous phase's solution
    if (pi > 0) {
      auto& prev_state = m_phase_states_[PhaseIndex {pi - 1}];
      const auto& prev_sys =
          m_planning_lp_.system(scene_index, PhaseIndex {pi - 1});
      const auto& prev_sol = prev_sys.linear_interface().get_col_sol();

      for (auto& link : prev_state.outgoing_links) {
        const auto value = prev_sol[link.source_col];
        link.trial_value = value;

        // Fix the dependent column: lb = ub = value
        li.set_col_low(link.dependent_col, value);
        li.set_col_upp(link.dependent_col, value);
      }
    }

    // Solve this phase
    auto result = li.resolve(lp_opts);
    if (!result.has_value() || !li.is_optimal()) {
      // Try elastic filter for infeasibility
      if (apply_elastic_filter(scene_index, phase_index, lp_opts)) {
        result = li.resolve(lp_opts);
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

    // Record the operating cost (objective minus α contribution)
    const auto obj = li.get_obj_value();
    double alpha_value = 0.0;
    if (state.alpha_col != ColIndex {unknown_index}) {
      alpha_value = li.get_col_sol()[state.alpha_col];
    }
    state.forward_objective = obj - alpha_value;
    total_operating_cost += state.forward_objective;

    SPDLOG_DEBUG("SDDP forward: phase {} obj={:.4f} alpha={:.4f} opex={:.4f}",
                 pi,
                 obj,
                 alpha_value,
                 state.forward_objective);
  }

  return total_operating_cost;
}

// ─── Backward pass ───────────────────────────────────────────────────────────

auto SDDPSolver::backward_pass(SceneIndex scene_index,
                               const SolverOptions& lp_opts)
    -> std::expected<int, Error>
{
  const auto& phases = m_planning_lp_.simulation().phases();
  const auto num_phases = static_cast<Index>(phases.size());
  int total_cuts = 0;

  // Iterate backward: for each phase t (from T-1 down to 1), use the
  // dual information from phase t to build a Benders cut for phase t-1.
  for (Index pi = num_phases - 1; pi >= 1; --pi) {
    const auto target_phase_index = PhaseIndex {pi};
    auto& target_sys = m_planning_lp_.system(scene_index, target_phase_index);
    auto& target_li = target_sys.linear_interface();

    // The LP of this phase was solved in the forward pass.
    const auto z = target_li.get_obj_value();
    const auto reduced_costs = target_li.get_col_cost();

    // Build the Benders cut for the previous phase (pi - 1)
    const auto source_phase_index = PhaseIndex {pi - 1};
    auto& source_sys = m_planning_lp_.system(scene_index, source_phase_index);
    auto& source_li = source_sys.linear_interface();
    const auto& source_state = m_phase_states_[source_phase_index];

    // The Benders cut approximates the future cost function:
    //
    //   α_{t-1} ≥ z_t + Σ_i rc_i · (x_{t-1,i} - v̂_i)
    //
    // In row form (added to phase t-1's LP):
    //
    //   α_{t-1} - Σ_i rc_i · x_{t-1,i} ≥ z_t - Σ_i rc_i · v̂_i
    //
    // where:
    //   rc_i      = reduced cost of the dependent column in phase t
    //   x_{t-1,i} = source column in phase t-1 (e.g. efin or capainst)
    //   v̂_i       = trial value from the forward pass

    SparseRow cut_row;
    cut_row.name = std::format("sddp_cut_ph{}_{}", pi, total_cuts);

    // α coefficient
    cut_row[source_state.alpha_col] = 1.0;

    // RHS starts with the objective value z
    double rhs = z;

    for (const auto& link : source_state.outgoing_links) {
      const auto rc = reduced_costs[link.dependent_col];

      // Coefficient of the source column in the cut: -rc
      cut_row[link.source_col] = -rc;

      // RHS adjustment: -rc * v̂
      rhs -= rc * link.trial_value;

      SPDLOG_DEBUG("  cut term: src_col={} dep_col={} rc={:.6f} trial={:.2f}",
                   static_cast<Index>(link.source_col),
                   static_cast<Index>(link.dependent_col),
                   rc,
                   link.trial_value);
    }

    cut_row.lowb = rhs;
    cut_row.uppb = LinearProblem::DblMax;

    // Add the cut to the source phase's LP
    source_li.add_row(cut_row);
    ++total_cuts;

    SPDLOG_DEBUG("SDDP backward: cut for phase {} rhs={:.4f}", pi - 1, rhs);

    // Re-solve the source phase with the new cut so that further backward
    // iterations (going to phase pi-2) see updated dual information.
    if (pi > 1) {
      auto result = source_li.resolve(lp_opts);
      if (!result.has_value() || !source_li.is_optimal()) {
        SPDLOG_WARN("SDDP backward: phase {} re-solve issue", pi - 1);
      }
    }
  }

  return total_cuts;
}

// ─── Elastic filter ──────────────────────────────────────────────────────────

bool SDDPSolver::apply_elastic_filter(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    [[maybe_unused]] const SolverOptions& lp_opts)
{
  if (phase_index == PhaseIndex {0}) {
    return false;  // Phase 0 has no incoming state variables to relax
  }

  auto& sys = m_planning_lp_.system(scene_index, phase_index);
  auto& li = sys.linear_interface();

  // Relax the dependent state-variable columns from the previous phase.
  const auto prev_phase_index =
      PhaseIndex {static_cast<Index>(phase_index) - 1};
  const auto& prev_state = m_phase_states_[prev_phase_index];

  bool modified = false;
  for (const auto& link : prev_state.outgoing_links) {
    const auto dep_col = link.dependent_col;
    const auto current_low = li.get_col_low()[dep_col];
    const auto current_upp = li.get_col_upp()[dep_col];

    if (std::abs(current_low - current_upp) < 1e-10) {
      // Variable is fixed; relax the bounds
      const auto margin = std::max(
          1.0, std::abs(current_low) * m_options_.elastic_margin_factor);
      li.set_col_low(dep_col, current_low - margin);
      li.set_col_upp(dep_col, current_upp + margin);

      // Add penalty slack variables
      const auto slack_up_col =
          li.add_col(std::format("elastic_up_ph{}_c{}",
                                 static_cast<Index>(phase_index),
                                 static_cast<Index>(dep_col)),
                     0.0,
                     LinearProblem::DblMax);
      li.set_obj_coeff(slack_up_col, m_options_.elastic_penalty);

      const auto slack_dn_col =
          li.add_col(std::format("elastic_dn_ph{}_c{}",
                                 static_cast<Index>(phase_index),
                                 static_cast<Index>(dep_col)),
                     0.0,
                     LinearProblem::DblMax);
      li.set_obj_coeff(slack_dn_col, m_options_.elastic_penalty);

      // Constraint: dep_col + slack_up - slack_dn = trial_value
      SparseRow elastic_row;
      elastic_row.name = std::format("elastic_ph{}_c{}",
                                     static_cast<Index>(phase_index),
                                     static_cast<Index>(dep_col));
      elastic_row[dep_col] = 1.0;
      elastic_row[slack_up_col] = 1.0;
      elastic_row[slack_dn_col] = -1.0;
      elastic_row.lowb = link.trial_value;
      elastic_row.uppb = link.trial_value;

      li.add_row(elastic_row);
      modified = true;
    }
  }

  return modified;
}

// ─── Main solve loop ─────────────────────────────────────────────────────────

auto SDDPSolver::solve(const SolverOptions& lp_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  const auto& scenes = m_planning_lp_.simulation().scenes();
  if (scenes.empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "No scenes in simulation",
    });
  }

  const auto& phases = m_planning_lp_.simulation().phases();
  if (phases.size() < 2) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "SDDP requires at least 2 phases",
    });
  }

  // Use scene 0 for deterministic SDDP (single scene)
  const auto scene_index = SceneIndex {0};

  // Initial solve: establish a baseline solution for all phases
  // (this also triggers the state-variable linking in PlanningLP)
  {
    auto result = m_planning_lp_.resolve();
    if (!result.has_value()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format("Initial PlanningLP solve failed: {}",
                                 result.error().message),
      });
    }
  }

  // Initialize: add α variables and discover state-variable links
  if (!m_initialized_) {
    initialize_alpha_variables(scene_index);
    collect_state_variable_links(scene_index);
    m_initialized_ = true;
  }

  std::vector<SDDPIterationResult> results;
  results.reserve(m_options_.max_iterations);

  for (int iter = 1; iter <= m_options_.max_iterations; ++iter) {
    SDDPIterationResult iter_result {
        .iteration = iter,
    };

    // ── Forward pass ─────────────────────────────────────────────────
    auto forward_result = forward_pass(scene_index, lp_opts);
    if (!forward_result.has_value()) {
      return std::unexpected(std::move(forward_result.error()));
    }
    iter_result.upper_bound = *forward_result;

    // Lower bound = phase 0 objective (includes α estimate of future)
    {
      const auto& sys0 = m_planning_lp_.system(scene_index, PhaseIndex {0});
      iter_result.lower_bound = sys0.linear_interface().get_obj_value();
    }

    // ── Backward pass ────────────────────────────────────────────────
    auto backward_result = backward_pass(scene_index, lp_opts);
    if (!backward_result.has_value()) {
      return std::unexpected(std::move(backward_result.error()));
    }
    iter_result.cuts_added = *backward_result;

    // ── Convergence check ────────────────────────────────────────────
    const auto denom = std::max(1.0, std::abs(iter_result.upper_bound));
    iter_result.gap =
        (iter_result.upper_bound - iter_result.lower_bound) / denom;

    iter_result.converged = (iter_result.gap < m_options_.convergence_tol);

    SPDLOG_INFO("SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} cuts={}{}",
                iter,
                iter_result.lower_bound,
                iter_result.upper_bound,
                iter_result.gap,
                iter_result.cuts_added,
                iter_result.converged ? " [CONVERGED]" : "");

    results.push_back(iter_result);

    if (iter_result.converged) {
      break;
    }
  }

  return results;
}

}  // namespace gtopt
