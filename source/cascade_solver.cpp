/**
 * @file      cascade_solver.cpp
 * @brief     Multi-level cascade solver implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <numeric>
#include <ranges>
#include <vector>

#include <gtopt/cascade_solver.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Constructor ────────────────────────────────────────────────────────────

CascadePlanningSolver::CascadePlanningSolver(
    SDDPOptions base_opts, CascadeOptions cascade_opts) noexcept
    : m_base_opts_(std::move(base_opts))
    , m_cascade_opts_(std::move(cascade_opts))
{
  if (m_cascade_opts_.level_array.empty()) {
    // Empty cascade = single level with default model/solver options.
    // Equivalent to running the base SDDP solver directly.
    m_cascade_opts_.level_array = {
        CascadeLevel {
            .name = OptName {"default"},
        },
    };
  }
}

// ─── Build SDDPOptions for one level ────────────────────────────────────────

auto CascadePlanningSolver::build_level_sddp_opts(
    const std::optional<CascadeLevelSolver>& level_solver,
    int remaining_budget) const -> SDDPOptions
{
  auto opts = m_base_opts_;

  // Apply cascade-global SDDP options as per-level defaults.
  // max_iterations is special: it's a global budget applied via
  // remaining_budget, not a per-level default.
  const auto& cs = m_cascade_opts_.sddp_options;
  if (cs.convergence_tol.has_value()) {
    opts.convergence_tol = *cs.convergence_tol;
  }
  if (cs.min_iterations.has_value()) {
    opts.min_iterations = *cs.min_iterations;
  }
  if (cs.elastic_penalty.has_value()) {
    opts.elastic_penalty = *cs.elastic_penalty;
  }
  if (cs.alpha_min.has_value()) {
    opts.alpha_min = *cs.alpha_min;
  }
  if (cs.alpha_max.has_value()) {
    opts.alpha_max = *cs.alpha_max;
  }

  // Apply per-level overrides
  if (level_solver) {
    if (level_solver->max_iterations) {
      opts.max_iterations = *level_solver->max_iterations;
    }
    if (level_solver->apertures.has_value()) {
      opts.apertures = level_solver->apertures;
    }
    if (level_solver->min_iterations) {
      opts.min_iterations = *level_solver->min_iterations;
    }
    if (level_solver->convergence_tol) {
      opts.convergence_tol = *level_solver->convergence_tol;
    }
  }

  // Cap max_iterations to the remaining global budget
  if (remaining_budget >= 0) {
    opts.max_iterations = std::min(opts.max_iterations, remaining_budget);
  }

  return opts;
}

// ─── Clone Planning with LP overrides ───────────────────────────────────────

auto CascadePlanningSolver::clone_planning_with_overrides(
    const Planning& source, const ModelOptions& model_opts) -> Planning
{
  Planning copy = source;

  // Ensure LP names are enabled for named state variable transfer
  if (!copy.options.use_lp_names.has_value() || *copy.options.use_lp_names < 1)
  {
    copy.options.use_lp_names = 1;
  }

  merge_opt(copy.options.use_single_bus, model_opts.use_single_bus);
  merge_opt(copy.options.use_kirchhoff, model_opts.use_kirchhoff);
  merge_opt(copy.options.use_line_losses, model_opts.use_line_losses);
  merge_opt(copy.options.kirchhoff_threshold, model_opts.kirchhoff_threshold);
  merge_opt(copy.options.loss_segments, model_opts.loss_segments);
  merge_opt(copy.options.scale_objective, model_opts.scale_objective);
  merge_opt(copy.options.scale_theta, model_opts.scale_theta);
  merge_opt(copy.options.demand_fail_cost, model_opts.demand_fail_cost);
  merge_opt(copy.options.reserve_fail_cost, model_opts.reserve_fail_cost);
  merge_opt(copy.options.annual_discount_rate, model_opts.annual_discount_rate);
  return copy;
}

// ─── Collect named state variable targets ───────────────────────────────────

auto CascadePlanningSolver::collect_named_targets(const SDDPSolver& solver,
                                                  const PlanningLP& planning_lp)
    -> std::vector<NamedStateTarget>
{
  std::vector<NamedStateTarget> targets;

  for (auto&& [scene, _sc] :
       enumerate<SceneIndex>(planning_lp.simulation().scenes()))
  {
    const auto& scene_states = solver.phase_states(scene);
    for (auto&& [phase, _ph] :
         enumerate<PhaseIndex>(planning_lp.simulation().phases()))
    {
      const auto& state = scene_states[phase];
      const auto& names = planning_lp.system(scene, phase)
                              .linear_interface()
                              .col_index_to_name();

      for (const auto& link : state.outgoing_links) {
        const auto col = static_cast<std::size_t>(link.source_col);
        if (col >= names.size() || names[col].empty()) {
          SPDLOG_DEBUG(
              "Cascade: skipping unnamed state col {} "
              "(scene={}, phase={})",
              col,
              scene,
              phase);
          continue;
        }
        const double val = (col < state.forward_col_sol.size())
            ? state.forward_col_sol[col]
            : 0.0;

        targets.push_back({
            .var_name = names[col],
            .scene = scene,
            .phase = phase,
            .target_value = val,
        });
      }
    }
  }

  SPDLOG_INFO("Cascade: collected {} named state variable targets",
              targets.size());
  return targets;
}

// ─── Add elastic target constraints ─────────────────────────────────────────

void CascadePlanningSolver::add_elastic_targets(
    PlanningLP& planning_lp,
    const std::vector<NamedStateTarget>& targets,
    const CascadeTransition& transition)
{
  const double rtol = transition.target_rtol.value_or(0.05);
  const double min_atol = transition.target_min_atol.value_or(1.0);
  const double penalty = transition.target_penalty.value_or(500.0);

  int added = 0;
  int skipped = 0;

  for (const auto& t : targets) {
    auto& li = planning_lp.system(t.scene, t.phase).linear_interface();
    const auto& col_map = li.col_name_map();

    auto it = col_map.find(t.var_name);
    if (it == col_map.end()) {
      ++skipped;
      continue;
    }

    const auto resolved_col = ColIndex {it->second};
    const double atol = std::max(rtol * std::abs(t.target_value), min_atol);

    // Add slack columns for elastic penalty
    const auto sup_name = std::format("tgt_sup_{}", t.var_name);
    const auto sdn_name = std::format("tgt_sdn_{}", t.var_name);
    const auto sup_col = li.add_col(sup_name, 0.0, 1e20);
    const auto sdn_col = li.add_col(sdn_name, 0.0, 1e20);
    li.set_obj_coeff(sup_col, penalty);
    li.set_obj_coeff(sdn_col, penalty);

    // Add constraint: x - s⁺ + s⁻ ∈ [target - atol, target + atol]
    SparseRow row;
    row.name = std::format("cascade_target_{}", t.var_name);
    row.lowb = t.target_value - atol;
    row.uppb = t.target_value + atol;
    row[resolved_col] = 1.0;
    row[sup_col] = -1.0;
    row[sdn_col] = 1.0;
    li.add_row(row);
    ++added;
  }

  SPDLOG_INFO(
      "Cascade: added {} elastic target constraints "
      "(skipped {} unresolved, rtol={}, min_atol={}, penalty={})",
      added,
      skipped,
      rtol,
      min_atol,
      penalty);
}

// ─── Clear all cuts ─────────────────────────────────────────────────────────

void CascadePlanningSolver::clear_all_cuts(PlanningLP& planning_lp,
                                           const SDDPSolver& solver)
{
  int total_removed = 0;

  for (auto&& [scene, _sc] :
       enumerate<SceneIndex>(planning_lp.simulation().scenes()))
  {
    const auto& states = solver.phase_states(scene);

    for (auto&& [phase, _ph] :
         enumerate<PhaseIndex>(planning_lp.simulation().phases()))
    {
      auto& li = planning_lp.system(scene, phase).linear_interface();
      const auto base = static_cast<int>(states[phase].base_nrows);
      const auto current = static_cast<int>(li.get_numrows());

      if (current > base) {
        auto indices =
            std::views::iota(base, current) | std::ranges::to<std::vector>();
        li.delete_rows(indices);
        total_removed += current - base;
      }
    }
  }

  SPDLOG_INFO("Cascade: cleared {} cut rows from LPs", total_removed);
}

// ─── Main solve orchestration ───────────────────────────────────────────────

auto CascadePlanningSolver::solve(PlanningLP& planning_lp,
                                  const SolverOptions& opts)
    -> std::expected<int, Error>
{
  PlanningLP* current_lp = nullptr;
  const PlanningLP* prev_lp = nullptr;
  std::unique_ptr<SDDPSolver> current_solver;
  std::vector<NamedStateTarget> prev_targets;
  std::vector<StoredCut> prev_cuts;
  ModelOptions prev_effective_model = m_cascade_opts_.model_options;

  // Global iteration budget: cascade sddp_options.max_iterations applies
  // to the sum of all level iterations.  -1 means no global cap.
  const auto& cascade_max_iter = m_cascade_opts_.sddp_options.max_iterations;
  int remaining_budget = cascade_max_iter.has_value() ? *cascade_max_iter : -1;

  SPDLOG_INFO("Cascade: starting with {} levels (global budget={})",
              m_cascade_opts_.level_array.size(),
              remaining_budget);

  for (std::size_t level_idx = 0;
       level_idx < m_cascade_opts_.level_array.size();
       ++level_idx)
  {
    const auto& level = m_cascade_opts_.level_array[level_idx];
    const auto level_name =
        level.name.value_or(std::format("level_{}", level_idx));

    // ── 1. Build LP for each level ──
    // Always build a fresh LP to ensure clean state (no leftover
    // target constraints or alpha variables from previous levels).
    // If this level has model_options, merge them; otherwise reuse
    // the previous level's effective model.
    auto effective_model = prev_effective_model;
    if (level.model_options.has_value()) {
      effective_model.merge(*level.model_options);
    }
    prev_effective_model = effective_model;
    {
      auto modified_planning = clone_planning_with_overrides(
          planning_lp.planning(), effective_model);
      // Default FlatOptions (level 0) provides col names for state variable
      // transfer — no explicit override needed.
      auto new_lp = std::make_unique<PlanningLP>(std::move(modified_planning));
      current_lp = new_lp.get();
      m_owned_lps_.push_back(std::move(new_lp));
      current_solver.reset();

      SPDLOG_INFO("Cascade [{}]: built new PlanningLP", level_name);
    }

    // ── 2. Apply elastic target constraints (before solver creation) ──
    if (level.transition && level_idx > 0) {
      const auto& trans = *level.transition;
      if (trans.inherit_targets.value_or(0) != 0 && !prev_targets.empty()) {
        add_elastic_targets(*current_lp, prev_targets, trans);
      }
    }

    // ── 3. Build SDDPOptions and create solver ──
    auto level_opts =
        build_level_sddp_opts(level.sddp_options, remaining_budget);
    level_opts.save_per_iteration = false;  // only save at final level

    // Last level: restore save_per_iteration from base opts
    if (level_idx == m_cascade_opts_.level_array.size() - 1) {
      level_opts.save_per_iteration = m_base_opts_.save_per_iteration;
    }

    // Always create a fresh solver for each level, ensuring clean state.
    current_solver = std::make_unique<SDDPSolver>(*current_lp, level_opts);

    SPDLOG_INFO(
        "Cascade [{}]: new solver (max_iters={}, "
        "apertures={}, tol={})",
        level_name,
        level_opts.max_iterations,
        level_opts.apertures.has_value()
            ? static_cast<int>(level_opts.apertures->size())
            : -1,
        level_opts.convergence_tol);

    // ── 4. Load inherited cuts (after solver init, so alpha cols exist) ──
    int inherited_cut_count = 0;
    if (level.transition && level_idx > 0) {
      // Ensure alpha variables are added before resolving cut column names
      if (auto init_err = current_solver->ensure_initialized();
          !init_err.has_value())
      {
        return std::unexpected(init_err.error());
      }
      const auto& trans = *level.transition;
      // Non-zero inherit value means inherit cuts (-1 = keep forever,
      // N > 0 = forget after N iterations).
      const int opt_cut_mode = trans.inherit_optimality_cuts.value_or(0);
      const int feas_cut_mode = trans.inherit_feasibility_cuts.value_or(0);
      const bool want_opt_cuts = opt_cut_mode != 0;
      const bool want_feas_cuts = feas_cut_mode != 0;

      if ((want_opt_cuts || want_feas_cuts) && !prev_cuts.empty()
          && prev_lp != nullptr)
      {
        // Filter cuts by type and optionally by dual activity
        const double dual_threshold =
            trans.optimality_dual_threshold.value_or(0.0);
        std::vector<StoredCut> filtered;
        int skipped_by_dual = 0;
        for (const auto& cut : prev_cuts) {
          if ((want_opt_cuts && cut.type == CutType::Optimality)
              || (want_feas_cuts && cut.type == CutType::Feasibility))
          {
            if (dual_threshold > 0.0 && cut.dual.has_value()
                && std::abs(*cut.dual) < dual_threshold)
            {
              ++skipped_by_dual;
              continue;
            }
            filtered.push_back(cut);
          }
        }
        if (skipped_by_dual > 0) {
          SPDLOG_INFO(
              "Cascade [{}]: skipped {} inactive cuts "
              "(|dual| < {})",
              level_name,
              skipped_by_dual,
              dual_threshold);
        }

        if (!filtered.empty()) {
          const auto tmp_path = std::filesystem::temp_directory_path()
              / std::format("cascade_cuts_{}.csv", level_idx);
          auto save_result =
              save_cuts_csv(filtered, *prev_lp, tmp_path.string());

          if (save_result.has_value()) {
            const LabelMaker label_maker(current_lp->options());
            auto load_result =
                load_cuts_csv(*current_lp, tmp_path.string(), label_maker);

            if (load_result.has_value()) {
              inherited_cut_count = load_result->count;
              SPDLOG_INFO(
                  "Cascade [{}]: transferred {} cuts "
                  "(optimality={}, feasibility={})",
                  level_name,
                  load_result->count,
                  want_opt_cuts,
                  want_feas_cuts);
            } else {
              SPDLOG_WARN("Cascade [{}]: cut load failed: {}",
                          level_name,
                          load_result.error().message);
            }

            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
          } else {
            SPDLOG_WARN("Cascade [{}]: cut save failed: {}",
                        level_name,
                        save_result.error().message);
          }
        }
      }
    }

    // ── Cut forgetting: iteration-aware inherited cut lifecycle ──
    // inherit_optimality_cuts / inherit_feasibility_cuts semantics:
    //   0 or absent: do not inherit
    //   -1:          inherit and keep forever
    //   N > 0:       inherit but forget after N training iterations,
    //                then re-solve with only self-generated cuts
    // The forget threshold is the minimum positive value across both
    // cut types (if either requests forgetting, we trigger it).
    int forget_threshold = 0;
    if (level.transition && inherited_cut_count > 0) {
      const int opt_mode =
          level.transition->inherit_optimality_cuts.value_or(0);
      const int feas_mode =
          level.transition->inherit_feasibility_cuts.value_or(0);
      // Find the minimum positive threshold
      if (opt_mode > 0 && feas_mode > 0) {
        forget_threshold = std::min(opt_mode, feas_mode);
      } else if (opt_mode > 0) {
        forget_threshold = opt_mode;
      } else if (feas_mode > 0) {
        forget_threshold = feas_mode;
      }
    }

    // If forget_threshold > 0, cap phase-1 to that many iterations
    if (forget_threshold > 0) {
      level_opts.max_iterations =
          std::min(level_opts.max_iterations, forget_threshold);
      current_solver->mutable_options() = level_opts;
    }

    const auto t_level = std::chrono::steady_clock::now();
    auto result = current_solver->solve(opts);

    if (!result.has_value()) {
      return std::unexpected(result.error());
    }

    // Forget inherited cuts if any inherit mode was positive (N > 0).
    // Phase-1 was already capped to N iterations; now we delete the
    // inherited cuts from the LP and re-solve with remaining ones.
    const auto phase1_iters = static_cast<int>(result->size()) - 1;
    const bool should_forget = inherited_cut_count > 0 && forget_threshold > 0;

    if (should_forget) {
      SPDLOG_INFO("Cascade [{}]: forgetting {} inherited cuts after {} iters",
                  level_name,
                  inherited_cut_count,
                  phase1_iters);

      // Delete inherited cuts from the LP rows and update row indices
      current_solver->forget_first_cuts(inherited_cut_count);

      // Deduct phase-1 iterations from budget
      if (remaining_budget >= 0) {
        remaining_budget = std::max(0, remaining_budget - phase1_iters);
      }

      // Keep phase-1 results
      m_all_results_.insert(
          m_all_results_.end(), result->begin(), result->end());

      // Update solver options with remaining budget for phase-2
      auto phase2_opts =
          build_level_sddp_opts(level.sddp_options, remaining_budget);
      phase2_opts.save_per_iteration = level_opts.save_per_iteration;
      current_solver->mutable_options() = phase2_opts;
      current_solver->clear_stop();

      SPDLOG_INFO(
          "Cascade [{}]: re-solving without inherited cuts "
          "(max_iters={})",
          level_name,
          phase2_opts.max_iterations);

      result = current_solver->solve(opts);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
    }

    const double level_elapsed = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - t_level)
                                     .count();

    m_all_results_.insert(m_all_results_.end(), result->begin(), result->end());

    // The SDDP solver returns N training iterations + 1 simulation pass.
    // Only the N iterations count towards the global budget.
    const auto level_iterations =
        static_cast<int>(result->size()) - 1;  // exclude final fwd

    // ── Collect per-level stats ──
    const auto& last = result->back();
    const int total_cuts =
        std::accumulate(result->begin(),
                        result->end(),
                        0,
                        [](int sum, const SDDPIterationResult& r)
                        { return sum + r.cuts_added; });

    m_level_stats_.push_back(CascadeLevelStats {
        .name = level_name,
        .iterations = level_iterations,
        .lower_bound = last.lower_bound,
        .upper_bound = last.upper_bound,
        .gap = last.gap,
        .converged = last.converged,
        .elapsed_s = level_elapsed,
        .cuts_added = total_cuts,
    });

    SPDLOG_INFO(
        "Cascade [{}]: {} iters, LB={:.6g}, UB={:.6g}, "
        "gap={:.4g}, cuts={}, {:.3f}s{}",
        level_name,
        level_iterations,
        last.lower_bound,
        last.upper_bound,
        last.gap,
        total_cuts,
        level_elapsed,
        last.converged ? " (converged)" : "");

    // Update remaining global budget
    if (remaining_budget >= 0) {
      remaining_budget = std::max(0, remaining_budget - level_iterations);
      SPDLOG_INFO("Cascade: remaining global budget = {}", remaining_budget);
    }

    // ── 4. Check convergence ──
    if (last.converged) {
      if (level_idx == m_cascade_opts_.level_array.size() - 1) {
        break;  // converged at final level
      }
      // Converged at intermediate level — continue to next level
    }

    // Check if global budget is exhausted
    if (remaining_budget == 0) {
      SPDLOG_INFO("Cascade: global iteration budget exhausted at level [{}]",
                  level_name);
      break;
    }

    // ── 5. Extract state for next level ──
    if (level_idx + 1 < m_cascade_opts_.level_array.size()) {
      prev_targets = collect_named_targets(*current_solver, *current_lp);

      // Update stored cut duals from the last LP solution
      current_solver->update_stored_cut_duals();
      prev_cuts = current_solver->stored_cuts();
      prev_lp = current_lp;
    }
  }

  const bool converged =
      !m_all_results_.empty() && m_all_results_.back().converged;

  // ── Print per-level summary ──
  SPDLOG_INFO("Cascade: ═══ Summary ═══");
  for (const auto& ls : m_level_stats_) {
    SPDLOG_INFO(
        "  [{}]: {} iters, LB={:.6g}, UB={:.6g}, gap={:.4g}, "
        "cuts={}, {:.3f}s{}",
        ls.name,
        ls.iterations,
        ls.lower_bound,
        ls.upper_bound,
        ls.gap,
        ls.cuts_added,
        ls.elapsed_s,
        ls.converged ? " (converged)" : "");
  }
  SPDLOG_INFO("Cascade: {} levels, {} total results (converged={})",
              m_level_stats_.size(),
              m_all_results_.size(),
              converged);

  return converged ? 1 : 0;
}

}  // namespace gtopt
