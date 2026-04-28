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

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

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
  cut.class_name = sddp_alpha_class_name;
  cut.constraint_name = sddp_scut_constraint_name;
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

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - fwd_start)
                      .count();

  // Post-pass cut count - pre-pass snapshot = fcuts installed this pass.
  // Non-negative because cuts are only added (never removed) during a
  // forward pass.
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

  const auto bwd_start = std::chrono::steady_clock::now();
  BackwardPassOutcome out;

  // Per-scene cumulative cut count for unique cut labels across phase steps
  std::vector<int> per_scene_cut_count(static_cast<std::size_t>(num_scenes), 0);

  // Apertures enabled unless explicitly set to empty array
  const bool use_apertures =
      !m_options_.apertures || !m_options_.apertures->empty();

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
          m_cut_store_.scene_cuts()[si].size();
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
      const auto& cuts = m_cut_store_.scene_cuts()[si];
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

    share_cuts_for_phase(src_phase_index, scene_cuts, iteration_index);

    SPDLOG_TRACE(
        "SDDP backward synchronized: phase {} cuts shared across {} scenes",
        phase,
        num_scenes);
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - bwd_start)
                      .count();
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
  const bool gap_ok = ir.gap < m_options_.convergence_tol;
  const bool past_min_iter =
      (iteration_index
       >= m_iteration_offset_ + IterationIndex {m_options_.min_iterations - 1});
  ir.converged = gap_ok && past_min_iter;

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
  m_cut_store_.save_cuts_for_iteration(iteration_index,
                                       scene_feasible,
                                       m_options_,
                                       planning_lp(),
                                       m_label_maker_,
                                       m_scene_phase_states_,
                                       current_iteration());
}

// ── solve() — now in sddp_iteration.cpp ─────────────────────────────────────

// ─── SDDPPlanningMethod ─────────────────────────────────────────────────────

SDDPPlanningMethod::SDDPPlanningMethod(SDDPOptions opts) noexcept
    : m_sddp_opts_(std::move(opts))
{
}

}  // namespace gtopt
