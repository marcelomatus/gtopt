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

#include <daw/daw_read_file.h>
#include <gtopt/as_label.hpp>
#include <gtopt/cascade_method.hpp>
#include <gtopt/cascade_progress.hpp>
#include <gtopt/constraint_names.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_common.hpp>  // gtopt::format_si
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/system_file_loader.hpp>
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
  // remaining_budget, not a per-level default.  `value_or(current)` is
  // the `double ← optional<double>` equivalent of `merge_opt`
  // (which only handles optional → optional).
  const auto& cs = m_cascade_opts_.sddp_options;
  opts.convergence_tol = cs.convergence_tol.value_or(opts.convergence_tol);
  opts.min_iterations = cs.min_iterations.value_or(opts.min_iterations);
  opts.elastic_penalty = cs.elastic_penalty.value_or(opts.elastic_penalty);
  opts.scale_alpha = cs.scale_alpha.value_or(opts.scale_alpha);
  opts.stationary_tol = cs.stationary_tol.value_or(opts.stationary_tol);
  opts.stationary_gap_ceiling =
      cs.stationary_gap_ceiling.value_or(opts.stationary_gap_ceiling);

  // Apply per-level overrides
  if (level_solver) {
    opts.max_iterations =
        level_solver->max_iterations.value_or(opts.max_iterations);
    if (level_solver->apertures.has_value()) {
      opts.apertures = level_solver->apertures;
    }
    if (level_solver->num_apertures.has_value()) {
      opts.num_apertures = level_solver->num_apertures;
    }
    if (level_solver->aperture_selection_mode.has_value()) {
      opts.aperture_selection_mode = gtopt::require_enum<ApertureSelectionMode>(
          "aperture_selection_mode", *level_solver->aperture_selection_mode);
    }
    if (level_solver->aperture_solve_mode.has_value()) {
      opts.aperture_solve_mode = gtopt::require_enum<ApertureSolveMode>(
          "aperture_solve_mode", *level_solver->aperture_solve_mode);
    }
    opts.aperture_chunk_size =
        level_solver->aperture_chunk_size.value_or(opts.aperture_chunk_size);
    opts.min_iterations =
        level_solver->min_iterations.value_or(opts.min_iterations);
    opts.convergence_tol =
        level_solver->convergence_tol.value_or(opts.convergence_tol);
    opts.stationary_tol =
        level_solver->stationary_tol.value_or(opts.stationary_tol);
    opts.stationary_gap_ceiling = level_solver->stationary_gap_ceiling.value_or(
        opts.stationary_gap_ceiling);
    opts.stationary_window =
        level_solver->stationary_window.value_or(opts.stationary_window);
    if (level_solver->elastic_mode.has_value()) {
      opts.elastic_filter_mode = gtopt::require_enum<ElasticFilterMode>(
          "elastic_mode", *level_solver->elastic_mode);
    }
    opts.elastic_penalty =
        level_solver->elastic_penalty.value_or(opts.elastic_penalty);
  }

  // Cap max_iterations to the remaining global budget
  if (remaining_budget >= 0) {
    opts.max_iterations = std::min(opts.max_iterations, remaining_budget);
  }

  return opts;
}

