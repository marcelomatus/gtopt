/**
 * @file      cascade_method.cpp
 * @brief     Multi-level cascade method implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/cascade_method.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace gtopt
{

// ─── Constructor ────────────────────────────────────────────────────────────

CascadePlanningMethod::CascadePlanningMethod(
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

auto CascadePlanningMethod::build_level_sddp_opts(
    const std::optional<CascadeLevelMethod>& level_solver,
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
  if (cs.scale_alpha.has_value()) {
    opts.scale_alpha = *cs.scale_alpha;
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

auto CascadePlanningMethod::clone_planning_with_overrides(
    const Planning& source, const ModelOptions& model_opts) -> Planning
{
  Planning copy = source;

  copy.options.model_options.merge(model_opts);
  return copy;
}

// ─── Collect state variable targets ─────────────────────────────────────────

auto CascadePlanningMethod::collect_state_targets(const SDDPMethod& solver,
                                                  const PlanningLP& planning_lp)
    -> std::vector<StateTarget>
{
  std::vector<StateTarget> targets;

  for (auto&& [scene, _sc] :
       enumerate<SceneIndex>(planning_lp.simulation().scenes()))
  {
    const auto& scene_states = solver.phase_states(scene);
    for (auto&& [phase, _ph] :
         enumerate<PhaseIndex>(planning_lp.simulation().phases()))
    {
      const auto& state = scene_states[phase];
      const auto& sv_map =
          planning_lp.simulation().state_variables(scene, phase);

      // Each outgoing link carries a source_col; we match it to the
      // registered state variable.  The per-solve col value is read from
      // `StateVariable::col_sol()` — which `capture_state_variable_values`
      // populates in SDDPMethod after every forward solve.
      for (const auto& [key, svar] : sv_map) {
        const auto col = svar.col();

        // Only collect variables that are outgoing links (state transfer)
        const bool is_outgoing = std::ranges::any_of(
            state.outgoing_links,
            [col](const auto& link) { return link.source_col == col; });
        if (!is_outgoing) {
          continue;
        }

        targets.push_back({
            .class_name = std::string(key.class_name),
            .col_name = std::string(key.col_name),
            .uid = key.uid,
            .context = svar.context(),
            .scene_index = scene,
            .phase_index = phase,
            .target_value = svar.col_sol(),
            .var_scale = svar.var_scale(),
        });
      }
    }
  }

  SPDLOG_INFO("Cascade: collected {} state variable targets", targets.size());
  return targets;
}

// ─── Add elastic target constraints ─────────────────────────────────────────

void CascadePlanningMethod::add_elastic_targets(
    PlanningLP& planning_lp,
    const std::vector<StateTarget>& targets,
    const CascadeTransition& transition)
{
  const double rtol = transition.target_rtol.value_or(0.05);
  const double min_atol = transition.target_min_atol.value_or(1.0);
  const double penalty = transition.target_penalty.value_or(500.0);

  int added = 0;
  int skipped = 0;

  for (const auto& t : targets) {
    // Look up by structured key (class_name, col_name, uid) in the
    // target level's state variable map — no LP name strings needed.
    const auto& sv_map =
        planning_lp.simulation().state_variables(t.scene_index, t.phase_index);

    // Find the matching state variable by (class_name, col_name, uid).
    // The key may differ in scenario_uid/stage_uid between levels, so
    // match only the stable identity fields.
    ColIndex resolved_col {unknown_index};
    for (const auto& [key, svar] : sv_map) {
      if (key.class_name == t.class_name && key.col_name == t.col_name
          && key.uid == t.uid)
      {
        resolved_col = svar.col();
        break;
      }
    }

    if (resolved_col == ColIndex {unknown_index}) {
      ++skipped;
      SPDLOG_DEBUG(
          "Cascade: target not found in next level "
          "(class={}, var={}, uid={})",
          t.class_name,
          t.col_name,
          static_cast<int>(t.uid));
      continue;
    }

    const double atol = std::max(rtol * std::abs(t.target_value), min_atol);

    auto& li =
        planning_lp.system(t.scene_index, t.phase_index).linear_interface();

    // Add slack columns for elastic penalty
    const auto sup_col = li.add_col(SparseCol {
        .uppb = DblMax,
        .cost = penalty,
        .class_name = "Cascade",
        .variable_name = "tgt_sup",
    });
    const auto sdn_col = li.add_col(SparseCol {
        .uppb = DblMax,
        .cost = penalty,
        .class_name = "Cascade",
        .variable_name = "tgt_sdn",
    });

    // Add constraint: x - s⁺ + s⁻ ∈ [target - atol, target + atol]
    SparseRow row;
    row.class_name = "Cascade";
    row.constraint_name = "target";
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

void CascadePlanningMethod::clear_all_cuts(PlanningLP& planning_lp,
                                           const SDDPMethod& solver)
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
        auto indices = std::ranges::to<std::vector>(iota_range(base, current));
        li.delete_rows(indices);
        total_removed += current - base;
      }
    }
  }

  SPDLOG_INFO("Cascade: cleared {} cut rows from LPs", total_removed);
}

// ─── Main solve orchestration ───────────────────────────────────────────────

auto CascadePlanningMethod::solve(PlanningLP& planning_lp,
                                  const SolverOptions& opts)
    -> std::expected<int, Error>
{
  PlanningLP* current_lp = nullptr;
  std::unique_ptr<SDDPMethod> current_solver;
  std::vector<StateTarget> prev_targets;
  ModelOptions prev_effective_model = m_cascade_opts_.model_options;

  // Global iteration budget: cascade sddp_options.max_iterations applies
  // to the sum of all level iterations.  -1 means no global cap.
  const auto& cascade_max_iter = m_cascade_opts_.sddp_options.max_iterations;
  int remaining_budget = cascade_max_iter.has_value() ? *cascade_max_iter : -1;

  // Running iteration index at which the NEXT level should start.
  // Fed into each level's SDDPOptions::iteration_offset_hint so that level
  // N's iteration indices start strictly past level N-1's — giving every
  // level a disjoint range in m_cut_store_ (no collisions on
  // save_cuts_for_iteration) and a globally monotonic `[i{}]` in logs.
  IterationIndex global_iter_index {};

  SPDLOG_INFO("Cascade: starting with {} levels (global budget={})",
              m_cascade_opts_.level_array.size(),
              remaining_budget);

  for (std::size_t level_idx = 0;
       level_idx < m_cascade_opts_.level_array.size();
       ++level_idx)
  {
    const auto& level = m_cascade_opts_.level_array[level_idx];
    const auto level_name = level.name.value_or(as_label("level", level_idx));

    // ── 0. Skip inactive level ──
    // `active = false` disables the level entirely: no LP build, no solve,
    // no state/cut production.  State and cut files left over from prior
    // active levels are preserved untouched so a later active level still
    // sees the latest upstream data.
    if (!level.active.value_or(true)) {
      SPDLOG_INFO("Cascade [{}]: inactive (active=false), skipping",
                  level_name);
      continue;
    }

    // ── 1. Build LP for each level ──
    // Intermediate/final levels always build a fresh LP to ensure clean
    // state (no leftover target constraints or alpha variables from
    // previous levels).  The first level reuses the caller-supplied
    // PlanningLP when the caller's options already cover the level-0
    // effective model — the caller pre-merges cascade level-0 overrides in
    // build_solve_and_output(), so the initial LP is normally compatible.
    auto effective_model = prev_effective_model;
    if (level.model_options.has_value()) {
      effective_model.merge(*level.model_options);
    }
    prev_effective_model = effective_model;

    if (level_idx == 0
        && planning_lp.planning().options.model_options.covers(effective_model))
    {
      current_lp = &planning_lp;
      current_solver.reset();
      SPDLOG_INFO("Cascade [{}]: reusing caller PlanningLP", level_name);
    } else {
      auto modified_planning = clone_planning_with_overrides(
          planning_lp.planning(), effective_model);
      // State variable transfer uses structured keys — no LP names needed.
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

    // Route each level's output to a level-specific subdirectory so
    // per-scene cut files and state files don't collide between levels.
    // Final level keeps the original path for backward compatibility.
    const bool is_last_level =
        level_idx == m_cascade_opts_.level_array.size() - 1;
    if (!m_base_opts_.cuts_output_file.empty()) {
      const auto base_dir =
          std::filesystem::path(m_base_opts_.cuts_output_file).parent_path();
      const auto base_name =
          std::filesystem::path(m_base_opts_.cuts_output_file).filename();
      if (is_last_level) {
        level_opts.cuts_output_file = m_base_opts_.cuts_output_file;
      } else {
        const auto level_dir = base_dir / level_name;
        std::filesystem::create_directories(level_dir);
        level_opts.cuts_output_file = (level_dir / base_name).string();
      }
    }

    // Intermediate levels save per-iteration so that their cuts and
    // state are available for cascade hot-start.  Final level uses
    // the base option.
    level_opts.save_per_iteration =
        is_last_level ? m_base_opts_.save_per_iteration : true;

    // Seed the level's iteration counter past all iterations that prior
    // levels consumed.  Hot-start cuts loaded below (via load_cuts) may
    // raise this further through std::max in initialize_solver.
    level_opts.iteration_offset_hint = global_iter_index;

    // Always create a fresh solver for each level, ensuring clean state.
    current_solver = std::make_unique<SDDPMethod>(*current_lp, level_opts);

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
    // Cuts were pre-filtered and serialized at the end of the previous
    // level (step 5) so that the previous level's LP/solver could be
    // released before this level built its own LP.  Here we just load
    // the resulting file into the new solver.
    int inherited_cut_count = 0;
    if (level.transition && level_idx > 0 && !m_prev_cuts_file_.empty()
        && std::filesystem::exists(m_prev_cuts_file_))
    {
      // Ensure alpha variables are added before resolving cut column names
      if (auto init_err = current_solver->ensure_initialized();
          !init_err.has_value())
      {
        return std::unexpected(init_err.error());
      }
      // Route through the new solver so that state-variable resolution
      // (including the alpha column, which is registered as a state
      // variable by initialize_alpha_variables) runs against the new
      // level's sv_map.
      auto load_result = current_solver->load_cuts(m_prev_cuts_file_);
      if (load_result.has_value()) {
        inherited_cut_count = load_result->count;
        SPDLOG_INFO("Cascade [{}]: transferred {} cuts from {}",
                    level_name,
                    load_result->count,
                    m_prev_cuts_file_);
      } else {
        SPDLOG_WARN("Cascade [{}]: cut load failed: {}",
                    level_name,
                    load_result.error().message);
      }
      std::error_code ec;
      std::filesystem::remove(m_prev_cuts_file_, ec);
      m_prev_cuts_file_.clear();
    }

    // ── Load previous level's state variable solutions ──
    // When a previous level saved state to a temp file, load it into the
    // new level's PlanningLP via name-based resolution.  This seeds the
    // warm column solutions so that update_lp elements (e.g.
    // ReservoirProductionFactorLP) use physical volumes from the previous
    // level rather than falling back to default JSON values.
    if (level_idx > 0 && !m_prev_state_file_.empty()
        && std::filesystem::exists(m_prev_state_file_))
    {
      auto state_load = load_state_csv(*current_lp, m_prev_state_file_);
      if (state_load.has_value()) {
        SPDLOG_INFO("Cascade [{}]: loaded state from previous level",
                    level_name);
      } else {
        SPDLOG_WARN("Cascade [{}]: could not load state: {}",
                    level_name,
                    state_load.error().message);
      }
      // Clean up temp file
      std::error_code ec;
      std::filesystem::remove(m_prev_state_file_, ec);
      m_prev_state_file_.clear();
    }

    // ── Cut forgetting: iteration-aware inherited cut lifecycle ──
    // inherit_optimality_cuts semantics:
    //   0 or absent: do not inherit
    //   -1:          inherit and keep forever
    //   N > 0:       inherit but forget after N training iterations,
    //                then re-solve with only self-generated cuts
    //
    // Only optimality cuts are inheritable across levels.  Feasibility
    // cuts are installed by the forward pass and tied to a level's own
    // trial values, so carrying them across levels is not meaningful.
    int forget_threshold = 0;
    if (level.transition && inherited_cut_count > 0) {
      const int opt_mode =
          level.transition->inherit_optimality_cuts.value_or(0);
      if (opt_mode > 0) {
        forget_threshold = opt_mode;
      }
    }

    // If forget_threshold > 0, cap phase-1 to that many iterations.
    // Only update max_iterations — do NOT reassign the full options,
    // as that would overwrite auto-computed fields (e.g. scale_alpha
    // auto-set during initialize_solver()).
    if (forget_threshold > 0) {
      current_solver->mutable_options().max_iterations =
          std::min(level_opts.max_iterations, forget_threshold);
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

      // Update solver options with remaining budget for phase-2.
      // Preserve auto-computed fields (scale_alpha, etc.) by updating
      // only the fields that need to change, not the full struct.
      auto phase2_opts =
          build_level_sddp_opts(level.sddp_options, remaining_budget);
      auto& solver_opts = current_solver->mutable_options();
      solver_opts.max_iterations = phase2_opts.max_iterations;
      solver_opts.save_per_iteration = level_opts.save_per_iteration;
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

    // Advance the global iteration index past every index this level
    // produced — `last.iteration_index` is the simulation-pass slot at
    // `next(last training iteration)`, so `next()` of that is strictly
    // past even the discarded sim index.
    global_iter_index = next(last.iteration_index);

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

    // ── 5. Extract state for next level + explicit cleanup ──
    // Only prepare state/cut transfer and release LP cells when an
    // ACTIVE subsequent level exists.  If every remaining level is
    // inactive, skip this block entirely so the caller's PlanningLP
    // retains its systems for write_out() to emit solution parquets.
    // Without this guard, level 0 would release its cells in
    // anticipation of level 1, only to find levels 1..N all inactive —
    // leaving write_out with an empty system grid.
    const bool has_active_successor = std::ranges::any_of(
        std::span(m_cascade_opts_.level_array).subspan(level_idx + 1),
        [](const CascadeLevel& l) { return l.active.value_or(true); });
    if (level_idx + 1 < m_cascade_opts_.level_array.size()
        && has_active_successor)
    {
      prev_targets = collect_state_targets(*current_solver, *current_lp);

      // Save state variable solutions to a temp file for inter-level
      // transfer.  The next level loads these via name-based resolution,
      // which handles different LP column layouts between levels.
      const auto state_tmp = std::filesystem::temp_directory_path()
          / std::format("cascade_state_{}_{}.csv",
                        static_cast<std::int64_t>(::getpid()),
                        level_idx);
      auto state_save =
          save_state_csv(*current_lp, state_tmp.string(), IterationIndex {0});
      if (state_save.has_value()) {
        m_prev_state_file_ = state_tmp.string();
        SPDLOG_INFO("Cascade [{}]: saved state for next level to {}",
                    level_name,
                    state_tmp.string());
      } else {
        m_prev_state_file_.clear();
        SPDLOG_WARN("Cascade [{}]: could not save state: {}",
                    level_name,
                    state_save.error().message);
      }

      // Pre-filter and serialize cuts to disk while the current LP is
      // still alive.  Doing this here (not at next-level start) lets us
      // drop the solver + owned LP before level N+1 allocates a new LP,
      // halving peak memory on the level boundary.
      m_prev_cuts_file_.clear();
      const auto& next_level = m_cascade_opts_.level_array[level_idx + 1];
      if (next_level.transition) {
        current_solver->update_stored_cut_duals();
        auto stored_cuts = current_solver->stored_cuts();
        const auto& trans = *next_level.transition;
        const int opt_cut_mode = trans.inherit_optimality_cuts.value_or(0);
        const bool want_opt_cuts = opt_cut_mode != 0;

        // Only optimality cuts are inheritable across levels.  Feasibility
        // cuts are regenerated by the forward pass as needed.
        if (want_opt_cuts && !stored_cuts.empty()) {
          const double dual_threshold =
              trans.optimality_dual_threshold.value_or(0.0);
          std::vector<StoredCut> filtered;
          filtered.reserve(stored_cuts.size());
          int skipped_by_dual = 0;
          for (const auto& cut : stored_cuts) {
            if (cut.type == CutType::Optimality) {
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
            const auto cuts_tmp = std::filesystem::temp_directory_path()
                / std::format("cascade_cuts_{}_{}.csv",
                              static_cast<std::int64_t>(::getpid()),
                              level_idx + 1);
            auto save_result =
                save_cuts_csv(filtered, *current_lp, cuts_tmp.string());
            if (save_result.has_value()) {
              m_prev_cuts_file_ = cuts_tmp.string();
              SPDLOG_INFO("Cascade [{}]: serialized {} cuts for next level",
                          level_name,
                          filtered.size());
            } else {
              SPDLOG_WARN("Cascade [{}]: cut save failed: {}",
                          level_name,
                          save_result.error().message);
            }
          }
        }
      }

      // ── EXPLICIT CLEANUP before next level allocates its LP ──
      // Drop the solver (frees aperture subproblems + scene_phase_states_)
      // and the owned LP (frees the per-cell LP matrices).  When the
      // caller's PlanningLP was reused at this level, also release its
      // per-(scene, phase) cells so the shell stays alive but the heavy
      // solver backends are returned to the allocator — otherwise the
      // caller's LP sits resident while the next level doubles memory.
      const auto owned_before = m_owned_lps_.size();
      const bool released_caller = (current_lp == &planning_lp);
      current_solver.reset();
      current_lp = nullptr;
      m_owned_lps_.clear();
      m_owned_lps_.shrink_to_fit();
      if (released_caller) {
        planning_lp.release_cells();
      }
      SPDLOG_INFO(
          "Cascade [{}]: released solver, {} owned LP(s){} before level [{}]",
          level_name,
          owned_before,
          released_caller ? " and caller LP cells" : "",
          m_cascade_opts_.level_array[level_idx + 1].name.value_or(
              as_label("level", level_idx + 1)));
    }
  }

  // ── Transfer the final level's LP to the caller for write_out ──
  // If the final active level built its own PlanningLP (i.e. the
  // caller's cells were released at a prior level→level cleanup, so
  // `planning_lp.systems()` is now empty), that LP lives in
  // `m_owned_lps_` and would be destroyed together with this
  // CascadePlanningMethod instance.  Without this transfer,
  // `gtopt_lp_runner` would call `planning_lp.write_out()` on an
  // empty system grid and emit only `planning.json` — solution.csv
  // stays header-only and element parquets are never written.
  //
  // Hand the owned LP over to the caller via the new output-delegate
  // channel so `write_out()` forwards to it, producing the full
  // per-(scene, phase) output.
  if (current_lp != nullptr && current_lp != &planning_lp) {
    auto it = std::ranges::find_if(m_owned_lps_,
                                   [current_lp](const auto& p)
                                   { return p.get() == current_lp; });
    if (it != m_owned_lps_.end()) {
      auto owned = std::move(*it);
      m_owned_lps_.erase(it);
      planning_lp.set_output_delegate(std::move(owned));
      SPDLOG_INFO(
          "Cascade: transferred final-level LP to caller as write_out "
          "delegate");
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
