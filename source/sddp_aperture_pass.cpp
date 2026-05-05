/**
 * @file      sddp_aperture_pass.cpp
 * @brief     Aperture backward pass methods for SDDPMethod
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the aperture-based backward pass variants of the SDDP solver.
 * Extracted from sddp_method.cpp to reduce file size.
 * These methods are member functions of SDDPMethod defined in sddp_method.hpp.
 */

// SPDLOG_ACTIVE_LEVEL must be set BEFORE any header that transitively
// includes <spdlog/spdlog.h> — otherwise the SPDLOG_TRACE macro is
// baked to `(void)0` for this whole translation unit and runtime
// `set_level(trace)` cannot recover the compiled-out calls.
#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include <gtopt/benders_cut.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
#include <spdlog/spdlog.h>

namespace
{

/// Format a range of formattable values as a comma-separated string.
template<typename Range>
[[nodiscard]] std::string join_values(const Range& values)
{
  std::string result;
  bool first = true;
  for (const auto& v : values) {
    if (!first) {
      result += ", ";
    }
    result += std::format("{}", v);
    first = false;
  }
  return result;
}

// `resolve_effective_apertures` was moved out of the anonymous
// namespace into `namespace gtopt` (declared in `sddp_aperture.hpp`)
// so the four-way decision (filter / synthetic / pass-through /
// fallback) can be covered by unit tests without reaching into
// translation-unit-private symbols.  See the header for the full
// contract.

}  // namespace