// ─── Clone Planning with LP overrides ───────────────────────────────────────
//
// `resolve_system_file` / `load_system_from_file` now live in
// `gtopt/system_file_loader.hpp` (shared with the SDDP aperture-system
// feature).  The cascade `system_file` path keeps using the system-only
// loader; its own `model_options` overlay remains authoritative.

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

  // Pre-count state variables for capacity reservation.
  size_t total_sv = 0;
  for (auto&& [si, _sc] :
       enumerate<SceneIndex>(planning_lp.simulation().scenes()))
  {
    for (auto&& [pi, _ph] :
         enumerate<PhaseIndex>(planning_lp.simulation().phases()))
    {
      total_sv += planning_lp.simulation().state_variables(si, pi).size();
    }
  }
  targets.reserve(total_sv);

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

        // PHYSICAL space target.  `svar.col_sol()` is the source phase's
        // LP-raw value (`physical / var_scale_source`).  The cascade row
        // installed below uses `set_col_low` / `set_col_upp` semantics:
        // the row coefficient `1.0` on the target's dependent column
        // gets multiplied by `col_scale_target` inside
        // `LinearInterface::add_row` (post-flatten cut-phase compose),
        // so the row reads in PHYSICAL space — therefore the bound
        // `target_value ± atol` must also be physical.  Reading via
        // `col_sol_physical()` (= `col_sol() × var_scale_source`) is
        // scale-agnostic and matches the
        // `propagate_trial_values(span, target_li)` overload's
        // physical-bound-pin convention used elsewhere in the SDDP
        // pipeline (benders_cut.cpp:90-94).
        targets.push_back({
            .class_name = std::string(key.class_name),
            .col_name = std::string(key.col_name),
            .uid = key.uid,
            .context = svar.context(),
            .scene_index = scene,
            .phase_index = phase,
            .target_value = svar.col_sol_physical(),
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

  // Per-cell accumulator: collect every target's slack pair + row
  // before installing.  Same shape as the SDDP cut loaders (commit
  // 08f0202a / 1e9b6a9c) — saves O(N) backend round-trips per
  // cascade transition by issuing one bulk `add_cols` (slacks) and
  // one bulk `add_rows` (target rows) per (scene, phase) cell.
  //
  // The metadata-based duplicate detector keys columns on
  // `(class, variable, uid, context)`; the per-target `(t.uid,
  // t.context)` pair already disambiguates `tgt_sup` / `tgt_sdn`
  // across targets, so no further keying is needed here.
  using CellKey = std::pair<SceneIndex, PhaseIndex>;
  struct PendingTarget
  {
    Uid uid {unknown_uid};  ///< target identity (carries through to row label)
    LpContext context {};  ///< target context (idem)
    ColIndex resolved_col {unknown_index};  ///< target's state-var column
    double lowb {0.0};
    double uppb {0.0};
  };
  flat_map<CellKey, std::vector<PendingTarget>> accum;

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
          t.uid);
      continue;
    }

    const double atol = std::max(rtol * std::abs(t.target_value), min_atol);
    accum[std::make_pair(t.scene_index, t.phase_index)].push_back(
        PendingTarget {
            .uid = t.uid,
            .context = t.context,
            .resolved_col = resolved_col,
            .lowb = t.target_value - atol,
            .uppb = t.target_value + atol,
        });
  }

  // Per-cell bulk install.  `auto&&` because `gtopt::flat_map`
  // (std::flat_map under GCC 15) yields a proxy `pair<key&, value&>`.
  for (auto&& [cell_key, pendings] : accum) {
    if (pendings.empty()) {
      continue;
    }
    const auto [scene_index, phase_index] = cell_key;
    auto& li = planning_lp.system(scene_index, phase_index).linear_interface();

    // Step 1: bulk-add 2N slack columns (sup / sdn pair per target).
    // The metadata-based duplicate detector (`f21641f9`) uses
    // `(class, variable, uid, context)` as the dedup key; the
    // per-target `(p.uid, p.context)` already disambiguates each
    // `tgt_sup` / `tgt_sdn` slack across targets.
    std::vector<SparseCol> slack_cols;
    slack_cols.reserve(pendings.size() * 2);
    for (const auto& p : pendings) {
      slack_cols.push_back(SparseCol {
          .uppb = DblMax,
          .cost = penalty,
          .class_name = cascade_class_name,
          .variable_name = "tgt_sup",
          .variable_uid = p.uid,
          .context = p.context,
      });
      slack_cols.push_back(SparseCol {
          .uppb = DblMax,
          .cost = penalty,
          .class_name = cascade_class_name,
          .variable_name = "tgt_sdn",
          .variable_uid = p.uid,
          .context = p.context,
      });
    }
    const auto first_slack_col = li.get_numcols();
    (void)li.add_cols(slack_cols);  // NOLINT

    // Mirror the slacks into the persistent `m_dynamic_cols_` registry
    // so `apply_post_load_replay` re-adds them on every
    // `release_backend()` → `reconstruct_backend()` cycle under
    // low-memory compress mode (commit a2b5a4fb).  The slacks are
    // added AFTER the initial `defer_initial_load` snapshot, so
    // without this mirror they would be dropped on the first reload.
    for (const auto& col : slack_cols) {
      li.record_dynamic_col(col);
    }

    // Step 2: build target rows referencing the freshly-allocated
    // slack column indices, then bulk add_rows + record each row
    // in the persistent dynamic-rows registry (same replay rationale).
    //
    // Constraint per target: `x − s⁺ + s⁻ ∈ [target − atol, target + atol]`.
    std::vector<SparseRow> rows;
    rows.reserve(pendings.size());
    for (size_t i = 0; i < pendings.size(); ++i) {
      const auto& p = pendings[i];
      const auto sup_col = ColIndex {first_slack_col + static_cast<int>(2 * i)};
      const auto sdn_col =
          ColIndex {first_slack_col + static_cast<int>(2 * i) + 1};

      SparseRow row;
      row.class_name = cascade_class_name;
      row.constraint_name = cascade_target_constraint_name;
      row.variable_uid = p.uid;
      row.context = p.context;
      row.lowb = p.lowb;
      row.uppb = p.uppb;
      row[p.resolved_col] = 1.0;
      row[sup_col] = -1.0;
      row[sdn_col] = 1.0;
      rows.push_back(std::move(row));
    }
    li.add_rows(rows);
    for (const auto& row : rows) {
      li.record_dynamic_row(row);
    }
    added += static_cast<int>(pendings.size());
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
      const auto current = li.get_numrows();

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

  // Locate the LAST ACTIVE level — its SDDP sim pass keeps writing
  // per-cell outputs; every earlier active level skips its sim-pass
  // write_out (the cells would be overwritten anyway).  Returns
  // `size()` when every level is inactive (loop body below never
  // executes, so the value is irrelevant).
  const std::size_t last_active_level_idx = [&]
  {
    for (std::size_t i = m_cascade_opts_.level_array.size(); i-- > 0;) {
      if (m_cascade_opts_.level_array[i].active.value_or(true)) {
        return i;
      }
    }
    return m_cascade_opts_.level_array.size();
  }();

  // ── Cascade checkpoint / resume bookkeeping ──
  //
  // Sidecar `cascade_progress.json` next to the per-level cut directories
  // tracks which levels have finished.  When a previous run died mid-
  // cascade, `--recover` (= `recovery_mode != none`) loads this file,
  // identifies the first non-done level, and skips earlier ones by
  // reusing their persisted cuts + state_targets — no LP rebuild for the
  // completed prefix.
  const std::filesystem::path progress_path = [&]() -> std::filesystem::path
  {
    if (m_base_opts_.cuts_output_file.empty()) {
      return {};  // no output dir → checkpointing disabled
    }
    return std::filesystem::path(m_base_opts_.cuts_output_file).parent_path()
        / "cascade_progress.json";
  }();
  const bool progress_enabled = !progress_path.empty();

  CascadeProgress progress;
  progress.run_id = std::format(
      "{}", std::chrono::system_clock::now().time_since_epoch().count());
  progress.levels.resize(m_cascade_opts_.level_array.size());
  for (std::size_t i = 0; i < m_cascade_opts_.level_array.size(); ++i) {
    auto& slot = progress.levels[i];
    slot.index = i;
    slot.name = std::string(
        m_cascade_opts_.level_array[i].name.value_or(as_label("level", i)));
  }

  std::size_t resume_idx = 0;
  std::vector<StateTarget> resumed_prev_targets;
  std::string resume_inherited_cuts_file;  // → m_prev_cuts_file_
  const bool recovery_enabled =
      m_base_opts_.recovery_mode != RecoveryMode::none;
  if (recovery_enabled && progress_enabled
      && std::filesystem::exists(progress_path))
  {
    auto loaded = load_cascade_progress(progress_path);
    if (loaded.has_value()) {
      // Hydrate our in-memory progress with the persisted state, keyed by
      // index.  Levels added/removed since the previous run are simply
      // ignored: name+index drift is recovery's only safety net for now.
      for (const auto& lp : loaded->levels) {
        if (lp.index < progress.levels.size()
            && progress.levels[lp.index].name == lp.name)
        {
          auto& slot = progress.levels[lp.index];
          slot.status = lp.status;
          slot.converged = lp.converged;
          slot.iters = lp.iters;
          slot.global_iter_after = lp.global_iter_after;
          slot.cuts_file = lp.cuts_file;
          slot.state_targets_file = lp.state_targets_file;
        }
      }
      // First ACTIVE level that isn't `done`.  Inactive levels are
      // configured-out, never counted as a resume target — otherwise an
      // `[L0 done, L1 inactive, L2 done]` checkpoint would resume at L1,
      // fall through to the inactive-skip, and then re-solve L2 from
      // scratch.  If every active level is done, resume_idx == size() and
      // the loop below exits without doing any work.
      //
      // `last_done_idx` walks alongside: it's the most recent active+done
      // level whose state_targets/cuts_file we should feed into the
      // resume level's transition.
      resume_idx = progress.levels.size();
      std::size_t last_done_idx = progress.levels.size();
      for (std::size_t i = 0; i < progress.levels.size(); ++i) {
        const auto& cfg = m_cascade_opts_.level_array[i];
        if (!cfg.active.value_or(true)) {
          continue;
        }
        if (progress.levels[i].status != CascadeLevelStatus::done) {
          resume_idx = i;
          break;
        }
        last_done_idx = i;
      }
      // Restore global_iter_index from the latest done level's checkpoint so
      // future cuts/iterations land past every iter the previous run
      // already consumed.
      if (last_done_idx < progress.levels.size()) {
        const auto& last_done = progress.levels[last_done_idx];
        if (last_done.global_iter_after >= 0) {
          global_iter_index = IterationIndex {last_done.global_iter_after};
        }
        if (!last_done.state_targets_file.empty()
            && std::filesystem::exists(last_done.state_targets_file))
        {
          auto targets = load_state_targets(last_done.state_targets_file);
          if (targets.has_value()) {
            resumed_prev_targets = std::move(*targets);
          } else {
            SPDLOG_WARN(
                "Cascade --recover: could not load state_targets '{}': {}",
                last_done.state_targets_file,
                targets.error().message);
          }
        }
        if (!last_done.cuts_file.empty()
            && std::filesystem::exists(last_done.cuts_file))
        {
          resume_inherited_cuts_file = last_done.cuts_file;
        }
      }
      if (resume_idx >= progress.levels.size()) {
        SPDLOG_INFO(
            "Cascade --recover: all {} level(s) already complete — "
            "nothing to do",
            progress.levels.size());
      } else {
        SPDLOG_INFO(
            "Cascade --recover: resuming from level [{}] (index {}/{}), "
            "global_iter_index={}",
            progress.levels[resume_idx].name,
            resume_idx,
            progress.levels.size(),
            value_of(global_iter_index));
      }
    } else {
      SPDLOG_WARN(
          "Cascade --recover: progress file '{}' present but "
          "unreadable: {} — cold start",
          progress_path.string(),
          loaded.error().message);
    }
  } else if (recovery_enabled && progress_enabled) {
    SPDLOG_INFO("Cascade --recover: no checkpoint at '{}' — cold start",
                progress_path.string());
  }

  // Loop-scoped kappa carry-forward.  Captured BEFORE the per-level
  // ``current_solver.reset()`` at the end of each iteration, so the
  // next level can seed its solver's ``m_seed_max_kappa_`` with the
  // previous level's value — keeps the ``kappa=…`` clause in the iter
  // log alive across cascade levels even when CPLEX barrier without
  // crossover leaves the new level's LPs without a queryable basis.
  // Sentinel ``-1.0`` = "no observation yet" (first level).
  double carry_kappa = -1.0;

  // Carried last-N (UB, LB) pairs from the previous cascade level,
  // installed on each new ``SDDPMethod`` via :func:`seed_prior_bounds`.
  // Lets iter 1 of L1+ exercise a real ``stationary_window``-iter
  // lookback against the previous level's tail instead of the
  // no-lookback 1.0 sentinel (or a 1-iter degraded lookback when the
  // seed was a single point).  Oldest-first ordering: the last
  // element is the previous level's most recent iter.  Empty for L0
  // (no prior level).  See the setter docstring on ``SDDPMethod`` for
  // the convergence-safety pairing with ``min_iterations >= 2``.
  std::vector<SDDPMethod::PriorIterBounds> carry_bounds {};

  // Seed the per-level transition state from the recovered checkpoint so
  // the first resumed level (resume_idx) sees the same `prev_targets` /
  // `m_prev_cuts_file_` it would have seen in the original run.  No-op
  // when resume_idx == 0 (cold start).
  if (!resumed_prev_targets.empty()) {
    prev_targets = std::move(resumed_prev_targets);
  }
  if (!resume_inherited_cuts_file.empty()) {
    m_prev_cuts_file_ = std::move(resume_inherited_cuts_file);
  }

  // Resolve the per-level subdirectory used for state_targets.json.
  // Mirrors the path layout chosen for `level_opts.cuts_output_file`
  // further down: intermediate levels write to `<base_dir>/<level_name>/`,
  // the final level keeps `<base_dir>/` (back-compat).  Returns "" when
  // no `cuts_output_file` is configured.
  const auto state_targets_file_for = [&](std::size_t idx) -> std::string
  {
    if (m_base_opts_.cuts_output_file.empty()) {
      return {};
    }
    const auto base_dir =
        std::filesystem::path(m_base_opts_.cuts_output_file).parent_path();
    const auto sub_name =
        m_cascade_opts_.level_array[idx].name.value_or(as_label("level", idx));
    const bool is_last = (idx == m_cascade_opts_.level_array.size() - 1);
    const auto level_dir = is_last ? base_dir : base_dir / sub_name;
    return (level_dir / "state_targets.json").string();
  };

  for (std::size_t level_idx = 0;
       level_idx < m_cascade_opts_.level_array.size();
       ++level_idx)
  {
    const auto& level = m_cascade_opts_.level_array[level_idx];
    const auto level_name = level.name.value_or(as_label("level", level_idx));

    // ── 0a. Skip already-completed level on --recover ──
    // Cuts + state_targets were persisted by the previous run.  We rely
    // on the disk artifacts for the transition into the resume level:
    // `prev_targets` was already seeded above; cut inheritance points at
    // the most recent done level's cuts file via `m_prev_cuts_file_`.
    if (level_idx < resume_idx) {
      SPDLOG_INFO(
          "Cascade [{}]: skipping (already complete in checkpoint, "
          "iters={}, converged={})",
          level_name,
          progress.levels[level_idx].iters,
          progress.levels[level_idx].converged ? "yes" : "no");
      continue;
    }

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

    // A non-empty system_file always forces a fresh LP build for this
    // level — the network topology is being swapped wholesale, so the
    // caller's pre-built PlanningLP cannot be reused.
    const bool has_system_swap =
        level.system_file.has_value() && !level.system_file->empty();

    if (level_idx == 0 && !has_system_swap
        && planning_lp.planning().options.model_options.covers(effective_model))
    {
      current_lp = &planning_lp;
      current_solver.reset();
      SPDLOG_INFO("Cascade [{}]: reusing caller PlanningLP", level_name);
    } else {
      auto modified_planning = clone_planning_with_overrides(
          planning_lp.planning(), effective_model);

      if (has_system_swap) {
        const auto& input_dir =
            planning_lp.planning().options.input_directory.value_or(
                std::string {});
        modified_planning.system =
            load_system_from_file(*level.system_file, input_dir);
        SPDLOG_INFO(
            "Cascade [{}]: swapped system from '{}' "
            "({} buses, {} lines, {} generators, {} demands)",
            level_name,
            *level.system_file,
            modified_planning.system.bus_array.size(),
            modified_planning.system.line_array.size(),
            modified_planning.system.generator_array.size(),
            modified_planning.system.demand_array.size());
      }

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

    // Skip the post-training simulation pass at every non-final-active
    // level: the sim pass at an intermediate level produces NOTHING
    // the cascade subsequently consumes — state-variable targets are
    // read from the last training forward pass, optimality cuts come
    // from the training backward passes, and the per-cell write_out
    // output (the sim pass's only persistent side-effect) would be
    // overwritten by the next level's sim pass anyway (per-element
    // output paths are shared across levels).  The last active level
    // keeps `skip_simulation_pass = false` so its simulation runs end-
    // to-end and its outputs land on disk under the configured
    // `output_directory`.  Filed as
    // https://github.com/marcelomatus/gtopt/issues/479 for the
    // accompanying feature flag that routes per-level output to
    // separate subdirectories — opting back in to the previous
    // behaviour will then require a config flip rather than a code
    // change here.
    level_opts.skip_simulation_pass = (level_idx != last_active_level_idx);

    // Seed the level's iteration counter past all iterations that prior
    // levels consumed.  Hot-start cuts loaded below (via load_cuts) may
    // raise this further through std::max in initialize_solver.
    level_opts.iteration_offset_hint = global_iter_index;

    // ── Resume: feed this level its own intra-level cuts checkpoint ──
    // When `--recover` lands on level_idx == resume_idx, the previous
    // run already persisted per-iteration cuts at
    // `level_opts.cuts_output_file`. Point the SDDP solver's `cuts_input_file`
    // at that path so its `initialize_solver` auto-loads the cuts and advances
    // the iteration counter past every iter the previous run consumed — the
    // saving continues seamlessly because `save_cuts_parquet` appends.
    //
    // We only override `cuts_input_file` when the user hasn't already
    // supplied one (rare for cascade) and the level's own checkpoint
    // exists on disk.
    if (recovery_enabled && level_idx == resume_idx
        && level_opts.cuts_input_file.empty()
        && !level_opts.cuts_output_file.empty()
        && std::filesystem::exists(level_opts.cuts_output_file))
    {
      level_opts.cuts_input_file = level_opts.cuts_output_file;
      SPDLOG_INFO("Cascade [{}]: hot-starting from intra-level checkpoint '{}'",
                  level_name,
                  level_opts.cuts_input_file);
    }

    // Record the level's cuts file path in the progress object now (so a
    // mid-solve crash leaves the next --recover with a working pointer
    // even before the level marks itself done).
    if (progress_enabled) {
      progress.levels[level_idx].cuts_file = level_opts.cuts_output_file;
      progress.levels[level_idx].state_targets_file =
          state_targets_file_for(level_idx);
      progress.levels[level_idx].status = CascadeLevelStatus::in_progress;
      if (auto save = save_cascade_progress(progress, progress_path);
          !save.has_value())
      {
        SPDLOG_WARN("Cascade: could not flush progress file '{}': {}",
                    progress_path.string(),
                    save.error().message);
      }
    }
    // Always create a fresh solver for each level, ensuring clean state.
    // Preserve the previous level's max kappa as an observability
    // baseline — CPLEX barrier without crossover may leave the new
    // level's LPs without a queryable basis, in which case
    // ``global_max_kappa()`` would report -1 and the per-iter
    // ``kappa=…`` log clause would silently disappear.  Seeding
    // forward keeps the iter table readable across the cascade.
    current_solver = std::make_unique<SDDPMethod>(*current_lp, level_opts);
    // ``carry_kappa`` was previously used to seed the new solver's
    // baseline, keeping the ``kappa=…`` clause alive across cascade
    // levels.  Removed: under CPLEX barrier without crossover the
    // per-iter LP solve never refreshes the value, so the seed
    // propagated a stale snapshot from the warmup LP into every later
    // level (uninodal, transport, full_network all showed exactly the
    // same 6.29e+05 — physically meaningless).  Leaving the kappa
    // column empty when there is no fresh observation is more honest
    // than displaying a misleading carry-forward.
    (void)carry_kappa;

    // Seed the new solver's Δgap lookback with the prior level's
    // last-N (UB, LB) so its iter 1 exercises a real windowed
    // lookback instead of the 1.0 sentinel.  See the
    // :func:`SDDPMethod::seed_prior_bounds` docstring for the
    // seed-array semantics and convergence-safety pairing with
    // min_iterations.
    if (!carry_bounds.empty()) {
      current_solver->seed_prior_bounds(carry_bounds);
    }

    // Aperture count to display:
    //   * explicit aperture list set     → its size
    //   * `num_apertures = N` set        → N (the per-phase cap)
    //   * neither set                    → -1 (use full per-phase list)
    // Previously this only inspected `apertures.size()` and reported -1
    // for the entire new `num_apertures` mode emitted by plp2gtopt for
    // cascade levels — making the log misleading on juan/iplp_plain
    // where every level has a meaningful per-phase aperture budget.
    const int aperture_log_count = level_opts.apertures.has_value()
        ? static_cast<int>(level_opts.apertures->size())
        : level_opts.num_apertures.value_or(-1);

    SPDLOG_INFO(
        "Cascade [{}]: new solver (max_iters={}, "
        "apertures={}, tol={})",
        level_name,
        level_opts.max_iterations,
        aperture_log_count,
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

    // Phase-1 accumulators — folded into level_iterations / total_cuts
    // below so the per-level summary log reports the FULL level work
    // (phase-1 + phase-2), not just phase-2's tail.  Without this the
    // ``Cascade [X]: N iters, new_cuts=M`` line under-reports at every
    // level that ran a forget pass — same observability bug family as
    // the "after 0 iters" off-by-one we fixed in the forget log.
    int phase1_cuts_added = 0;
    int phase1_iter_count_for_stats = 0;

    // Forget inherited cuts if any inherit mode was positive (N > 0).
    // Phase-1 was already capped to N iterations; now we delete the
    // inherited cuts from the LP and re-solve with remaining ones.
    //
    // `result->size()` = N_train + (sim_pass ? 1 : 0).  Intermediate
    // cascade levels set `skip_simulation_pass = true` so the trailing
    // simulation entry is absent — without this guard the count was
    // off by one and the "forgetting X cuts after N iters" log read
    // ``after 0 iters`` whenever a level skipped its sim pass.
    const auto phase1_iters = level_opts.skip_simulation_pass
        ? static_cast<int>(result->size())
        : static_cast<int>(result->size()) - 1;
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

      // Keep phase-1 results.  Track its cut count + iter count so the
      // per-level summary log below reports the FULL level (phase-1 +
      // phase-2), not just phase-2 — otherwise ``new_cuts=N`` and
      // ``N iters`` under-report at every level that ran a forget pass.
      phase1_cuts_added =
          std::accumulate(result->begin(),
                          result->end(),
                          0,
                          [](int sum, const SDDPIterationResult& r)
                          { return sum + r.cuts_added; });
      phase1_iter_count_for_stats = phase1_iters;
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

      // Bump the solver's iteration counter past phase-1's last
      // training iter so phase-2 doesn't restart at the phase-1 offset
      // and produce duplicate iter indices in the per-iter log.  The
      // SDDPMethod sets ``m_iteration_offset_`` once at construction
      // from ``iteration_offset_hint``; a plain re-entry into
      // ``solve()`` would otherwise reuse the same starting index.
      if (!result->empty()) {
        current_solver->bump_iteration_offset(
            next(result->back().iteration_index));
      }

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

    // The SDDP solver returns N training iterations + (optionally) 1
    // simulation pass entry — but ONLY the SYNCHRONOUS solve path
    // appends a sim-pass `SDDPIterationResult` (see
    // `sddp_iteration.cpp::iterate`'s `results.push_back(ir)` after
    // the sim pass).  The ASYNC path
    // (`SDDPMethod::solve_async`, dispatched when
    // `cut_sharing == none && max_async_spread > 0 && num_scenes > 1`)
    // runs the sim pass per-scene without pushing an aggregate
    // `SDDPIterationResult`.  Intermediate cascade levels skip the sim
    // pass entirely (`level_opts.skip_simulation_pass=true`), so
    // `result->size()` equals exactly N there.
    //
    // Decision matrix for whether to subtract 1 (= "sim entry
    // included in result vector"):
    //   skip_simulation_pass=true          → no sim entry → no -1
    //   skip_simulation_pass=false, sync   → sim entry    → -1
    //   skip_simulation_pass=false, async  → no sim entry → no -1
    //
    // Mirror the async-dispatch condition from
    // `sddp_iteration.cpp:290-292` exactly so the two stay in sync
    // (num_scenes>1 is dropped here: if num_scenes==1 the level
    // produced at most one training iter so the off-by-one matters
    // little either way, and querying scene_count from the
    // owned/released level LP would be brittle).  Without this
    // adjustment the async final level on juan/IPLP reports
    // `0 iters` even when training had been running for ~5 min
    // before convergence fired.
    //
    // `level_iterations` counts ONLY the iters that have not yet been
    // deducted from the global budget — phase-1's iters were already
    // subtracted inside the forget branch at line ~660, so we keep
    // this value at phase-2-only for the budget arithmetic below.
    // `iterations_reported` is the FULL level (phase-1 + phase-2) for
    // the per-level log + CascadeLevelStats so operators see the real
    // work done.
    const bool sim_entry_in_result = !level_opts.skip_simulation_pass
        && (level_opts.cut_sharing != CutSharingMode::none
            || level_opts.max_async_spread <= 0);
    const auto level_iterations = sim_entry_in_result
        ? static_cast<int>(result->size()) - 1  // exclude final fwd
        : static_cast<int>(result->size());
    const auto iterations_reported =
        phase1_iter_count_for_stats + level_iterations;

    // ── Collect per-level stats ──
    // When this level runs with max_iterations=0 *and*
    // skip_simulation_pass=true (i.e. every cascade level except the
    // last one, in a no-training cascade run) the SDDP solver returns
    // an empty results vector — no training iters, no sim pass slot.
    // Fall back to a default-constructed sentinel so the per-level
    // stats and log line stay well-defined and the global iteration
    // index does not advance (no iterations consumed any budget).
    const SDDPIterationResult empty_sentinel {};
    const auto& last = result->empty() ? empty_sentinel : result->back();
    const int phase2_cuts =
        std::accumulate(result->begin(),
                        result->end(),
                        0,
                        [](int sum, const SDDPIterationResult& r)
                        { return sum + r.cuts_added; });
    const int total_cuts = phase1_cuts_added + phase2_cuts;

    // `gap0` = first training iteration's gap (initial level gap).
    // `Δgap` = last - first (signed; negative = gap closed).  When the
    // level produced no training iterations (size <= 1), gap0 and Δ are
    // suppressed to avoid log noise on degenerate cases.
    // `new_cuts` is the total Benders cuts *added this level* (summed
    // `bwd.total_cuts` across iterations), not the cumulative store.
    const bool have_gap0 = result->size() >= 2;
    const double gap0 = have_gap0 ? result->front().gap : last.gap;
    const double dgap = last.gap - gap0;

    m_level_stats_.push_back(CascadeLevelStats {
        .name = level_name,
        .iterations = iterations_reported,
        .lower_bound = last.lower_bound,
        .upper_bound = last.upper_bound,
        .gap = last.gap,
        .gap_initial = gap0,
        .gap_delta = dgap,
        .have_gap_delta = have_gap0,
        .converged = last.converged,
        .elapsed_s = level_elapsed,
        .cuts_added = total_cuts,
    });

    // Mark this level as `done` in the checkpoint.  The actual flush to
    // disk happens further below — AFTER state_targets are persisted —
    // so a SIGKILL between the two writes leaves the checkpoint behind
    // the disk state (recovery redoes a few iters), never ahead of it.
    if (progress_enabled) {
      auto& pslot = progress.levels[level_idx];
      pslot.iters = iterations_reported;
      pslot.converged = last.converged;
      pslot.status = CascadeLevelStatus::done;
    }
    SPDLOG_INFO(
        "Cascade [{}]: {} iters, lb={}, ub={}, "
        "gap={:.4g}{}, new_cuts={}, {:.3f}s{}",
        level_name,
        iterations_reported,
        format_si(last.lower_bound),
        format_si(last.upper_bound),
        last.gap,
        have_gap0 ? std::format(" (gap0={:.4g}, Δ={:+.2e})", gap0, dgap)
                  : std::string {},
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
    // past even the discarded sim index.  When the level produced no
    // results (max_iters=0 + skip_simulation_pass) keep the index where
    // it was — no iterations have been emitted on this level.
    if (!result->empty()) {
      global_iter_index = next(last.iteration_index);
    }
    if (progress_enabled) {
      progress.levels[level_idx].global_iter_after =
          static_cast<Index>(value_of(global_iter_index));
    }

    // ── 4. Check convergence ──
    if (last.converged) {
      if (level_idx == m_cascade_opts_.level_array.size() - 1) {
        // Flush progress to disk before breaking — the post-loop flush at
        // line ~1451 is skipped on this path, so without this an
        // `--recover` after a happy-path converged cascade would see the
        // final level still marked `in_progress` and re-solve it (the
        // upstream save happened at level *entry* — `:852` — but the
        // post-convergence `status = done` assignment at `:1165` only
        // lives in memory).  Captured by the
        // `test_cascade_recover.cpp` "final-level converges" assertion
        // — synthesized sidecar there documents the historical
        // pre-fix shape too.
        if (progress_enabled) {
          if (auto save = save_cascade_progress(progress, progress_path);
              !save.has_value())
          {
            SPDLOG_WARN(
                "Cascade: could not flush progress file '{}' "
                "after final-level convergence: {}",
                progress_path.string(),
                save.error().message);
          }
        }
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

      // Persist state_targets atomically so a `--recover` after this point
      // can skip this level: load the targets from disk and re-enter the
      // cascade at the successor level.  Must happen BEFORE the progress
      // file's `done` flush — see the ordering invariant at the top of
      // `solve()`.
      if (progress_enabled) {
        const auto st_path = state_targets_file_for(level_idx);
        if (!st_path.empty()) {
          auto save_res = save_state_targets(prev_targets, st_path);
          if (save_res.has_value()) {
            progress.levels[level_idx].state_targets_file = st_path;
          } else {
            SPDLOG_WARN("Cascade [{}]: could not save state_targets '{}': {}",
                        level_name,
                        st_path,
                        save_res.error().message);
          }
        }
      }

      // Pre-filter and serialize cuts to disk while the current LP is
      // still alive.  Doing this here (not at next-level start) lets us
      // drop the solver + owned LP before level N+1 allocates a new LP,
      // halving peak memory on the level boundary.
      m_prev_cuts_file_.clear();
      const auto& next_level = m_cascade_opts_.level_array[level_idx + 1];
      // [DIAG cut-transfer] Surface every decision point on the
      // intermediate→next cut serialization path.  Added 2026-05-13
      // after juan/iplp_plain showed `new_cuts=4000` at level 0 but
      // ZERO `serialized N cuts` log lines AND α=0 throughout level 1
      // (forward primal-only objective) — i.e. cuts were stored and
      // counted but never made it into the parquet handed to
      // transport.  Remove (or fold to TRACE) after the root cause
      // is fixed.
      SPDLOG_INFO(
          "[DIAG] Cascade [{}]: transition decision: "
          "next_level.transition.has_value={}",
          level_name,
          next_level.transition.has_value());
      if (next_level.transition) {
        // NOTE on cut activity / pruning at the cascade boundary:
        // The previous implementation refreshed each stored cut's
        // dual by reading `row_dual[cut.row]` from the main cell's
        // LP and filtered by `optimality_dual_threshold`.  That was
        // structurally wrong — cuts are added to the main cell via
        // `add_row` after the aperture clones have been solved; the
        // main cell itself is not re-solved with the new cut binding,
        // so its last-solve row_dual at `cut.row` is either stale
        // (from a forward solve that predates this cut) or empty
        // (cache wiped by the post-add invalidate-on-mutation).
        // The structurally correct "cut activity" is the probability-
        // weighted dual aggregate across apertures that exercise the
        // cut during the backward pass.  That requires instrumenting
        // the aperture loop and accumulating per-cut activity across
        // iterations — a real feature (decay / normalisation design
        // choices) that does not exist today.  Until it does, every
        // Optimality cut is inherited and no threshold filter is
        // applied; Feasibility cuts are still skipped (they are
        // regenerated by the next level's forward pass as needed).
        auto stored_cuts = current_solver->stored_cuts();
        const auto& trans = *next_level.transition;
        const int opt_cut_mode = trans.inherit_optimality_cuts.value_or(0);
        const bool want_opt_cuts = opt_cut_mode != 0;
        SPDLOG_INFO(
            "[DIAG] Cascade [{}]: opt_cut_mode={} want_opt_cuts={} "
            "stored_cuts.size()={} num_stored_cuts(direct)={}",
            level_name,
            opt_cut_mode,
            want_opt_cuts,
            stored_cuts.size(),
            current_solver->num_stored_cuts());

        // Only optimality cuts are inheritable across levels.  Feasibility
        // cuts are regenerated by the forward pass as needed.
        if (want_opt_cuts && !stored_cuts.empty()) {
          std::vector<StoredCut> filtered;
          filtered.reserve(stored_cuts.size());
          int n_opt = 0;
          int n_feas = 0;
          int n_other = 0;
          for (const auto& cut : stored_cuts) {
            if (cut.type == CutType::Optimality) {
              ++n_opt;
              filtered.push_back(cut);
            } else if (cut.type == CutType::Feasibility) {
              ++n_feas;
            } else {
              ++n_other;
            }
          }
          SPDLOG_INFO(
              "[DIAG] Cascade [{}]: stored cuts breakdown: "
              "n_opt={} n_feas={} n_other={} -> filtered={}",
              level_name,
              n_opt,
              n_feas,
              n_other,
              filtered.size());
          if (!filtered.empty()) {
            const auto cuts_tmp = std::filesystem::temp_directory_path()
                / std::format("cascade_cuts_{}_{}.parquet",
                              static_cast<std::int64_t>(::getpid()),
                              level_idx + 1);
            auto save_result =
                save_cuts_parquet(filtered, *current_lp, cuts_tmp.string());
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
      //
      // Snapshot the solver's max kappa BEFORE the reset so the next
      // level can seed its own kappa baseline.  Without this snapshot
      // the kappa-carry would read -1 (current_solver already null)
      // and the iter log would silently drop the ``kappa=…`` clause.

      // Force-flush the async logger BEFORE the level teardown.  The
      // overrun_oldest async policy (gtopt_main.cpp) drops oldest
      // messages when the bounded queue saturates — under sustained
      // backpressure this can silently swallow critical post-
      // convergence diagnostics (per-scene cancellation warnings,
      // final iter UB/LB lines) that we need on disk before the next
      // level's parallel LP build starts a fresh burst of log lines.
      // `flush()` blocks until every queued message has been written
      // through the sinks, so any line emitted up to this point in
      // the L_N solve is guaranteed to land in the log file before
      // L_{N+1} begins producing.
      // Time the whole teardown so the per-level boundary cost stays
      // visible in the iter log.  Historically the dominant component
      // was the serial destruction of the (scene × phase) LP grid
      // (~4-5s/level on plp/2_years); ``release_cells()`` now fans that
      // per-cell teardown across the solver work pool.
      const auto t_teardown_start = std::chrono::steady_clock::now();
      try {
        if (auto logger = spdlog::default_logger()) {
          logger->flush();
        }
      } catch (...) {
        // Best-effort: a flush failure must not prevent the cascade
        // transition.  The next level will still run; we just lose
        // the disk-flush guarantee for the prior level's tail logs.
      }

      const auto owned_before = m_owned_lps_.size();
      const bool released_caller = (current_lp == &planning_lp);
      if (current_solver) {
        const double k = current_solver->global_max_kappa();
        if (k >= 0.0) {
          carry_kappa = std::max(carry_kappa, k);
        }
      }
      // Snapshot the level's last-N (UB, LB) BEFORE resetting the
      // solver so the next level can seed its Δgap lookback against
      // a real window of prior iters (a single point is not enough
      // for ``stationary_window > 1``).  ``m_all_results_``
      // accumulates every iter result across the cascade; its tail
      // at this point is this level's last iter.  We take up to
      // ``SDDPMethod::STATIONARY_SEED_DEPTH`` of the most recent
      // entries, preserving cascade order (oldest-first).  When the
      // level produced no results (cancelled early before iter 1
      // ever finalised), leave carry_bounds unchanged so the next
      // level keeps whatever was carried through from earlier (or
      // stays empty at L0).
      if (!m_all_results_.empty()) {
        const std::size_t depth = SDDPMethod::STATIONARY_SEED_DEPTH;
        const std::size_t take = std::min(depth, m_all_results_.size());
        carry_bounds.clear();
        carry_bounds.reserve(take);
        for (std::size_t i = m_all_results_.size() - take;
             i < m_all_results_.size();
             ++i)
        {
          carry_bounds.push_back(SDDPMethod::PriorIterBounds {
              .upper_bound = m_all_results_[i].upper_bound,
              .lower_bound = m_all_results_[i].lower_bound,
          });
        }
      }
      current_solver.reset();
      current_lp = nullptr;
      // Release each owned LP's (scene × phase) cells via the parallel
      // ``release_cells()`` path BEFORE destroying the owning
      // unique_ptrs.  The default ``~PlanningLP`` destroys the grid
      // serially, which costs ~4.6s/level on plp/2_years; calling
      // ``release_cells()`` first fans the per-cell teardown across the
      // solver work pool, leaving only empty shells for ``clear()`` to
      // drop.
      for (auto& owned_lp : m_owned_lps_) {
        if (owned_lp) {
          owned_lp->release_cells();
        }
      }
      m_owned_lps_.clear();
      m_owned_lps_.shrink_to_fit();
      if (released_caller) {
        planning_lp.release_cells();
      }
      const auto teardown_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t_teardown_start)
              .count();
      SPDLOG_INFO(
          "Cascade [{}]: released solver, {} owned LP(s){} before level "
          "[{}] (teardown {}ms)",
          level_name,
          owned_before,
          released_caller ? " and caller LP cells" : "",
          m_cascade_opts_.level_array[level_idx + 1].name.value_or(
              as_label("level", level_idx + 1)),
          teardown_ms);

      // Second flush: ensure the "released solver" line + any other
      // post-teardown diagnostics make it to disk before the next
      // level's PlanningLP build starts emitting parallel-scene LP-
      // element log lines.  Cheap (queue is usually near-empty
      // immediately after the previous flush).
      try {
        if (auto logger = spdlog::default_logger()) {
          logger->flush();
        }
      } catch (...) {
        // Best-effort — same rationale as above.
      }
    }

    // Flush progress AFTER cuts + state_targets have been written.  Order
    // matters: a SIGKILL between (cuts/state_targets) and this rename
    // leaves the checkpoint trailing the disk state — recovery redoes a
    // few iters, never resumes past missing data.
    if (progress_enabled) {
      if (auto save = save_cascade_progress(progress, progress_path);
          !save.has_value())
      {
        SPDLOG_WARN("Cascade: could not flush progress file '{}': {}",
                    progress_path.string(),
                    save.error().message);
      }
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
        "  [{}]: {} iters, lb={}, ub={}, gap={:.4g}{}, "
        "cuts={}, {:.3f}s{}",
        ls.name,
        ls.iterations,
        format_si(ls.lower_bound),
        format_si(ls.upper_bound),
        ls.gap,
        ls.have_gap_delta
            ? std::format(
                  " (gap0={:.4g}, Δ={:+.2e})", ls.gap_initial, ls.gap_delta)
            : std::string {},
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
