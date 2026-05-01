/**
 * @file      sddp_method_iteration.cpp
 * @brief     SDDPMethod per-iteration coordination + diagnostics
 * @date      2026-04-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Sibling translation unit of ``sddp_method.cpp``; carries a
 * focused subset of ``SDDPMethod``'s member functions to keep
 * each TU under ~700 LoC.  Split landed in commit referenced by
 * Phase B of the gtopt-hygiene refactor.  See
 * ``include/gtopt/sddp_method.hpp`` for the class declaration
 * and ``source/sddp_method.cpp`` for the constructor / solver
 * lifecycle helpers.
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
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_status.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

void SDDPMethod::update_max_kappa(SceneIndex scene_index,
                                  PhaseIndex phase_index,
                                  const LinearInterface& li,
                                  IterationIndex iteration_index)
{
  const auto kappa_opt = li.get_kappa();

  // No value means the backend doesn't support the query (e.g. MindOpt)
  // or the native query failed (e.g. no basis after barrier w/o
  // crossover).  In that case leave the grid cell untouched: the
  // internal -1.0 "unset" storage sentinel is fine, and no downstream
  // consumer sees it because gtopt_lp_runner guards with `> 0.0`.
  if (!kappa_opt.has_value()) {
    return;
  }
  const double kappa = *kappa_opt;

  m_max_kappa_[scene_index][phase_index] =
      std::max(m_max_kappa_[scene_index][phase_index], kappa);

  const auto& sim = planning_lp().planning().simulation;
  const auto mode = sim.kappa_warning.value_or(KappaWarningMode::warn);
  if (mode == KappaWarningMode::none) {
    return;
  }

  constexpr double default_kappa_threshold = 1e9;
  const double threshold =
      sim.kappa_threshold.value_or(default_kappa_threshold);
  if (kappa <= threshold) {
    return;
  }

  spdlog::warn("SDDP Kappa [i{} s{} p{}]: high kappa {:.2e} (threshold {:.2e})",
               iteration_index,
               uid_of(scene_index),
               uid_of(phase_index),
               kappa,
               threshold);

  const bool should_save_lp =
      (mode == KappaWarningMode::save_lp || mode == KappaWarningMode::diagnose);
  if (should_save_lp && !m_options_.log_directory.empty()) {
    std::filesystem::create_directories(m_options_.log_directory);
    const auto stem = (std::filesystem::path(m_options_.log_directory)
                       / as_label("kappa",
                                  uid_of(scene_index),
                                  uid_of(phase_index),
                                  iteration_index))
                          .string();
    if (auto r = li.write_lp(stem)) {
      spdlog::warn("Saved high-kappa LP to {}.lp", stem);
    }
  }

  if (mode == KappaWarningMode::diagnose) {
    diagnose_kappa(scene_index, phase_index, li, iteration_index);
  }
}

void SDDPMethod::diagnose_kappa(SceneIndex scene_index,
                                PhaseIndex phase_index,
                                const LinearInterface& li,
                                IterationIndex iteration_index)
{
  const auto prefix = sddp_log(
      "Kappa", iteration_index, uid_of(scene_index), uid_of(phase_index));

  // Collect cut rows for this (scene, phase) from the cut store
  const auto& scene_cuts = m_cut_store_.scene_cuts();
  const auto phase_uid_val = uid_of(phase_index);

  std::vector<RowDiagnostics> cut_diags;

  // Check per-scene cuts
  if (scene_index < std::ssize(scene_cuts)) {
    for (const auto& cut : scene_cuts[scene_index]) {
      if (cut.phase_uid != phase_uid_val) {
        continue;
      }
      cut_diags.push_back(li.diagnose_row(cut.row));
    }
  }

  // Also check other scenes' cuts for the same (phase) — under
  // CutSharingMode::max / expected / accumulate, cuts from other
  // scenes are also present on this scene's LP row span.  Walk every
  // scene's vector, dedup by row index.
  for (auto&& [other_si, other_cuts] :
       enumerate<SceneIndex>(m_cut_store_.scene_cuts()))
  {
    if (other_si == scene_index) {
      continue;  // already covered above
    }
    for (const auto& cut : other_cuts) {
      if (cut.phase_uid != phase_uid_val) {
        continue;
      }
      const bool already = std::ranges::any_of(
          cut_diags, [&](const auto& d) { return d.row == cut.row; });
      if (!already) {
        cut_diags.push_back(li.diagnose_row(cut.row));
      }
    }
  }

  if (cut_diags.empty()) {
    spdlog::warn("{}: no cut rows found for diagnosis", prefix);
    return;
  }

  // Sort by worst coefficient ratio (descending)
  std::ranges::sort(cut_diags,
                    [](const auto& a, const auto& b)
                    { return a.coeff_ratio > b.coeff_ratio; });

  spdlog::warn("{}: diagnosing {} cut rows...", prefix, cut_diags.size());

  // Report top 5 worst cuts
  constexpr int max_report = 5;
  const auto report_count =
      std::min(max_report, static_cast<int>(cut_diags.size()));
  for (int i = 0; i < report_count; ++i) {
    const auto& d = cut_diags[static_cast<size_t>(i)];
    spdlog::warn(
        "{}:   #{} {} ratio={:.2e} [{:.2e}, {:.2e}] rhs={:.2e} nnz={}"
        " min_col={} max_col={}",
        prefix,
        i + 1,
        d.name,
        d.coeff_ratio,
        d.min_abs_coeff,
        d.max_abs_coeff,
        d.rhs_lb,
        d.num_nonzeros,
        d.min_col_name,
        d.max_col_name);
  }

  // Summary: overall cut coefficient range
  double global_min = std::numeric_limits<double>::max();
  double global_max = 0.0;
  for (const auto& d : cut_diags) {
    if (d.num_nonzeros > 0) {
      global_min = std::min(global_min, d.min_abs_coeff);
      global_max = std::max(global_max, d.max_abs_coeff);
    }
  }
  if (global_max > 0.0 && global_min < std::numeric_limits<double>::max()) {
    spdlog::warn("{}:   cut coeff range: [{:.2e}, {:.2e}] ratio={:.2e}",
                 prefix,
                 global_min,
                 global_max,
                 global_max / global_min);
  }

  // Report LP matrix stats (from initial build) for comparison
  spdlog::warn("{}:   LP matrix stats: nnz={} |coeff| in [{:.2e}, {:.2e}]",
               prefix,
               li.lp_stats_nnz(),
               li.lp_stats_min_abs(),
               li.lp_stats_max_abs());
}

// ─── Free-function building blocks ──────────────────────────────────────────
// Now implemented in benders_cut.cpp; this file uses them via benders_cut.hpp.

// Now implemented in benders_cut.cpp; this file uses them via benders_cut.hpp.

// ── Helper: local utilities ─────────────────────────────────────────────────

bool SDDPMethod::should_dispatch_update_lp(IterationIndex iteration_index) const
{
  // Three-way logic from the preallocated iteration vector:
  //  - update_lp == false  → explicitly skip
  //  - update_lp == true   → force dispatch (bypass skip count)
  //  - not specified        → default behaviour (respect global skip count)
  if (std::cmp_less(Index {iteration_index}, m_iterations_.size())) {
    const auto& iter_lp = m_iterations_[iteration_index];

    if (iter_lp.has_explicit_update_lp()) {
      return iter_lp.should_update_lp();
    }
    // Default: apply global skip count using relative iteration
    const auto skip = planning_lp().options().sddp_update_lp_skip();
    const auto rel = iteration_relative(iteration_index, m_iteration_offset_);
    if (skip > 0 && rel > 0 && (rel % (skip + 1)) != 0) {
      return false;
    }
  }
  return true;
}

int SDDPMethod::update_lp_for_phase(SceneIndex scene_index,
                                    PhaseIndex phase_index)
{
  auto& sys = planning_lp().system(scene_index, phase_index);

  // Set previous phase's SystemLP unconditionally so that update_lp
  // elements (seepage, production factor, discharge limit) can look up
  // the previous phase's optimal efin when re-evaluating volume-
  // dependent linearisations via physical_eini.
  //
  // The pointer is consumed read-only — solely for segment selection
  // during LP coefficient updates — and physical_eini only follows the
  // cross-phase branch when the predecessor LP is_optimal().  It does
  // not change state-variable propagation, which is governed by the
  // separate sddp_state_variable_lookup_mode option (warm_start vs
  // cross_phase) handled in propagate_trial_values.
  //
  // Previously this pointer was only set when lookup == cross_phase,
  // leaving warm_start (the default) with prev_phase_sys = nullptr.
  // physical_eini then fell through to its JSON `eini` fallback (e.g.
  // 1731 Hm³ for ELTORO), pinning the seepage segment to the
  // construction-time volume regardless of the actual forward-pass
  // trajectory.  Observed on juan/gtopt_iplp p38 where the resulting
  // 15.09 m³/s seepage baseflow exceeded the scheduled affluent and
  // structurally broke the LP after the forward pass had drained the
  // reservoir to zero.
  if (phase_index) {
    const auto prev_phase_index = previous(phase_index);
    sys.set_prev_phase_sys(
        &planning_lp().system(scene_index, prev_phase_index));
  } else {
    sys.set_prev_phase_sys(nullptr);
  }

  return sys.update_lp();
}

void SDDPMethod::dispatch_update_lp(SceneIndex scene_index,
                                    IterationIndex iteration_index)
{
  if (!should_dispatch_update_lp(iteration_index)) {
    return;
  }

  const auto num_phases = planning_lp().simulation().phase_count();

  for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
    const auto updated = update_lp_for_phase(scene_index, phase_index);

    if (updated > 0) {
      SPDLOG_TRACE("SDDP Update [i{} s{} p{}]: updated {} LP elements",
                   iteration_index,
                   uid_of(scene_index),
                   uid_of(phase_index),
                   updated);
    }
  }
}

// ── Forward pass ────────────────────────────────────────────────────────────

// ── SDDP task priority helpers ───────────────────────────────────────────────

// `make_forward_lp_task_req` and `make_backward_lp_task_req` are now declared
// in `<gtopt/sddp_pool.hpp>` (next to `make_sddp_task_key`) so the priority-
// key ordering invariant can be covered by unit tests.  The previous
// anonymous-namespace duplicates were removed in the audit pass.

// ── forward_pass() — now in sddp_forward_pass.cpp ───────────────────────────

// ── Helper: store a cut for sharing and persistence (thread-safe) ───────────

auto SDDPMethod::resolve_via_pool(
    LinearInterface& li,
    const SolverOptions& opts,
    const BasicTaskRequirements<SDDPTaskKey>& task_req)
    -> std::expected<int, Error>
{
  if (m_pool_ == nullptr) {
    // No pool available — fall back to direct solve
    return li.resolve(opts);
  }

  auto fut =
      m_pool_->submit([&li, &opts] { return li.resolve(opts); }, task_req);
  if (fut.has_value()) {
    return fut->get();
  }
  // Pool submission failed — fall back to direct solve
  SPDLOG_WARN("resolve_via_pool: pool submit failed, falling back to direct");
  return li.resolve(opts);
}

// ── Helper: resolve a clone via the work pool ───────────────────────────────

auto SDDPMethod::resolve_clone_via_pool(
    LinearInterface& clone,
    const SolverOptions& opts,
    const BasicTaskRequirements<SDDPTaskKey>& task_req)
    -> std::expected<int, Error>
{
  if (m_pool_ == nullptr) {
    return clone.resolve(opts);
  }

  // Submit resolve to the pool.  The clone reference is safe because we
  // call future.get() synchronously before this scope exits.
  auto fut = m_pool_->submit([&clone, &opts] { return clone.resolve(opts); },
                             task_req);
  if (fut.has_value()) {
    return fut->get();
  }
  // Pool submission failed — fall back to direct solve
  SPDLOG_WARN(
      "resolve_clone_via_pool: pool submit failed, falling back to direct");
  return clone.resolve(opts);
}

// ── feasibility_backpropagate() removed — forward pass installs fcuts ──────

// ── Per-phase backward-pass step (optimality cut only; no feasibility sharing)

auto SDDPMethod::backward_pass_single_phase(SceneIndex scene_index,
                                            PhaseIndex phase_index,
                                            int cut_offset,
                                            const SolverOptions& opts,
                                            IterationIndex iteration_index)
    -> std::expected<int, Error>
{
  // Fine-grained stage timing for the backward-cut step.  Each pair of
  // chrono::steady_clock::now() calls is O(100ns); the stages they
  // bracket dominate by 3-5 orders of magnitude (LP resolve, kappa
  // query) so the overhead is irrelevant.  The counters land on the
  // previous-phase LP's SolverStats so end-of-run aggregation and
  // per-iteration diffing both fall out of the existing infrastructure.
  using Clock = std::chrono::steady_clock;
  const auto elapsed_s = [](Clock::time_point start) noexcept
  { return std::chrono::duration<double>(Clock::now() - start).count(); };

  auto& phase_states = m_scene_phase_states_[scene_index];
  int cuts_added = 0;

  const auto prev_phase_index = previous(phase_index);
  auto& src_sys = planning_lp().system(scene_index, prev_phase_index);
  const auto& src_state = phase_states[prev_phase_index];

  // Ensure the previous-phase LP is built.  No-op when backend is live
  // (mode=off, or a prior task already rebuilt it); otherwise reloads
  // from snapshot (snapshot/compress) or re-flattens from collections
  // (rebuild).
  const auto t_rebuild = Clock::now();
  src_sys.ensure_lp_built();
  const auto dt_rebuild = elapsed_s(t_rebuild);

  auto& src_li = src_sys.linear_interface();

  // Use cached forward-pass solution for cut generation.
  const auto& target_state = phase_states[phase_index];

  const auto ceps = m_options_.cut_coeff_eps;
  // Physical-space builder: reduced costs and trial values come from
  // each link's back-pointer StateVariable (mirrored by the last
  // forward pass via `capture_state_variable_values`).  `add_row` on
  // the source LP folds `col_scales` and runs per-row row-max
  // equilibration, so no post-hoc `rescale_benders_cut` pass is needed.
  //
  // Resolve the α column freshly from the state-variable registry so
  // low_memory reconstruct paths never see a stale cached index.
  const auto* src_alpha_svar = find_alpha_state_var(
      planning_lp().simulation(), scene_index, prev_phase_index);
  const auto src_alpha_col = (src_alpha_svar != nullptr)
      ? src_alpha_svar->col()
      : ColIndex {unknown_index};

  const auto scale_obj = planning_lp().options().scale_objective();
  const auto t_build = Clock::now();
  auto cut = build_benders_cut_physical(src_alpha_col,
                                        src_state.outgoing_links,
                                        target_state.forward_full_obj_physical,
                                        scale_obj,
                                        ceps);
  sddp_scut_tag.apply_to(cut);
  cut.variable_uid = uid_of(prev_phase_index);
  cut.context = make_iteration_context(uid_of(scene_index),
                                       uid_of(phase_index),
                                       gtopt::uid_of(iteration_index),
                                       cut_offset);
  const auto dt_build = elapsed_s(t_build);

  // Unified `add_cut_row`: releases α on `prev_phase_index` iff the
  // cut references α, then adds + records the row for low-memory
  // replay in one call.
  const auto t_add_row = Clock::now();
  const auto cut_row = gtopt::add_cut_row(planning_lp(),
                                          scene_index,
                                          prev_phase_index,
                                          CutType::Optimality,
                                          cut,
                                          ceps);
  const auto dt_add_row = elapsed_s(t_add_row);

  const auto t_store = Clock::now();
  store_cut(scene_index, prev_phase_index, cut, CutType::Optimality, cut_row);
  const auto dt_store = elapsed_s(t_store);

  ++cuts_added;
  m_phase_grid_.record(
      iteration_index, uid_of(scene_index), phase_index, GridCell::Backward);

  SPDLOG_TRACE("SDDP Backward [i{} s{} p{}]: cut for phase {} rhs={:.4f}",
               iteration_index,
               uid_of(scene_index),
               uid_of(phase_index),
               uid_of(prev_phase_index),
               cut.lowb);

  // Re-solve src_li so downstream code (the async iteration's
  // per-scene LB read at sddp_iteration.cpp:929 — `lower_bound =
  // first_phase.linear_interface().get_obj_value()`) sees a fresh
  // post-cut optimum, and kappa tracking can run.
  //
  // NOTE: src_li was optimal when this backward step was entered, and
  // the cut is a valid Benders underestimator — adding it cannot make
  // the LP infeasible.  If the resolve still reports non-optimal it is
  // a numerical artifact (cut coefficients pushing the solver into a
  // degenerate basis); we log it but do NOT fail the iteration.  The
  // cut row is already installed and the next forward pass will re-
  // solve from a fresh basis.  Mirrors the bcut-path simplification in
  // sddp_aperture_pass.cpp where we removed the corresponding resolve
  // entirely (the bcut path doesn't feed into async LB computation).
  double dt_resolve = 0.0;
  double dt_kappa = 0.0;
  if (phase_index) {
    src_li.set_log_tag(sddp_log("Backward",
                                iteration_index,
                                uid_of(scene_index),
                                uid_of(prev_phase_index)));
    const auto t_resolve = Clock::now();
    auto r = src_li.resolve(opts);
    dt_resolve = elapsed_s(t_resolve);
    if (r.has_value() && src_li.is_optimal()) {
      const auto t_kappa = Clock::now();
      update_max_kappa(scene_index, prev_phase_index, src_li, iteration_index);
      dt_kappa = elapsed_s(t_kappa);
    } else {
      SPDLOG_DEBUG(
          "{}: post-cut resolve non-optimal (status {}) — keeping "
          "cut, next forward pass will re-solve",
          sddp_log("Backward",
                   iteration_index,
                   uid_of(scene_index),
                   uid_of(prev_phase_index)),
          src_li.get_status());
    }
  }

  // Fold this step's timings into the previous-phase LP's SolverStats.
  // Single-writer per-LP invariant holds: phase access within a scene
  // is serial during the backward pass (both the async and
  // phase-synchronised variants obey this), so no atomics are needed.
  auto& sstats = src_li.mutable_solver_stats();
  ++sstats.bwd_step_count;
  sstats.bwd_lp_rebuild_s += dt_rebuild;
  sstats.bwd_cut_build_s += dt_build;
  sstats.bwd_add_row_s += dt_add_row;
  sstats.bwd_store_cut_s += dt_store;
  sstats.bwd_resolve_s += dt_resolve;
  sstats.bwd_kappa_s += dt_kappa;

  return cuts_added;
}

// ── Backward pass with iterative feasibility backpropagation ────────────────

auto SDDPMethod::backward_pass(SceneIndex scene_index,
                               const SolverOptions& opts,
                               IterationIndex iteration_index)
    -> std::expected<int, Error>
{
  const auto num_phases = planning_lp().simulation().phase_count();
  int total_cuts = 0;

  SPDLOG_DEBUG("SDDP Backward [i{} s{}]: starting ({} phases)",
               iteration_index,
               uid_of(scene_index),
               num_phases);

  // Iterate backward from last phase to phase 1
  for (const auto phase_index :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format("{}: cancelled",
                                 sddp_log("Backward",
                                          iteration_index,
                                          uid_of(scene_index),
                                          uid_of(phase_index))),
      });
    }

    auto step_result = backward_pass_single_phase(
        scene_index, phase_index, total_cuts, opts, iteration_index);
    if (!step_result.has_value()) {
      return std::unexpected(std::move(step_result.error()));
    }
    total_cuts += *step_result;

    // ── Per-cell release: drop (scene, phase_index - 1) backend now ──
    //
    // `backward_pass_single_phase` operates on
    // `prev_phase = phase_index - 1`: it `ensure_lp_built`s the cell,
    // adds an optimality cut, and resolves it.  In this non-
    // synchronized path (cut_sharing == none) there is no cross-scene
    // share step that would re-touch the cell after the loop body
    // returns, so the LP is no longer needed in this scene's
    // backward pass — the next loop iteration's call uses
    // `phase_index - 2`.  Release synchronously inside the worker
    // (single-threaded per scene) and let the scheduler overlap the
    // release with other scenes' tasks.  Mirrors the synchronised-
    // path release after `share_cuts_for_phase` so both paths converge
    // to the same end-state ahead of the bulk safety-net loop.
    if (m_options_.low_memory_mode != LowMemoryMode::off) {
      const auto prev_phase_index = previous(phase_index);
      planning_lp().system(scene_index, prev_phase_index).release_backend();
    }
  }

  SPDLOG_DEBUG("SDDP Backward [i{} s{}]: done, {} cuts added",
               iteration_index,
               uid_of(scene_index),
               total_cuts);
  return total_cuts;
}

// ── Cut sharing (delegated to sddp_cut_sharing.hpp free function) ───────────

auto SDDPMethod::run_forward_pass_all_scenes(SDDPWorkPool& pool,
                                             const SolverOptions& opts,
                                             IterationIndex iteration_index)
    -> std::expected<ForwardPassOutcome, Error>
{
  const auto num_scenes = planning_lp().simulation().scene_count();

  m_current_pass_.store(1);
  m_scenes_done_.store(0);

  SPDLOG_INFO("SDDP Forward [i{}]: dispatching {} scene(s) to work pool",
              iteration_index,
              num_scenes);

  // Stall-stop guard for `forward_infeas_rollback` — see
  // `support/scene_infeasibility_rollback_plan_2026-04-30.md` §2.4.
  // For every scene that failed last iteration (and had its cuts
  // rolled back), check whether the global stored-cut count has grown
  // since the failure.  If it has (peers contributed cuts via cut
  // sharing or their own backward pass), the scene retries this iter:
  // clear the failure marker so the post-pass hook below can flag a
  // *new* failure if it happens again.  If the global count has not
  // grown, the scene is "stalled" — cuts that arrive only from
  // peers via share_cuts_for_phase aren't tracked in
  // m_cut_store_.scene_cuts (they live as LP rows + replay buffer
  // only), so the global cut count is the right proxy: if zero new
  // cuts have entered the cut store across every scene, no recovery
  // path exists for this stalled scene and we abort cleanly to
  // avoid an infinite-loop.
  if (m_options_.forward_infeas_rollback) {
    const auto current_global_cuts = m_cut_store_.num_stored_cuts();
    std::ptrdiff_t failed_last = 0;
    std::ptrdiff_t stalled = 0;
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      auto& rs = m_scene_retry_state_[si];
      if (!rs.global_cuts_at_last_failure.has_value()) {
        continue;
      }
      ++failed_last;
      if (current_global_cuts > *rs.global_cuts_at_last_failure) {
        rs.global_cuts_at_last_failure.reset();  // scene retries
      } else {
        ++stalled;
      }
    }
    if (failed_last > 0 && stalled == failed_last) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message =
              std::format("SDDP: {} scene(s) infeasible at iter {} and "
                          "zero new cuts since their last failure — no "
                          "recovery path (forward_infeas_rollback stall)",
                          stalled,
                          iteration_index),
      });
    }
  }

  const auto fwd_start = std::chrono::steady_clock::now();
  // Snapshot total stored cuts before the pass.  Forward passes only
  // install feasibility cuts (`store_cut(..., CutType::Feasibility, ...)`
  // at sddp_forward_pass.cpp), so the post-pass delta is exactly the
  // count of fcuts installed across all scenes this attempt.  Used by
  // the simulation retry loop to detect "no progress" attempts.
  const auto cuts_before_pass = m_cut_store_.num_stored_cuts();

  std::vector<std::future<std::expected<double, Error>>> futures;
  futures.reserve(num_scenes);

  // Forward-pass scene tasks use High priority; lower iteration = higher key.
  const auto fwd_req =
      make_forward_lp_task_req(iteration_index, first_phase_index());

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    auto fut = pool.submit(
        [this, scene_index, iteration_index, &opts]
        { return forward_pass(scene_index, opts, iteration_index); },
        fwd_req);
    futures.push_back(std::move(fut.value()));
  }

  ForwardPassOutcome out;
  out.scene_upper_bounds.resize(num_scenes, 0.0);
  out.scene_feasible.resize(num_scenes, 1);

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    auto fwd = futures[scene_index].get();
    if (!fwd.has_value()) {
      SPDLOG_WARN("SDDP Forward [i{} s{}]: failed: {}",
                  iteration_index,
                  uid_of(scene_index),
                  fwd.error().message);
      out.has_feasibility_issue = true;
      out.scene_feasible[scene_index] = 0;
      m_scenes_done_.fetch_add(1);
      continue;
    }
    out.scene_upper_bounds[scene_index] = *fwd;
    ++out.scenes_solved;
    m_scenes_done_.fetch_add(1);
  }

  // Per-scene rollback hook for `forward_infeas_rollback` — see
  // `support/scene_infeasibility_rollback_plan_2026-04-30.md` §2.3.
  // For every scene S declared infeasible this iteration, drop every
  // cut S has accumulated in the global cut store (both forward-pass
  // fcuts installed during PLP-style backtrack chains and earlier
  // backward-pass optcuts).  Snapshot the global cut count *after*
  // rollback so the next iteration's stall-stop guard measures only
  // new cuts contributed by peers.
  if (m_options_.forward_infeas_rollback) {
    std::ptrdiff_t total_rollback_rows = 0;
    int n_rolled_back = 0;
    // Build a per-scene breakdown for the user-facing log so a 10-
    // scene rollback shows exactly which scenes lost how many cut
    // rows.  Format: "s1=8 s3=8 s6=8 …"
    std::string per_scene_breakdown;
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      if (out.scene_feasible[scene_index] != 0) {
        continue;
      }
      const auto deleted =
          m_cut_store_.clear_scene_cuts(scene_index, planning_lp());
      if (deleted > 0) {
        ++n_rolled_back;
        total_rollback_rows += deleted;
        if (!per_scene_breakdown.empty()) {
          per_scene_breakdown += ' ';
        }
        per_scene_breakdown +=
            std::format("s{}={}", uid_of(scene_index), deleted);
      }
      m_scene_retry_state_[scene_index].global_cuts_at_last_failure =
          m_cut_store_.num_stored_cuts();
    }
    if (n_rolled_back > 0) {
      SPDLOG_INFO(
          "SDDP Forward [i{}]: rolled back {} cut row(s) across {} "
          "infeasible scene(s) [{}] (forward_infeas_rollback)",
          iteration_index,
          total_rollback_rows,
          n_rolled_back,
          per_scene_breakdown);
    }
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - fwd_start)
                      .count();

  // Post-pass cut count - pre-pass snapshot = fcuts installed this pass.
  // Non-negative because cuts are only added (never removed) during a
  // forward pass under the legacy path; under
  // `forward_infeas_rollback` the rollback hook above may also delete
  // cuts, in which case `cuts_after_pass < cuts_before_pass` is
  // possible — the `max(..., 0)` keeps `n_fcuts_installed` non-negative
  // (the field is consumed only by the simulation retry loop's
  // "no progress" check, where a negative delta should still register
  // as zero progress).
  const auto cuts_after_pass = m_cut_store_.num_stored_cuts();
  out.n_fcuts_installed = static_cast<std::size_t>(
      std::max<std::ptrdiff_t>(0, cuts_after_pass - cuts_before_pass));

  m_current_pass_.store(0);

  // Only declare the run unrecoverably stuck when ZERO scenes survived AND
  // ZERO feasibility cuts were installed.  When fail-stop scenes installed
  // fcuts on predecessor phases (the documented "scene-level fail-stop"
  // path in sddp_forward_pass.cpp:528-558), those cuts persist in the
  // global cut store and can make the master produce different state
  // outputs in the next iteration.  Aborting here would discard them and
  // contradict the fail-stop design comment ("the next iteration restarts
  // every scene's forward pass from p1 with the freshly accumulated cuts").
  if (out.scenes_solved == 0 && out.n_fcuts_installed == 0) {
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = "SDDP: all scenes infeasible in forward pass "
                   "and no recoverable feasibility cuts were installed",
    });
  }

  return out;
}

auto SDDPMethod::run_backward_pass_all_scenes(
    std::span<const uint8_t> scene_feasible,
    SDDPWorkPool& pool,
    const SolverOptions& opts,
    IterationIndex iteration_index) -> BackwardPassOutcome
{
  const auto& bwd_opts = opts;

  m_current_pass_.store(2);
  m_scenes_done_.store(0);

  // When cut sharing is enabled, use the phase-synchronized backward pass:
  // all scenes complete a phase before cuts are shared and the next phase
  // is processed.  When sharing is disabled (None), scenes run their full
  // backward pass independently in parallel with no synchronization.
  if (m_options_.cut_sharing != CutSharingMode::none) {
    auto result = run_backward_pass_synchronized(
        scene_feasible, pool, bwd_opts, iteration_index);
    m_current_pass_.store(0);
    return result;
  }

  const auto num_scenes = planning_lp().simulation().scene_count();

  SPDLOG_INFO(
      "SDDP Backward [i{}]: dispatching {} scene(s) to work pool "
      "(cut_sharing=none, apertures={})",
      iteration_index,
      num_scenes,
      !m_options_.apertures || !m_options_.apertures->empty() ? "enabled"
                                                              : "disabled");

  const auto bwd_start = std::chrono::steady_clock::now();
  std::vector<std::future<std::expected<int, Error>>> futures;
  futures.reserve(num_scenes);

  // Backward-pass scene tasks use Medium priority; scenes with lower index
  // get slightly higher priority_key (phase 0 = lowest phase index).
  const auto bwd_req =
      make_backward_lp_task_req(iteration_index, first_phase_index());

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    if (scene_feasible[scene_index] == 0U) {
      continue;
    }
    const bool use_ap = !m_options_.apertures || !m_options_.apertures->empty();
    auto fut = use_ap
        ? pool.submit(
              [this, scene_index, &bwd_opts, iteration_index]
              {
                return backward_pass_with_apertures(
                    scene_index, bwd_opts, iteration_index);
              },
              bwd_req)
        : pool.submit(
              [this, scene_index, &bwd_opts, iteration_index]
              { return backward_pass(scene_index, bwd_opts, iteration_index); },
              bwd_req);
    futures.push_back(std::move(fut.value()));
  }

  BackwardPassOutcome out;
  for (auto& fut : futures) {
    auto bwd = fut.get();
    if (!bwd.has_value()) {
      SPDLOG_WARN("SDDP Backward [i{}]: failed: {}",
                  iteration_index,
                  bwd.error().message);
      out.has_feasibility_issue = true;
      m_scenes_done_.fetch_add(1);
      continue;
    }
    out.total_cuts += *bwd;
    m_scenes_done_.fetch_add(1);
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - bwd_start)
                      .count();

  m_current_pass_.store(0);
  return out;
}

// ── Phase-synchronized backward pass (for cut sharing) ──────────────────────

auto SDDPMethod::run_backward_pass_synchronized(
    std::span<const uint8_t> scene_feasible,
    SDDPWorkPool& pool,
    const SolverOptions& opts,
    IterationIndex iteration_index) -> BackwardPassOutcome
{
  const auto num_scenes = planning_lp().simulation().scene_count();
  const auto num_phases = planning_lp().simulation().phase_count();
  const auto feasible_scenes = static_cast<std::ptrdiff_t>(
      std::ranges::count(scene_feasible, uint8_t {1}));

  // INFO-level start log so users tailing trace logs see the
  // backward pass kick off (the non-synchronized path has the
  // matching log at line ~764).  Without this the log goes silent
  // between "Forward [i0] rolled back N cuts" and the next
  // iteration summary, which on juan-scale runs is several
  // minutes of aperture solves.
  SPDLOG_INFO(
      "SDDP Backward [i{}]: dispatching {}/{} feasible scene(s) × "
      "{} phase(s) (cut_sharing={}, apertures={})",
      iteration_index,
      feasible_scenes,
      num_scenes,
      num_phases - 1,
      enum_name(m_options_.cut_sharing),
      !m_options_.apertures || !m_options_.apertures->empty() ? "enabled"
                                                              : "disabled");

  const auto bwd_start = std::chrono::steady_clock::now();
  BackwardPassOutcome out;

  // Per-scene cumulative cut count for unique cut labels across phase steps
  std::vector<int> per_scene_cut_count(static_cast<std::size_t>(num_scenes), 0);

  // Apertures enabled unless explicitly set to empty array
  const bool use_apertures =
      !m_options_.apertures || !m_options_.apertures->empty();

  // Aggregate per-scene cut-receive counts across the whole pass so
  // we can tell, at end-of-pass, whether infeasible scenes (which
  // were skipped at dispatch) actually receive shared cuts from
  // their feasible peers via `share_cuts_for_phase`.  A scene with
  // `feasible[s] == 0` should still see this counter grow under any
  // `cut_sharing != none` mode — that's the evidence the rollback
  // stall-stop guard reads next iteration.
  std::vector<std::ptrdiff_t> per_scene_cuts_received(
      static_cast<std::size_t>(num_scenes), 0);

  // Process phases backward: all scenes complete one phase before
  // sharing cuts and moving to the previous phase.
  for (const auto phase_index :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    // Snapshot each scene's cut-count before this phase step so we
    // can identify newly-added cuts below for cross-scene sharing.
    std::vector<std::size_t> per_scene_before(
        static_cast<std::size_t>(num_scenes), 0);
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      per_scene_before[static_cast<std::size_t>(si)] =
          m_cut_store_.at(si).size();
    }

    // Submit all feasible scenes for this phase step in parallel
    std::vector<std::pair<SceneIndex, std::future<std::expected<int, Error>>>>
        futures;
    futures.reserve(num_scenes);

    const auto bwd_req =
        make_backward_lp_task_req(iteration_index, phase_index);

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      if (scene_feasible[scene_index] == 0U) {
        continue;
      }
      const int offset = per_scene_cut_count[scene_index];

      auto fut = use_apertures
          ? pool.submit(
                [this, scene_index, phase_index, offset, &opts, iteration_index]
                {
                  return backward_pass_with_apertures_single_phase(
                      scene_index, phase_index, offset, opts, iteration_index);
                },
                bwd_req)
          : pool.submit(
                [this, scene_index, phase_index, offset, &opts, iteration_index]
                {
                  return backward_pass_single_phase(
                      scene_index, phase_index, offset, opts, iteration_index);
                },
                bwd_req);
      futures.emplace_back(scene_index, std::move(fut.value()));
    }

    // Wait for all scenes to complete this phase step
    for (auto& [scene_index, fut] : futures) {
      auto step_result = fut.get();
      if (!step_result.has_value()) {
        SPDLOG_WARN("SDDP backward synchronized: scene {} phase {} failed: {}",
                    scene_index,
                    phase_index,
                    step_result.error().message);
        out.has_feasibility_issue = true;
        m_scenes_done_.fetch_add(1);
        continue;
      }
      out.total_cuts += *step_result;
      per_scene_cut_count[scene_index] += *step_result;
      m_scenes_done_.fetch_add(1);
    }

    // Share optimality cuts generated in this phase step across all scenes.
    // Feasibility cuts are stored but only optimality cuts are shared.
    const auto src_phase_index = previous(phase_index);

    StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
    scene_cuts.resize(num_scenes);

    // Collect newly-added optimality cuts on src_phase_index from
    // each scene's vector using the per-scene-before offsets
    // snapshotted above.  No lock needed — the scene-step barrier
    // (fut.get() loop above) already synchronises.
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      const auto& cuts = m_cut_store_.at(si);
      for (std::size_t ci = per_scene_before[si]; ci < cuts.size(); ++ci) {
        const auto& sc = cuts[ci];
        if (sc.type != CutType::Optimality) {
          continue;
        }
        if (sc.phase_uid != uid_of(src_phase_index)) {
          continue;
        }
        auto row = SparseRow {
            .lowb = sc.rhs,
            .uppb = LinearProblem::DblMax,
            .scale = sc.scale,
        };
        for (const auto& [col, coeff] : sc.coefficients) {
          row[col] = coeff;
        }
        scene_cuts[si].push_back(std::move(row));
      }
    }

    // Periodic INFO progress every 10 phases on long backward
    // passes — juan-scale runs have 50 phases × ~80 cells per
    // phase × 2-3s per aperture solve, so the synchronized loop
    // can take many minutes silent.  10 phases ≈ once per minute
    // on juan, frequent enough for users to see progress without
    // flooding the log on smaller fixtures.
    if (uid_of(phase_index) % 10 == 0) {
      const auto elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - bwd_start)
                               .count();
      SPDLOG_INFO(
          "SDDP Backward [i{}]: phase p{} done — {} cut(s) so far, "
          "{:.1f}s elapsed",
          iteration_index,
          uid_of(phase_index),
          out.total_cuts,
          elapsed);
    }

    // Snapshot post-share row counts on each scene's
    // (scene, src_phase_index) cell so we can attribute newly-added
    // rows to the share broadcast.  `share_cuts_for_phase` adds
    // rows to every destination's LP via `add_cut_row`; the delta
    // tells us how many cuts each scene received.
    std::vector<std::size_t> rows_before_share(
        static_cast<std::size_t>(num_scenes), 0);
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      rows_before_share[static_cast<std::size_t>(si)] =
          planning_lp()
              .system(si, src_phase_index)
              .linear_interface()
              .get_numrows();
    }

    share_cuts_for_phase(src_phase_index, scene_cuts, iteration_index);

    // Per-scene receive count = post - pre on the destination
    // (scene, src_phase_index) cell.
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      const auto si_sz = static_cast<std::size_t>(si);
      const auto rows_after = planning_lp()
                                  .system(si, src_phase_index)
                                  .linear_interface()
                                  .get_numrows();
      const auto delta = static_cast<std::ptrdiff_t>(rows_after)
          - static_cast<std::ptrdiff_t>(rows_before_share[si_sz]);
      if (delta > 0) {
        per_scene_cuts_received[si_sz] += delta;
      }
    }

    // ── Per-cell release for src_phase = first phase (last loop iter) ──
    //
    // Cells `(scene, k)` are touched twice in the backward pass: once
    // at iter `phase_index = k+1` as `src` (cut added + resolved +
    // peer cuts via share), and once at iter `phase_index = k` as
    // `target` (apertures clone from it).  The optimal release point
    // is the end of iter k's per-scene worker — handled inside
    // `backward_pass_with_apertures_single_phase`.  The exception is
    // `(scene, 0)`: phase 0 is never a target (the loop stops at
    // `phase_index = 1`), so it must be released here, after
    // `share_cuts_for_phase` writes the last batch of peer cuts.
    if (m_options_.low_memory_mode != LowMemoryMode::off
        && uid_of(src_phase_index) == uid_of(first_phase_index()))
    {
      std::vector<std::future<int>> rel_futures;
      rel_futures.reserve(static_cast<std::size_t>(num_scenes));
      for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
        auto fut = pool.submit(
            [this, si, src_phase_index]() -> int
            {
              planning_lp().system(si, src_phase_index).release_backend();
              return 0;
            },
            bwd_req);
        if (fut.has_value()) {
          rel_futures.push_back(std::move(*fut));
        }
      }
      for (auto& f : rel_futures) {
        f.get();
      }
    }

    SPDLOG_TRACE(
        "SDDP backward synchronized: phase {} cuts shared across {} scenes",
        src_phase_index,
        num_scenes);
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - bwd_start)
                      .count();

  // INFO summary: per-scene cut-receive counts.  Distinguishes
  // feasible (own cuts + peer broadcasts) from infeasible (peer
  // broadcasts only) so the user can verify the rollback
  // stall-stop guard's "global cuts grew" condition will be
  // satisfied for the infeasible scenes at the next iteration.
  if (m_options_.cut_sharing != CutSharingMode::none) {
    std::ptrdiff_t infeas_total = 0;
    std::ptrdiff_t infeas_count = 0;
    std::ptrdiff_t feas_total = 0;
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      const auto si_sz = static_cast<std::size_t>(si);
      if (scene_feasible[si] == 0U) {
        infeas_total += per_scene_cuts_received[si_sz];
        ++infeas_count;
      } else {
        feas_total += per_scene_cuts_received[si_sz];
      }
    }
    SPDLOG_INFO(
        "SDDP Backward [i{}]: cut sharing — feasible scenes received "
        "{} cut row(s), {} infeasible scene(s) received {} cut row(s) "
        "(rollback stall-stop guard reads global count next iter)",
        iteration_index,
        feas_total,
        infeas_count,
        infeas_total);
  }

  return out;
}

void SDDPMethod::compute_iteration_bounds(
    SDDPIterationResult& ir,
    std::span<const uint8_t> scene_feasible,
    std::span<const double> weights) const
{
  const auto num_scenes = planning_lp().simulation().scene_count();

  double weighted_upper = 0.0;
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    weighted_upper += weights[scene] * ir.scene_upper_bounds[scene];
  }
  ir.upper_bound = weighted_upper;

  ir.scene_lower_bounds.resize(num_scenes, 0.0);
  double weighted_lower = 0.0;
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    if (scene_feasible[scene] == 0U) {
      continue;
    }
    // Physical ($) units — must match the physical-space upper bound
    // aggregated in ``sddp_forward_pass`` (which uses
    // ``get_obj_value()`` after commit dd5c88ee).  Reading
    // LP-raw here put UB and LB in different unit spaces, producing
    // a gap = (UB − LB) / |UB| ≈ ``scale_objective − 1`` /
    // ``scale_objective`` that never closes regardless of cut
    // quality (juan/gtopt_iplp at scale_objective = 1000: a
    // permanent ≈ 0.999 gap).
    const double lb_si = planning_lp()
                             .system(scene, first_phase_index())
                             .linear_interface()
                             .get_obj_value();
    ir.scene_lower_bounds[scene] = lb_si;
    weighted_lower += weights[scene] * lb_si;
  }
  ir.lower_bound = weighted_lower;
}

void SDDPMethod::apply_cut_sharing_for_iteration(IterationIndex iteration_index)
{
  m_cut_store_.apply_cut_sharing_for_iteration(
      iteration_index, m_options_, planning_lp(), m_label_maker_);
}

void SDDPMethod::finalize_iteration_result(
    SDDPIterationResult& ir,
    IterationIndex iteration_index,
    const std::vector<SDDPIterationResult>& results)
{
  ir.gap = compute_convergence_gap(ir.upper_bound, ir.lower_bound);

  // Compute gap_change against the stationary look-back window so the
  // mid-iteration log below reports the same value carried forward to
  // the downstream "Iter [iN]: done" and stationary-convergence logs.
  // Keep the 1.0 sentinel when stationarity tracking is disabled or no
  // prior results exist — the same default used when constructing ir.
  if (m_options_.stationary_window > 0 && m_options_.stationary_tol > 0.0
      && !results.empty())
  {
    const auto window = static_cast<std::size_t>(m_options_.stationary_window);
    const auto lookback = std::min(window, results.size());
    const double old_gap = results[results.size() - lookback].gap;
    ir.gap_change =
        std::abs(ir.gap - old_gap) / std::max(1e-10, std::abs(old_gap));
  }

  // Primary convergence: the absolute gap (|UB-LB|/max(1,|UB|)) must
  // drop below convergence_tol.  Earlier we also accepted gap_change
  // (LB-drift stationary) as a secondary signal, but that masks
  // pathological cases where the LB stays frozen (e.g. Juan/gtopt_iplp
  // where α-bootstrap + loaded boundary cuts pin LB≈0 while UB floats
  // freely) by declaring "[CONVERGED]" at 99%+ absolute gap.  The
  // extended stationary-window / statistical convergence modes below
  // still consult gap_change, but only after min_iterations.
  //
  // Convergence requires `|gap| < tol`.  A small POSITIVE gap below
  // tolerance is the textbook convergence condition.  A small
  // NEGATIVE gap (e.g. `-1e-15`) when UB = LB exactly is harmless
  // floating-point precision — accept it.  Only a NEGATIVE gap whose
  // magnitude EXCEEDS the tolerance is an SDDP-theory violation (LB
  // > UB, cuts overshooting the optimum) — those are rejected and
  // logged as a warning.  Observed on juan/gtopt_iplp pre-fix: iter
  // 1 produced gap=-6.59 with LB ≈ 1.16B vs UB ≈ 153M; iter 2 sim
  // pass produced gap=-73.68 with LB ≈ 11.4B.  The run wrote
  // `[CONVERGED]` but the LB was wildly off.  Post-fix: those large
  // negative gaps log a warning and `ir.converged` stays false.
  // FP-noise band sourced from a single named constant in
  // <gtopt/sddp_types.hpp>.  Distinct from `convergence_tol`
  // (the user-facing knob, may be a negative sentinel like
  // `-1.0` to disable the primary gap test in favour of the
  // stationary criterion — see `test_sddp_method.cpp:1357`).
  const auto tol = m_options_.convergence_tol;
  const bool gap_in_range = ir.gap < tol && ir.gap > -kSddpGapFpEpsilon;
  if (ir.gap < -kSddpGapFpEpsilon) {
    SPDLOG_WARN(
        "SDDP Iter [i{}]: SIGNIFICANT negative gap = {:.6f} "
        "(UB={:.4f}, LB={:.4f}) — LB > UB violates SDDP theory; "
        "cuts likely overshoot the optimum. Refusing to declare "
        "[CONVERGED] until the bound asymmetry resolves.",
        iteration_index,
        ir.gap,
        ir.upper_bound,
        ir.lower_bound);
  }
  const bool past_min_iter =
      (iteration_index
       >= m_iteration_offset_ + IterationIndex {m_options_.min_iterations - 1});
  ir.converged = gap_in_range && past_min_iter;

  publish_live_metrics_(LiveMetrics {
      .iteration = iteration_index,
      .gap = ir.gap,
      .lower_bound = ir.lower_bound,
      .upper_bound = ir.upper_bound,
      .converged = ir.converged,
  });

  // Per-iteration end-of-iteration summary is emitted from
  // sddp_iteration.cpp (`SDDP Iter [i{}]: done in ...`).  Keep only the
  // TRACE breakdown here for very-verbose post-mortem analysis; the
  // INFO-level subset duplicated the iteration-summary line.
  SPDLOG_TRACE(
      "SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} gap_change={:.6f} "
      "cuts={} infeas_cuts={} fwd={:.3f}s bwd={:.3f}s total={:.3f}s{}",
      iteration_index,
      ir.lower_bound,
      ir.upper_bound,
      ir.gap,
      ir.gap_change,
      ir.cuts_added,
      ir.infeasible_cuts_added,
      ir.forward_pass_s,
      ir.backward_pass_s,
      ir.iteration_s,
      ir.converged ? " [CONVERGED]" : "");
}

void SDDPMethod::maybe_write_api_status(
    const std::string& status_file,
    const std::vector<SDDPIterationResult>& results,
    std::chrono::steady_clock::time_point solve_start,
    const SolverMonitor& monitor) const
{
  if (!m_options_.enable_api || status_file.empty()) {
    return;
  }
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - solve_start)
                             .count();
  // Query the actual solver identity from the first available LP
  std::string solver_id;
  if (!planning_lp().systems().empty()
      && !planning_lp().systems().front().empty())
  {
    solver_id =
        planning_lp().systems().front().front().linear_interface().solver_id();
  }

  // Read the 5 per-iteration metrics from a single coherent snapshot
  // so `iteration` can never come from a different step than `gap`.
  const auto metrics = live_metrics_();
  const SolverStatusSnapshot snapshot {
      .iteration_index = metrics->iteration,
      .gap = metrics->gap,
      .lower_bound = metrics->lower_bound,
      .upper_bound = metrics->upper_bound,
      .converged = metrics->converged,
      .max_iterations = m_options_.max_iterations,
      .min_iterations = m_options_.min_iterations,
      .current_pass = m_current_pass_.load(),
      .scenes_done = m_scenes_done_.load(),
      .solver = std::move(solver_id),
      .method = "sddp",
      .phase_grid = &m_phase_grid_,
  };
  write_solver_status(status_file, results, elapsed, snapshot, monitor);
}

void SDDPMethod::save_cuts_for_iteration(
    IterationIndex iteration_index, std::span<const uint8_t> scene_feasible)
{
  // Pass `m_pool_` so the per-scene cut writes can be dispatched in
  // parallel (sequential fallback when null — only happens before
  // `solve()` has constructed the pool).
  m_cut_store_.save_cuts_for_iteration(iteration_index,
                                       scene_feasible,
                                       m_options_,
                                       planning_lp(),
                                       m_label_maker_,
                                       m_scene_phase_states_,
                                       current_iteration(),
                                       m_pool_);
}

// ── solve() — now in sddp_iteration.cpp ─────────────────────────────────────

// ─── SDDPPlanningMethod ─────────────────────────────────────────────────────

SDDPPlanningMethod::SDDPPlanningMethod(SDDPOptions opts) noexcept
    : m_sddp_opts_(std::move(opts))
{
}

}  // namespace gtopt