namespace gtopt
{

[[nodiscard]] auto resolve_effective_apertures(
    std::span<const Aperture> aperture_defs,
    std::span<const ScenarioLP> all_scenarios,
    const std::optional<Array<Uid>>& requested_uids,
    Array<Aperture>& owned,
    std::string_view log_tag) -> std::optional<std::span<const Aperture>>
{
  if (!requested_uids.has_value()) {
    if (aperture_defs.empty()) {
      return std::nullopt;  // fallback
    }
    return std::span<const Aperture> {aperture_defs};
  }

  const auto& requested = *requested_uids;
  if (requested.empty()) {
    return std::nullopt;  // fallback
  }

  if (!aperture_defs.empty()) {
    // Filter existing apertures by requested UIDs.
    for (const auto& ap : aperture_defs) {
      if (std::ranges::find(requested, ap.uid) != requested.end()) {
        owned.push_back(ap);
      }
    }
    for (const auto uid : requested) {
      const bool found = std::ranges::any_of(
          owned, [uid](const auto& a) { return a.uid == uid; });
      if (!found) {
        SPDLOG_WARN("{}: requested UID {} not found, skipping", log_tag, uid);
      }
    }
  } else {
    // No aperture_array: build synthetic from scenarios matching UIDs.
    const auto num_all = std::ssize(all_scenarios);
    owned = build_synthetic_apertures(all_scenarios,
                                      std::min(std::ssize(requested), num_all));
  }

  if (owned.empty()) {
    return std::nullopt;  // fallback
  }
  return std::span<const Aperture> {owned};
}

// ── Helper: install Benders cut on src_li with bcut fallback ────────────────

auto SDDPMethod::install_aperture_backward_cut(
    SceneIndex scene_index,
    PhaseIndex src_phase_index,
    PhaseIndex phase_index,
    int cut_offset,
    IterationIndex iteration_index,
    ColIndex src_alpha_col,
    const PhaseStateInfo& src_state,
    const PhaseStateInfo& target_state,
    std::optional<SparseRow> expected_cut,
    LinearInterface& src_li,
    const SolverOptions& opts) -> int
{
  // Fine-grained stage timers — same semantics as the Benders-only
  // path in `backward_pass_single_phase`.  `cut_build_s` only fires
  // when we fall through to the bcut path (the aperture-built cut
  // arrived already built in @p expected_cut, so its build cost is
  // captured by `total_solve_time_s` on the aperture clones — not
  // attributable to a single per-step bwd_cut_build bucket).
  using Clock = std::chrono::steady_clock;
  const auto elapsed_s = [](Clock::time_point start) noexcept
  { return std::chrono::duration<double>(Clock::now() - start).count(); };
  double dt_cut_build = 0.0;
  double dt_add_row = 0.0;
  double dt_resolve = 0.0;
  double dt_kappa = 0.0;
  double dt_store = 0.0;

  const auto ceps = m_options_.cut_coeff_eps;
  const auto scale_obj = planning_lp().options().scale_objective();

  // Try the aperture-built cut first when available.  If the post-cut
  // resolve leaves src_li non-optimal, back it out and fall through to
  // the bcut path.  `expected_cut` is consumed on the success path.
  if (expected_cut.has_value()) {
    // Unified SDDP-level `add_cut_row`: releases α on `src_phase_index`
    // (the phase where the cut lives — not the target phase) iff the
    // cut references α, then adds the row via
    // `LinearInterface::add_cut_row` which also records for low-memory
    // replay.  No separate `record_cut_row` call needed.
    const auto t_add_row = Clock::now();
    const auto cut_row = add_cut_row(planning_lp(),
                                     scene_index,
                                     src_phase_index,
                                     CutType::Optimality,
                                     *expected_cut,
                                     ceps);
    dt_add_row += elapsed_s(t_add_row);

    // Re-solve src_li only when there is a further backward step that
    // will consume its state.  At src_phase_index == 0 there is no
    // earlier phase, so we trust the cut (it came from a valid aperture
    // solve) and skip the resolve.
    bool keep_expected_cut = true;
    if (src_phase_index) {
      src_li.set_log_tag(sddp_log("Backward",
                                  gtopt::uid_of(iteration_index),
                                  uid_of(scene_index),
                                  uid_of(src_phase_index)));
      const auto t_resolve = Clock::now();
      auto r = src_li.resolve(opts);
      dt_resolve += elapsed_s(t_resolve);
      if (r.has_value() && src_li.is_optimal()) {
        const auto t_kappa = Clock::now();
        update_max_kappa(scene_index, src_phase_index, src_li, iteration_index);
        dt_kappa += elapsed_s(t_kappa);
      } else {
        keep_expected_cut = false;
        SPDLOG_WARN(
            "{}: non-optimal after expected cut (status {}, resolve "
            "{:.3f}s), reverting row and installing bcut fallback",
            sddp_log("Backward",
                     gtopt::uid_of(iteration_index),
                     uid_of(scene_index),
                     uid_of(src_phase_index)),
            src_li.get_status(),
            dt_resolve);
      }
    }

    if (keep_expected_cut) {
      const auto t_store = Clock::now();
      store_cut(scene_index,
                src_phase_index,
                *expected_cut,
                CutType::Optimality,
                cut_row);
      dt_store += elapsed_s(t_store);
      SPDLOG_TRACE("{}: cut for phase {} rhs={:.4f}",
                   sddp_log("Aperture",
                            gtopt::uid_of(iteration_index),
                            uid_of(scene_index),
                            uid_of(phase_index)),
                   src_phase_index,
                   expected_cut->lowb);

      auto& sstats = src_li.mutable_solver_stats();
      ++sstats.bwd_step_count;
      sstats.bwd_cut_build_s += dt_cut_build;
      sstats.bwd_add_row_s += dt_add_row;
      sstats.bwd_resolve_s += dt_resolve;
      sstats.bwd_kappa_s += dt_kappa;
      sstats.bwd_store_cut_s += dt_store;
      return 1;
    }

    // Recovery: delete the bad row before adding the bcut.  The row was
    // never passed to `store_cut`, so there is no cut-store / active-cut
    // bookkeeping to unwind — only the backend row matrix.
    const std::array<int, 1> to_delete {static_cast<int>(cut_row)};
    src_li.delete_rows(to_delete);
  }

  // Bcut path: aperture failed, or the expected cut broke optimality.
  //
  // INVARIANT — the BACKWARD pass only produces optimality cuts.
  //   There is NO elastic branch in the backward pass.
  //   There are NO feasibility cuts in the backward pass.
  //   The bcut fallback is ALWAYS a `CutType::Optimality` cut.
  //
  // The cut is a standard Benders underestimator
  //   α_{k-1} ≥ Z + Σ rc_i · (s_{k-1} - v̂_i)
  // valid for the true future-cost function at phase k-1.  The rc
  // values are read off the state-variable registry (populated by
  // `capture_state_variable_values` during the forward solve of
  // phase k) and Z is `target_state.forward_full_obj_physical`.
  //
  // `free_alpha` fires as usual — that is exactly the point of an
  // optimality cut: it certifies a tighter lower bound on α and
  // releases the bootstrap floor.  Feasibility cuts (if any) are
  // installed from the FORWARD pass when its elastic branch fires
  // (`sddp_forward_pass.cpp`), never from here.
  //
  // DO NOT reclassify this cut as `Feasibility` based on what the
  // forward pass did at phase k.  Doing so masks real Benders
  // optimality cuts and breaks the SDDP invariant that every
  // non-last phase receives an optimality cut per backward
  // iteration.

  // ── Optional: re-solve LP_t before the bcut to pick up cuts on α_t
  // added earlier in this backward pass.  See
  // `SDDPOptions::backward_resolve_target` for full semantics.  The
  // aperture-success path already sees these cuts because each clone
  // is forked from LP_t at solve time; only the bcut fallback (which
  // reads ``forward_full_obj_physical`` cached from the forward
  // pass) needs the refresh.
  double Z = target_state.forward_full_obj_physical;
  if (m_options_.backward_resolve_target) {
    auto& tgt_sys = planning_lp().system(scene_index, phase_index);
    tgt_sys.ensure_lp_built();
    auto& tgt_li = tgt_sys.linear_interface();
    tgt_li.set_log_tag(sddp_log("Backward",
                                gtopt::uid_of(iteration_index),
                                uid_of(scene_index),
                                uid_of(phase_index)));
    const auto t_tgt_resolve = Clock::now();
    auto r = tgt_li.resolve(opts);
    dt_resolve += elapsed_s(t_tgt_resolve);
    if (r.has_value() && tgt_li.is_optimal()) {
      Z = tgt_li.get_obj_value();
      const auto sol_phys = tgt_li.get_col_sol();
      const auto rc = tgt_li.get_col_cost_raw();
      capture_state_variable_values(scene_index, phase_index, sol_phys, rc);
      m_scene_phase_states_[scene_index][phase_index]
          .forward_full_obj_physical = Z;
      update_max_kappa(scene_index, phase_index, tgt_li, iteration_index);
    } else {
      SPDLOG_DEBUG(
          "{}: backward_resolve_target re-solve non-optimal "
          "(status {}) — using forward-cached cut data for bcut",
          sddp_log("Backward",
                   gtopt::uid_of(iteration_index),
                   uid_of(scene_index),
                   uid_of(phase_index)),
          tgt_li.get_status());
    }
  }

  const auto t_build = Clock::now();
  auto fallback_cut = build_benders_cut_physical(
      src_alpha_col, src_state.outgoing_links, Z, scale_obj, ceps);
  sddp_bcut_tag.apply_to(fallback_cut);
  fallback_cut.variable_uid = uid_of(src_phase_index);
  fallback_cut.context = make_iteration_context(uid_of(scene_index),
                                                uid_of(phase_index),
                                                gtopt::uid_of(iteration_index),
                                                cut_offset);
  dt_cut_build += elapsed_s(t_build);

  // Unified `add_cut_row`: releases α on `src_phase_index` iff the
  // cut references α, then adds + records the row for low-memory
  // replay.  Idempotent α release if the expected_cut path already
  // fired.
  const auto t_add_row = Clock::now();
  const auto cut_row = add_cut_row(planning_lp(),
                                   scene_index,
                                   src_phase_index,
                                   CutType::Optimality,
                                   fallback_cut,
                                   ceps);
  dt_add_row += elapsed_s(t_add_row);

  const auto t_store = Clock::now();
  store_cut(
      scene_index, src_phase_index, fallback_cut, CutType::Optimality, cut_row);
  dt_store += elapsed_s(t_store);

  SPDLOG_TRACE("{}: bcut for phase {} rhs={:.4f}",
               sddp_log("Aperture",
                        gtopt::uid_of(iteration_index),
                        uid_of(scene_index),
                        uid_of(phase_index)),
               src_phase_index,
               fallback_cut.lowb);

  auto& sstats = src_li.mutable_solver_stats();
  ++sstats.bwd_step_count;
  sstats.bwd_cut_build_s += dt_cut_build;
  sstats.bwd_add_row_s += dt_add_row;
  sstats.bwd_resolve_s += dt_resolve;
  sstats.bwd_kappa_s += dt_kappa;
  sstats.bwd_store_cut_s += dt_store;
  return 1;
}

// ── Helper: build the ApertureSubmitFunc callback ───────────────────────────

auto SDDPMethod::make_aperture_submit_fn(PhaseIndex phase_index,
                                         IterationIndex iteration_index)
    -> ApertureSubmitFunc
{
  // Submit aperture tasks to the SDDP work pool for parallel execution.
  // Each aperture task operates on its own LP clone, so they are
  // independent and can safely execute concurrently.  The calling scene
  // thread blocks on the returned futures while pool threads process
  // the aperture solves.
  //
  // Fallback to synchronous execution when no pool is available (e.g.
  // during unit tests).
  auto* pool = m_pool_;
  // TaskPriority::Medium is the scheduling tier (controls CPU threshold);
  // the SDDPTaskKey tuple provides the secondary sort within that tier.
  const BasicTaskRequirements<SDDPTaskKey> req {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(iteration_index,
                                         SDDPPassDirection::backward,
                                         phase_index,
                                         SDDPTaskKind::lp),
      .name = {},
  };

  return [pool, req](const std::function<ApertureCutResult()>& task)
             -> std::future<ApertureCutResult>
  {
    if (pool != nullptr) {
      auto fut = pool->submit(task, req);
      if (fut.has_value()) {
        return std::move(*fut);
      }
      SPDLOG_WARN("SDDP Aperture: pool submit failed, running synchronously");
    }
    // Fallback: run synchronously
    std::promise<ApertureCutResult> p;
    p.set_value(task());
    return p.get_future();
  };
}

// ── Aperture per-phase implementation (shared by single-phase and full pass)

auto SDDPMethod::backward_pass_aperture_phase_impl(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    int cut_offset,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    const SolverOptions& opts,
    IterationIndex iteration_index) -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene_index];
  int cuts_added = 0;
  m_phase_grid_.record(gtopt::uid_of(iteration_index),
                       uid_of(scene_index),
                       uid_of(phase_index),
                       GridCell::Aperture);

  const auto src_phase_index = previous(phase_index);
  auto& src_sys = planning_lp().system(scene_index, src_phase_index);
  const auto& src_state = phase_states[src_phase_index];

  // Ensure the source-phase LP backend is live.  No-op when already
  // live; otherwise reloads from snapshot (snapshot/compress) or
  // re-flattens from collections (rebuild).
  src_sys.ensure_lp_built();

  auto& src_li = src_sys.linear_interface();
  const auto& plp = planning_lp().simulation().phases()[phase_index];

  auto& target_sys = planning_lp().system(scene_index, phase_index);
  target_sys.ensure_lp_built();

  // Populate the per-element XLP state (generation_cols, …) on the
  // main thread BEFORE dispatching aperture tasks.  Under compress
  // the backward-pass aperture update loop in sddp_aperture.cpp
  // reads `sys.collections()` from every task concurrently — without
  // a pre-populated state, each task would race on `m_collections_`
  // via `rebuild_collections_if_needed()` and segfault.
  // Single-threaded call here is safe; the subsequent aperture tasks
  // only read collections.  No-op under `off` (collections always
  // alive) and under `rebuild` (refreshed by rebuild_in_place).
  target_sys.rebuild_collections_if_needed();

  // Re-apply volume-dependent LP coefficient updates on `target_sys`
  // BEFORE aperture clones are created.  Under
  // `LowMemoryMode::compress` / `rebuild`, `ensure_lp_built()` above
  // reloads the construction-time matval/RHS from the snapshot — the
  // forward / backward passes' `update_lp_for_phase` mutations
  // (turbine production factor, seepage segment selection, discharge
  // limit) are NOT in the snapshot; they live only on the live
  // backend that was dropped at the previous `release_backend()`.
  //
  // Without this call the aperture clones inherit construction-time
  // volume-dependent coefficients while their cuts are computed
  // against the current iteration's physical_eini trial values —
  // producing per-aperture LPs whose value-function geometry is
  // inconsistent with the forward model.  Observed on juan/iplp
  // (1 active scene + 14 feasible apertures, 5 iters):
  //   * `all apertures infeasible at N phase(s)` warnings under
  //     compress only — accumulating across iters (4→2→3→4→5 phases).
  //   * `infeasible: 35 (primal)` total under compress vs 0 under off.
  //   * UB exploding from 2.55 G to 3.35 × 10²⁹ T by iter 5 because
  //     the Benders-fallback cuts derived from those infeasibilities
  //     poison the SDDP recourse-cost estimator.
  //
  // Same root pattern as the main backward pass fix (commit
  // `675422e7 fix(reservoir): always re-issue update_lp coefficients
  // + efin-only DRL`) but on the aperture pass's source-LP
  // refreshing path instead of the main `tgt_li.resolve` path
  // (`sddp_method_iteration.cpp:463`).  Under `LowMemoryMode::off`
  // `update_lp_for_phase` is idempotent (the always-re-issue change
  // ensures both `set_coeff` and `set_rhs` fire even when memory
  // state matches), so this is a no-op cost there.
  update_lp_for_phase(scene_index, phase_index);

  // Keep the flat LP decompressed while aperture tasks create clones.
  // The guard re-compresses on scope exit (level 2 only).
  const DecompressionGuard dcomp_guard(target_sys.linear_interface());

  // Resolve the α column for the source phase once; it is passed into
  // aperture cut building and reused below for any fallback.
  const auto* src_alpha_svar = find_alpha_state_var(
      planning_lp().simulation(), scene_index, src_phase_index);
  const auto src_alpha_col = (src_alpha_svar != nullptr)
      ? src_alpha_svar->col()
      : ColIndex {unknown_index};

  // Extend `lp_debug` to aperture clones: pass the debug writer only
  // when the current (scene, phase) falls inside the configured
  // filter window AND `aperture` is selected by `lp_debug_passes`.
  // Aperture clones are then dumped via
  // `sddp_file::debug_aperture_lp_fmt` under `log_directory`.
  auto* const aperture_lp_debug =
      (m_options_.lp_debug
       && lp_debug_passes_includes(m_options_.lp_debug_passes,
                                   LpDebugPass::aperture)
       && in_lp_debug_range(uid_of(scene_index),
                            uid_of(phase_index),
                            m_options_.lp_debug_scene_min,
                            m_options_.lp_debug_scene_max,
                            m_options_.lp_debug_phase_min,
                            m_options_.lp_debug_phase_max))
      ? &m_lp_debug_writer_
      : nullptr;

  auto expected_cut = solve_apertures_for_phase(
      scene_index,
      phase_index,
      src_state,
      src_alpha_col,
      base_scenario,
      all_scenarios,
      aperture_defs,
      plp.apertures(),
      cut_offset,
      target_sys,
      plp,
      opts,
      m_label_maker_,
      m_options_.log_directory,
      uid_of(scene_index),
      uid_of(phase_index),
      make_aperture_submit_fn(phase_index, iteration_index),
      m_options_.aperture_timeout,
      m_options_.save_aperture_lp,
      m_aperture_cache_,
      iteration_index,
      m_options_.cut_coeff_eps,
      aperture_lp_debug,
      m_options_.aperture_use_manual_clone);

  const auto& target_state = phase_states[phase_index];
  cuts_added += install_aperture_backward_cut(scene_index,
                                              src_phase_index,
                                              phase_index,
                                              cut_offset,
                                              iteration_index,
                                              src_alpha_col,
                                              src_state,
                                              target_state,
                                              std::move(expected_cut),
                                              src_li,
                                              opts);

  // ── Per-cell release: drop target_sys's backend now ──
  //
  // Aperture clones constructed inside `solve_apertures_for_phase`
  // are destroyed when the per-aperture task lambdas return (the
  // `LinearInterface` clones own their backends and free them on
  // scope exit).  After `install_aperture_backward_cut` returns the
  // aggregated cut has been added to `src_li = (scene, src_phase)`,
  // a different cell.  `target_sys = (scene, phase_index)` is the
  // last touch this loop iteration and is **not visited again** in
  // the remainder of this backward pass:
  //  - The next loop iteration uses `phase_index − 1`, with
  //    `target = (s, phase_index − 1)` and `src = (s, phase_index − 2)`.
  //  - `(s, phase_index)` would only be touched again if the loop
  //    revisited a higher phase, which it does not (the loop is a
  //    one-shot reverse pass from `N−1` down to `1`).
  // The cell is reloaded from snapshot at the next SDDP iteration's
  // forward pass — that is the natural reconstruction point.
  //
  // Phase 0 is never a target (the loop stops at `phase_index = 1`)
  // and is released separately in `run_backward_pass_synchronized`
  // after `share_cuts_for_phase` writes the last batch of peer
  // cuts to it.
  //
  // No-op when `low_memory_mode == off`; idempotent across repeat
  // calls (`linear_interface.cpp:144` early-return on
  // `m_backend_released_`).
  if (m_options_.low_memory_mode != LowMemoryMode::off) {
    target_sys.release_backend();
  }

  return cuts_added;
}

// ── Per-phase aperture backward pass step ───────────────────────────────────

auto SDDPMethod::backward_pass_with_apertures_single_phase(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    int cut_offset,
    const SolverOptions& opts,
    IterationIndex iteration_index) -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();
  const auto& scene_lp = simulation.scenes()[scene_index];
  const auto& scene_scenarios = scene_lp.scenarios();

  if (scene_scenarios.empty()) {
    return backward_pass_single_phase(
        scene_index, phase_index, cut_offset, opts, iteration_index);
  }

  Array<Aperture> owned;
  const auto effective = resolve_effective_apertures(
      aperture_defs,
      all_scenarios,
      m_options_.apertures,
      owned,
      sddp_log(
          "Aperture", gtopt::uid_of(iteration_index), uid_of(scene_index)));
  if (!effective.has_value()) {
    return backward_pass_single_phase(
        scene_index, phase_index, cut_offset, opts, iteration_index);
  }

  return backward_pass_aperture_phase_impl(scene_index,
                                           phase_index,
                                           cut_offset,
                                           scene_scenarios.front(),
                                           all_scenarios,
                                           *effective,
                                           opts,
                                           iteration_index);
}

// ── Aperture backward pass ──────────────────────────────────────────────────

auto SDDPMethod::backward_pass_with_apertures(SceneIndex scene_index,
                                              const SolverOptions& opts,
                                              IterationIndex iteration_index)
    -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();

  Array<Aperture> owned;
  const auto effective = resolve_effective_apertures(
      aperture_defs,
      all_scenarios,
      m_options_.apertures,
      owned,
      sddp_log(
          "Aperture", gtopt::uid_of(iteration_index), uid_of(scene_index)));
  if (!effective.has_value()) {
    return backward_pass(scene_index, opts, iteration_index);
  }
  const auto effective_defs = *effective;

  // ── Common aperture backward loop ─────────────────────────────────────
  const auto& phases = simulation.phases();
  const auto num_phases = static_cast<Index>(phases.size());
  auto& phase_states = m_scene_phase_states_[scene_index];
  int total_cuts = 0;
  [[maybe_unused]] const auto bwd_tid = std::this_thread::get_id();

  SPDLOG_INFO(
      "{}: backward starting ({} phases) [thread {}]",
      sddp_log("Aperture", gtopt::uid_of(iteration_index), uid_of(scene_index)),
      num_phases - 1,
      std::hash<std::thread::id> {}(bwd_tid) % 10000);

  const auto& scene_lp = simulation.scenes()[scene_index];
  const auto& scene_scenarios = scene_lp.scenarios();
  if (scene_scenarios.empty()) {
    return backward_pass(scene_index, opts, iteration_index);
  }
  const auto& base_scenario = scene_scenarios.front();
  // Collect phases where all apertures were infeasible for a summary
  std::vector<PhaseUid> infeasible_phases;

  for (const auto phase_index :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format("{}: cancelled at phase {}",
                                 sddp_log("Aperture",
                                          gtopt::uid_of(iteration_index),
                                          uid_of(scene_index)),
                                 uid_of(phase_index)),
      });
    }

    const auto src_phase_index = previous(phase_index);
    auto& src_sys = planning_lp().system(scene_index, src_phase_index);

    const auto& src_state = phase_states[src_phase_index];

    // Ensure the source-phase LP backend is live; no-op when already
    // loaded, otherwise reload (snapshot/compress) or re-flatten
    // (rebuild).
    src_sys.ensure_lp_built();

    auto& src_li = src_sys.linear_interface();
    const auto& plp = phases[phase_index];

    // Forward-pass summary for the target phase.
    const auto& target_state = phase_states[phase_index];

    auto& target_sys = planning_lp().system(scene_index, phase_index);
    target_sys.ensure_lp_built();
    // Single-threaded XLP-state rebuild before aperture tasks are
    // dispatched (same rationale as in
    // `backward_pass_aperture_phase_impl`).
    target_sys.rebuild_collections_if_needed();

    // Re-apply volume-dependent LP coefficient updates on `target_sys`
    // BEFORE aperture clones are created.  See the matching call in
    // `backward_pass_aperture_phase_impl` above for full rationale —
    // without this the aperture clones inherit construction-time
    // matval/RHS under `LowMemoryMode::compress` / `rebuild` while
    // their per-aperture cuts assume current-iteration trial values,
    // producing the `all apertures infeasible at N phase(s)`
    // cascade and the UB-explosion-to-10²⁹ symptom on juan/iplp.
    update_lp_for_phase(scene_index, phase_index);

    // Keep the flat LP decompressed while aperture tasks create clones.
    const DecompressionGuard dcomp_guard(target_sys.linear_interface());

    // Resolve α column for the source phase once per iteration.
    const auto* src_alpha_svar = find_alpha_state_var(
        planning_lp().simulation(), scene_index, src_phase_index);
    const auto src_alpha_col = (src_alpha_svar != nullptr)
        ? src_alpha_svar->col()
        : ColIndex {unknown_index};

    // Extend `lp_debug` to aperture clones (see the loop-variant above).
    auto* const aperture_lp_debug =
        (m_options_.lp_debug
         && lp_debug_passes_includes(m_options_.lp_debug_passes,
                                     LpDebugPass::aperture)
         && in_lp_debug_range(uid_of(scene_index),
                              uid_of(phase_index),
                              m_options_.lp_debug_scene_min,
                              m_options_.lp_debug_scene_max,
                              m_options_.lp_debug_phase_min,
                              m_options_.lp_debug_phase_max))
        ? &m_lp_debug_writer_
        : nullptr;

    auto expected_cut = solve_apertures_for_phase(
        scene_index,
        phase_index,
        src_state,
        src_alpha_col,
        base_scenario,
        all_scenarios,
        effective_defs,
        plp.apertures(),
        total_cuts,
        target_sys,
        plp,
        opts,
        m_label_maker_,
        m_options_.log_directory,
        uid_of(scene_index),
        uid_of(phase_index),
        make_aperture_submit_fn(phase_index, iteration_index),
        0.0,
        m_options_.save_aperture_lp,
        m_aperture_cache_,
        iteration_index,
        m_options_.cut_coeff_eps,
        aperture_lp_debug,
        m_options_.aperture_use_manual_clone);

    if (!expected_cut.has_value()) {
      infeasible_phases.push_back(uid_of(phase_index));
    }

    total_cuts += install_aperture_backward_cut(scene_index,
                                                src_phase_index,
                                                phase_index,
                                                total_cuts,
                                                iteration_index,
                                                src_alpha_col,
                                                src_state,
                                                target_state,
                                                std::move(expected_cut),
                                                src_li,
                                                opts);

    // ── Per-cell release: drop target_sys's backend now ──
    // Same rationale as `backward_pass_aperture_phase_impl` (sync path
    // counterpart): apertures done, clones already destroyed, cut
    // installed on `src_li` (different cell), `target_sys` not
    // touched again in this scene's backward pass.
    if (m_options_.low_memory_mode != LowMemoryMode::off) {
      target_sys.release_backend();
    }
  }

  // ── Per-cell release: phase 0 src cell ──
  // The loop terminates at `phase_index = 1`, so phase 0 is never a
  // target.  Its last touch is the resolve at the end of
  // `install_aperture_backward_cut` for `phase_index = 1`.  Release
  // it here so the bulk-loop safety net at the end of the iteration
  // has nothing left to do (under cut_sharing == none — the only
  // mode that reaches this non-synchronized loop).
  if (m_options_.low_memory_mode != LowMemoryMode::off) {
    planning_lp().system(scene_index, first_phase_index()).release_backend();
  }

  // Log a single summary for all phases with infeasible apertures
  if (!infeasible_phases.empty()) {
    SPDLOG_WARN(
        "{}: all apertures infeasible at {} phase(s) [{}], "
        "used Benders fallback cuts",
        sddp_log(
            "Aperture", gtopt::uid_of(iteration_index), uid_of(scene_index)),
        infeasible_phases.size(),
        join_values(infeasible_phases));
  }

  return total_cuts;
}

}  // namespace gtopt
