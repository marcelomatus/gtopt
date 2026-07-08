/**
 * @file      sddp_aperture.cpp
 * @brief     Aperture backward-pass logic for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the free functions declared in sddp_aperture.hpp.
 * Extracted from sddp_solver.cpp to reduce coupling and improve testability.
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
#include <functional>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── build_effective_apertures ──────────────────────────────────────────────

auto build_effective_apertures(std::span<const Aperture> aperture_defs,
                               std::span<const Uid> phase_apertures)
    -> std::vector<ApertureEntry>
{
  std::vector<ApertureEntry> result;

  if (phase_apertures.empty()) {
    // Use all aperture definitions (each with count = 1)
    for (const auto& ap : aperture_defs) {
      if (ap.is_active()) {
        result.push_back({
            .aperture = std::cref(ap),
            .count = 1,
        });
      }
    }
    return result;
  }

  // Count occurrences of each UID in the phase_apertures list.
  // Preserving order of first appearance keeps results deterministic.
  std::vector<std::pair<Uid, int>> uid_counts;
  for (const auto ap_uid : phase_apertures) {
    auto it = std::ranges::find_if(
        uid_counts, [ap_uid](const auto& p) { return p.first == ap_uid; });
    if (it != uid_counts.end()) {
      ++it->second;
    } else {
      uid_counts.emplace_back(ap_uid, 1);
    }
  }

  // Map each unique UID to its aperture definition
  for (const auto& [uid, cnt] : uid_counts) {
    auto it = std::ranges::find_if(aperture_defs,
                                   [uid](const auto& ap)
                                   { return ap.uid == uid && ap.is_active(); });
    if (it != aperture_defs.end()) {
      result.push_back({
          .aperture = std::cref(*it),
          .count = cnt,
      });
    }
  }

  return result;
}

// ─── select_apertures ───────────────────────────────────────────────────────

auto select_apertures(std::span<const Uid> phase_apertures,
                      const std::optional<int>& num_apertures,
                      ApertureSelectionMode mode) -> std::vector<Uid>
{
  // No truncation requested or empty input → return a full copy.
  if (!num_apertures || phase_apertures.empty()) {
    return {phase_apertures.begin(), phase_apertures.end()};
  }

  const auto total = phase_apertures.size();
  const auto n = static_cast<std::size_t>(std::max(0, *num_apertures));

  if (n == 0) {
    return {};
  }
  if (n >= total) {
    return {phase_apertures.begin(), phase_apertures.end()};
  }

  std::vector<Uid> out;
  out.reserve(n);

  switch (mode) {
    case ApertureSelectionMode::head:
      // First N (= N wettest when input is wettest-first).
      for (std::size_t i = 0; i < n; ++i) {
        out.push_back(phase_apertures[i]);
      }
      break;
    case ApertureSelectionMode::stride:
      // N entries evenly spaced across the full ordered list, ALWAYS
      // including the first (wettest) and last (driest) entry — the
      // (N-2) interior picks are evenly distributed between them.
      // Indices = ``i * (total - 1) / (n - 1)`` for i = 0..n-1:
      //   * i=0     → 0           (wettest)
      //   * i=n-1   → total - 1   (driest)
      //   * else    → interior, integer-truncated  (no duplicates because
      //                n < total guarantees the step (total-1)/(n-1) ≥ 1
      //                and ``i * (total - 1) / (n - 1)`` is monotone
      //                non-decreasing in i; strictly increasing when
      //                total ≥ n ≥ 2).
      // The single-N case (n == 1) returns the MIDDLE entry — the
      // median across the wet→dry spectrum is the most representative
      // single sample for `stride` semantics.  (Use `head` if you want
      // the wettest, `tail` for the driest.)
      if (n == 1) {
        out.push_back(phase_apertures[total / 2]);
      } else {
        for (std::size_t i = 0; i < n; ++i) {
          const auto idx = (i * (total - 1)) / (n - 1);
          out.push_back(phase_apertures[idx]);
        }
      }
      break;
    case ApertureSelectionMode::tail:
      // Last N (= N driest when input is wettest-first).  Mirror of
      // head: original wetness order is preserved within the picks
      // (wetter of the surviving N first, then drier ones).
      for (std::size_t i = total - n; i < total; ++i) {
        out.push_back(phase_apertures[i]);
      }
      break;
  }

  return out;
}

// ─── partition_apertures ────────────────────────────────────────────────────

// Split ONE phase's aperture list into contiguous chunks of `chunk_size`.
// `apertures` is always the `effective_apertures` of a single
// (scene, phase) — the caller (`solve_apertures_for_phase`) runs per
// (scene, phase) — so every returned chunk holds only same-(scene, phase)
// apertures.  The aperture-warm-start path depends on this: a chunk's
// apertures share one LP clone and differ only by `update_aperture`
// bound deltas (see the warm-start block in `solve_apertures_for_phase`).
auto partition_apertures(std::span<const ApertureEntry> apertures,
                         int chunk_size)
    -> std::vector<std::span<const ApertureEntry>>
{
  std::vector<std::span<const ApertureEntry>> chunks;
  const auto n = apertures.size();
  if (n == 0) {
    return chunks;
  }
  const auto step =
      (chunk_size > 0) ? static_cast<std::size_t>(chunk_size) : std::size_t {1};
  chunks.reserve((n + step - 1) / step);
  for (std::size_t off = 0; off < n; off += step) {
    const auto take = std::min(step, n - off);
    chunks.push_back(apertures.subspan(off, take));
  }
  return chunks;
}

// ─── build_synthetic_apertures ──────────────────────────────────────────────

auto build_synthetic_apertures(std::span<const ScenarioLP> all_scenarios,
                               std::ptrdiff_t n_apertures) -> Array<Aperture>
{
  const auto n =
      std::min(static_cast<std::size_t>(n_apertures), all_scenarios.size());
  Array<Aperture> synthetic;
  synthetic.reserve(n);
  const double prob = 1.0 / static_cast<double>(n);
  for (const auto sidx : iota_range<ScenarioIndex>(0, n)) {
    const auto scen_uid = Uid {all_scenarios[sidx].uid()};
    synthetic.push_back(Aperture {
        .uid = scen_uid,
        .source_scenario = scen_uid,
        .probability_factor = prob,
    });
  }
  return synthetic;
}

// ─── dual_shared_bound_correction ───────────────────────────────────────────

namespace
{

/// Bound magnitudes at or above this are treated as ±∞ sentinels
/// (backends encode "unbounded" as ±1e30-ish or ±DBL_MAX, and the
/// physical view can scale them further).  Any physical flow bound is
/// orders of magnitude below.
constexpr double kDualSharedBoundSentinel = 1.0e29;

/// A finite, sub-sentinel value the correction arithmetic can trust.
[[nodiscard]] constexpr bool ds_bound_usable(double v) noexcept
{
  return std::isfinite(v) && std::abs(v) < kDualSharedBoundSentinel;
}

}  // namespace

auto dual_shared_bound_correction(std::span<const double> rep_reduced_costs,
                                  std::span<const double> rep_low,
                                  std::span<const double> rep_upp,
                                  std::span<const double> ap_low,
                                  std::span<const double> ap_upp) noexcept
    -> std::optional<double>
{
  const auto n = rep_reduced_costs.size();
  if (rep_low.size() != n || rep_upp.size() != n || ap_low.size() != n
      || ap_upp.size() != n)
  {
    return std::nullopt;
  }

  double corr = 0.0;
  for (std::size_t j = 0; j < n; ++j) {
    const double d = rep_reduced_costs[j];
    if (!std::isfinite(d)) {
      return std::nullopt;
    }
    if (d > 0.0) {
      // λ_j = d⁺ prices the lower bound:  corr += d · (l_j^a − l_j^rep).
      if (rep_low[j] != ap_low[j]) {
        if (!ds_bound_usable(rep_low[j]) || !ds_bound_usable(ap_low[j])) {
          return std::nullopt;  // bound became (un)bounded — no finite delta
        }
        corr += d * (ap_low[j] - rep_low[j]);
      }
    } else if (d < 0.0) {
      // −μ_j = d⁻ prices the upper bound:  corr += d · (u_j^a − u_j^rep).
      if (rep_upp[j] != ap_upp[j]) {
        if (!ds_bound_usable(rep_upp[j]) || !ds_bound_usable(ap_upp[j])) {
          return std::nullopt;
        }
        corr += d * (ap_upp[j] - rep_upp[j]);
      }
    }
    // d == 0: λ = μ = 0 — the column contributes nothing regardless of
    // its bound delta (equal-sentinel bounds are skipped BEFORE any
    // subtraction above, so ∞ − ∞ can never be formed).
  }

  if (!std::isfinite(corr)) {
    return std::nullopt;
  }
  return corr;
}

// ─── solve_apertures_for_phase ──────────────────────────────────────────────

auto solve_apertures_for_phase(
    [[maybe_unused]] SceneIndex scene_index,
    [[maybe_unused]] PhaseIndex phase_index,
    const PhaseStateInfo& src_state,
    ColIndex src_alpha_col,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    std::span<const Uid> phase_apertures,
    int total_cuts,
    SystemLP& sys,
    const PhaseLP& phase_lp,
    const SolverOptions& opts,
    [[maybe_unused]] const LabelMaker& label_maker,
    [[maybe_unused]] const std::string& log_directory,
    SceneUid scene_uid_val,
    PhaseUid phase_uid_val,
    const ApertureChunkSubmitFunc& submit_fn,
    double aperture_timeout,
    [[maybe_unused]] bool save_aperture_lp,
    const ApertureDataCache& aperture_cache,
    IterationIndex iteration_index,
    double cut_coeff_eps,
    LpDebugWriter* lp_debug_writer,
    bool use_manual_clone,
    int chunk_size,
    SDDPWorkPool* pool_for_slot_release,
    std::span<const StateVarLink> cut_links,
    ApertureSolveMode aperture_solve_mode,
    const Basis* seed_basis,
    Basis* captured_basis_out,
    int aperture_screen_count) -> std::optional<SparseRow>
{
  const auto& phase_li = sys.linear_interface();

  // Dual-shared modes (Lemma AP2): solve the highest-weight
  // representative aperture exactly, synthesize every other aperture's
  // cut from its vertex duals (`dual_shared_bound_correction`), and —
  // under `screened` — re-solve the top-`aperture_screen_count`
  // synthesized cuts by |correction| on the resident basis.
  const bool ds_mode = aperture_solve_mode == ApertureSolveMode::dual_shared
      || aperture_solve_mode == ApertureSolveMode::screened;

  // Cross-iteration first-aperture warm start (aperture_seed_basis).  Only
  // composes with the vertex-cut modes — `reduced_cost` reads the cut from
  // the interior point and has no basis to seed or capture.
  const bool seed_enabled =
      aperture_solve_mode != ApertureSolveMode::reduced_cost;
  const Basis* const effective_seed =
      (seed_enabled && seed_basis != nullptr && !seed_basis->empty())
      ? seed_basis
      : nullptr;
  const bool capture_basis = seed_enabled && captured_basis_out != nullptr;
  // Written only by the first chunk's task, read only after every future has
  // joined (happens-before via the future) — no synchronisation needed.
  std::optional<Basis> captured_first_basis;

  // Apply aperture timeout to solver options if configured.
  // Crossover policy (see `ApertureSolveMode`):
  //   - `cold` / `warm`: honour the user's crossover setting.  Aperture
  //     cuts are built from reduced costs (`get_col_cost_raw`); the cold
  //     seed defaults to barrier + crossover so the cut comes from a
  //     vertex.  Simplex (the warm re-solve) always has a basis, so
  //     crossover is a no-op there.
  //   - `reduced_cost`: barrier; cut taken from reduced costs.  This used to
  //     request `crossover = false` ("no crossover", interior reduced costs)
  //     but on CPLEX the `BarCrossAlg = -1` was silently rejected, so the
  //     solve always crossed over (auto → vertex reduced costs).
  //     `automatic` reproduces that real, validated behaviour; their
  //     tolerance-level noise is filtered by `cut_coeff_eps`.  Switching to
  //     `CrossoverMode::none` now genuinely yields interior reduced costs,
  //     which changes the cut sequence and breaks cascade LB≤UB checks — so
  //     it is deferred to a deliberate, re-validated change, not flipped here.
  auto aperture_opts = opts;
  if (aperture_timeout > 0.0) {
    aperture_opts.time_limit = aperture_timeout;
  }
  if (aperture_solve_mode == ApertureSolveMode::reduced_cost) {
    aperture_opts.algorithm = LPAlgo::barrier;
    aperture_opts.crossover = CrossoverMode::automatic;
  }

  // Build the effective aperture list for this phase
  auto effective_apertures =
      build_effective_apertures(aperture_defs, phase_apertures);

  if (effective_apertures.empty()) {
    return std::nullopt;
  }

  // ── Submit all aperture tasks to the pool ────────────────────────────
  //
  // Each task is a complete unit: clone → update (apply aperture bounds)
  // → solve (cold barrier, no warm-start — see the solve site below) →
  // build cut.  Tasks are independent (separate LP clones) and execute
  // concurrently in the SDDP work pool.

  const auto phase_start = std::chrono::steady_clock::now();
  const auto caller_tid = std::this_thread::get_id();

  SPDLOG_TRACE(
      "SDDP Aperture [i{} s{} p{}]: starting {} aperture(s) [thread {}]",
      gtopt::uid_of(iteration_index),
      scene_uid_val,
      phase_uid_val,
      effective_apertures.size(),
      std::hash<std::thread::id> {}(caller_tid) % 10000);

  // ── Per-task cloning from a shared source LP ─────────────────────────
  //
  // Two clone routes coexist, selected by `use_manual_clone`:
  //
  //   * Native route (`use_manual_clone == false`, legacy default).
  //     Each aperture task calls `phase_li.clone(CloneKind::shallow)`,
  //     which goes through the backend's native `clone()` (e.g.
  //     `CPXcloneprob`).  This must be globally serialised under
  //     `s_global_clone_mutex` because the underlying solver has
  //     process-wide internal state (environment, allocator mutex,
  //     licence manager) that is not reentrant across threads during
  //     `CPXcloneprob`.  On the juan/gtopt_iplp compress run we
  //     observed 3 threads GPF'ing at the same IP inside the solver's
  //     shared lib, which is the fingerprint of concurrent cloneprob
  //     without a process-global lock (commit `1d7a05c1`).  The
  //     previous per-scene mutex was insufficient: it serialised
  //     within one scene's apertures but allowed 16 cross-scene
  //     clones to race.
  //
  //   * Manual route (`use_manual_clone == true`).  Each aperture
  //     task calls `phase_li.clone_from_flat(CloneKind::shallow)`,
  //     which builds the clone via `CPXcreateprob` + `CPXaddrows`
  //     into a freshly-opened CPLEX env.  Those calls are env-local
  //     and have no process-global side effects, so the manual
  //     route does NOT acquire the global mutex — 80 aperture
  //     clones can be built in parallel.  Pre-condition: the
  //     source `phase_li` must hold a decompressed
  //     `FlatLinearProblem` snapshot (satisfied during the aperture
  //     window by the `DecompressionGuard` at
  //     `sddp_aperture_pass.cpp:390, 579`).  See
  //     `LinearInterface::clone_from_flat` for full contract.
  static std::mutex s_global_clone_mutex;
  auto* clone_mutex = &s_global_clone_mutex;

  // ── Pre-filter: drop apertures with no resolvable source ────────────
  //
  // An aperture whose ``source_scenario`` is neither in the forward
  // ``all_scenarios`` set nor backed by an ``aperture_cache`` cannot
  // be solved.  Filtering happens here (not inside the chunk task)
  // so the chunk-shape fed to ``partition_apertures`` already
  // excludes unsolvable entries — the inner serial loop never has
  // to early-out, and the per-chunk UID memo stays consistent.
  struct PreparedAperture
  {
    std::reference_wrapper<const Aperture> aperture;
    int count {};
    std::ptrdiff_t scenario_idx {-1};  // -1 = use aperture_cache fallback
  };
  std::vector<PreparedAperture> prepared;
  prepared.reserve(effective_apertures.size());
  int n_skipped = 0;
  for (const auto& [ap_ref, ap_count] : effective_apertures) {
    const auto& aperture = ap_ref.get();
    const ApertureUid ap_uid = make_uid<Scenario>(aperture.uid);
    const auto scen_it = std::ranges::find_if(
        all_scenarios,
        [&aperture](const auto& scen)
        { return Uid {scen.uid()} == aperture.source_scenario; });
    if (scen_it == all_scenarios.end() && aperture_cache.empty()) {
      spdlog::info(
          "SDDP Aperture [i{} s{} p{} a{}]: source_scenario {} not found and "
          "no aperture cache, skipping",
          gtopt::uid_of(iteration_index),
          scene_uid_val,
          phase_uid_val,
          ap_uid,
          aperture.source_scenario);
      ++n_skipped;
      continue;
    }
    const auto idx = scen_it == all_scenarios.end()
        ? std::ptrdiff_t {-1}
        : std::distance(all_scenarios.begin(), scen_it);
    prepared.push_back(
        {.aperture = ap_ref, .count = ap_count, .scenario_idx = idx});
  }

  // ── Dual-shared: rotate the representative to the front ────────────
  //
  // The representative anchors the shared dual point, so pick the
  // highest-weight (count × probability_factor) aperture — it carries
  // the largest share of the expected cut, and every synthesized
  // intercept is exact at zero delta from it.  `std::ranges::rotate`
  // preserves the relative order of the rest (memo/duplicate semantics
  // unchanged).
  if (ds_mode && prepared.size() > 1) {
    const auto ds_weight = [](const PreparedAperture& p) noexcept
    {
      const double pf = p.aperture.get().probability_factor.value_or(1.0);
      return static_cast<double>(p.count) * (pf > 0.0 ? pf : 1.0);
    };
    const auto rep_it = std::ranges::max_element(prepared, {}, ds_weight);
    std::ranges::rotate(prepared, rep_it);
  }

  // ── Wrap prepared entries as ApertureEntry spans for partitioning ──
  //
  // ``partition_apertures`` operates on ``ApertureEntry`` (which holds
  // the same {aperture, count} pair, minus the resolved scenario_idx).
  // Build a parallel ``ApertureEntry`` view so the partition helper
  // can be reused unchanged; the chunk task indexes into ``prepared``
  // for the resolved scenario_idx.
  std::vector<ApertureEntry> prepared_view;
  prepared_view.reserve(prepared.size());
  for (const auto& p : prepared) {
    prepared_view.push_back({.aperture = p.aperture, .count = p.count});
  }
  // Dual-shared forces a SINGLE chunk: the synthesis needs the
  // representative's snapshot resident in the same task as every
  // non-representative aperture (and there is nothing to fan out — no
  // per-aperture solves remain).
  const int effective_chunk_size = (ds_mode && !prepared_view.empty())
      ? static_cast<int>(prepared_view.size())
      : chunk_size;
  const auto chunks = partition_apertures(prepared_view, effective_chunk_size);

  // ── Submit one task per chunk ──────────────────────────────────────
  //
  // Each chunk task body:
  //   1. Clones the phase LP **once** (manual `clone_from_flat` w/
  //      replay if available, else mutex-serialised native `clone()`).
  //   2. Iterates its assigned apertures serially, for each:
  //      a. Per-chunk UID memo — adjacent (or in-chunk) duplicates
  //         reuse the previous cut + status, replacing only the
  //         per-instance weight.  Saves the full visitor traversal
  //         and the LP resolve.
  //      b. Always runs the visitor (no `is_base_scenario` short-
  //         circuit — in chunked mode the clone already carries the
  //         previous aperture's bounds, so the base-scenario aperture
  //         must re-overwrite to base values explicitly).  The
  //         per-element ``update_aperture`` overwrites its LP entries
  //         (flow col bounds; AR-row RHS under ``Flow.inflow_model``)
  //         for every (stage, block) its data covers, so apertures
  //         with the same coverage fully overwrite each other — no
  //         snapshot/restore needed.  Precondition: every aperture's
  //         data covers the same (stage, block) set; a sparser
  //         aperture inherits the previous aperture's values.
  //      c. Solves; build cut from reduced costs.
  //   3. Returns one ``ApertureCutResult`` per inner aperture in
  //      input order.
  std::vector<std::future<ApertureChunkResult>> futures;
  futures.reserve(chunks.size());
  for (std::size_t chunk_idx = 0; chunk_idx < chunks.size(); ++chunk_idx) {
    const auto chunk = chunks[chunk_idx];
    // Only the first chunk (which owns the cell's first aperture) captures
    // the basis to persist for next iteration's seed.
    const bool do_capture = capture_basis && chunk_idx == 0;
    futures.push_back(submit_fn(
        [&, chunk, clone_mutex, use_manual_clone, do_capture]()
            -> ApertureChunkResult
        {
          const auto chunk_start = std::chrono::steady_clock::now();
          const auto task_tid = std::this_thread::get_id();

          // ── Single LP clone per chunk ────────────────────────────
          //
          // Manual route (parallel-safe, no global mutex): builds the
          // clone via `CPXcreateprob` + `CPXaddrows` on a fresh env,
          // then replays `m_replay_.active_cuts()` so cuts on α_p
          // installed by the previous backward iteration (phase p+1)
          // are visible to every aperture in this chunk.
          // Native route: `CPXcloneprob` copies the live backend
          // (cuts included) under the process-global clone mutex.
          LinearInterface clone = [&]
          {
            if (use_manual_clone && phase_li.has_snapshot_data()) {
              return phase_li.clone_from_flat(
                  LinearInterface::CloneKind::shallow);
            }
            const std::scoped_lock lock(*clone_mutex);
            return phase_li.clone(LinearInterface::CloneKind::shallow);
          }();

          ApertureChunkResult results;
          results.reserve(chunk.size());

          // Per-chunk UID memo (linear search — chunk_size is small,
          // K ≈ 4..16 in production).  Maps an already-solved
          // aperture UID to its index inside `results` so duplicates
          // copy the cut + feasibility but use this instance's
          // weight = ap_count × probability_factor.
          struct MemoEntry
          {
            ApertureUid uid;
            std::size_t idx;
          };
          std::vector<MemoEntry> seen;
          seen.reserve(chunk.size());

          // ── Warm-start state (aperture_solve_mode == warm) ──────────
          // The FIRST solved aperture in this chunk runs cold (barrier +
          // crossover, leaving an optimal basis on the shared clone);
          // every subsequent aperture re-optimizes that resident basis
          // with a warm simplex solve.  `update_aperture` deltas
          // (column bounds / equality-row RHS) keep the basis valid —
          // no basis is saved/restored, the clone's resident basis is
          // reused in place.
          //
          // INVARIANT this relies on: every aperture in a chunk belongs
          // to the SAME (scene, phase).  This holds by construction —
          // `solve_apertures_for_phase` is called once per
          // (scene_index, phase_index); `partition_apertures` only splits
          // that single phase's `effective_apertures` into contiguous
          // subspans; the clone is that phase's LP; and the aperture
          // system (hence the LP *matrix* and replayed cuts) is resolved
          // per (scene, phase), not per aperture.  So all apertures in a
          // chunk share an identical matrix and differ only in the
          // flow-col bounds / equality-row RHS `update_aperture`
          // rewrites — exactly the condition under which the resident
          // basis stays valid.  A
          // future refactor that batched apertures across phases/scenes
          // into one chunk would break this and must NOT reuse the clone.
          SolverOptions warm_opts = aperture_opts;
          warm_opts.advanced_basis = true;
          // Apertures differ only in flow-col BOUNDS and equality-row
          // RHS (update_aperture; the RHS case is the AR inflow model
          // and the profile elements) — both delta kinds preserve dual
          // feasibility of the resident basis, so dual simplex resumes
          // from it efficiently, whereas primal would restart from a
          // primal-infeasible point.  Use dual for the warm re-solve.
          warm_opts.algorithm = LPAlgo::dual;
          // CRITICAL: disable presolve for the warm solve.  CPLEX presolve
          // rebuilds the model and DISCARDS the resident/seeded basis, so an
          // "advanced basis" warm start with presolve on silently degrades to
          // a cold solve — this is why the warm aperture mode measured as "not
          // faster" and was left disabled.  With presolve off the dual simplex
          // actually resumes from the basis in a few pivots.
          warm_opts.presolve = false;

          // Coordinated-seed cold anchor: when the seed scheme is active, the
          // FIRST aperture (no resident basis — iteration 1) solves barrier +
          // primal-crossover instead of the .prm's cold method.  This lands on
          // a deterministic vertex basis fast (vs a ~13k-iteration cold dual
          // simplex), and that canonical basis anchors every subsequent dual
          // warm start (the within-chunk chain and next iterations' seeds).
          // `force_barrier_crossover` overrides any .prm-pinned LPMethod.
          SolverOptions cold_opts = aperture_opts;
          if (capture_basis) {
            cold_opts.algorithm = LPAlgo::barrier;
            cold_opts.crossover = CrossoverMode::primal;
            cold_opts.force_barrier_crossover = true;
          }
          bool clone_has_basis = false;

          // Marks the chunk's first actual solve.  The cross-iteration seed
          // (`set_basis`) and the cold anchor are applied to this first solve
          // ONLY — see the in-loop block below.  `set_basis` is deliberately
          // deferred to AFTER `update_aperture` + `relax_integers` so that
          // neither the inflow-bound write nor a MILP→LP `CPXchgprobtype`
          // (which discards a basis) can wipe the copied seed before it is
          // used.  On a pure-LP `relax_integers` is a no-op, but doing it in
          // this order keeps the seed robust when the clone carries integers.
          bool first_solve_pending = true;

          // Instrumentation: split solve wall-time into the cold seed and
          // the warm re-solves so the speedup is observable in the logs.
          double cold_solve_s = 0.0;
          double warm_solve_s = 0.0;
          int n_cold_solves = 0;
          int n_warm_solves = 0;

          // ── Dual-shared state (modes dual_shared / screened only) ──
          //
          // The first feasible EXACT solve in the chunk becomes the
          // representative: its physical reduced costs, physical column
          // bounds (as solved) and built cut are snapshotted; every
          // later aperture whose deltas are clean (finite, sub-sentinel,
          // column-bound-only) gets a synthesized cut = representative
          // cut with `lowb += corr` (Lemma AP2).
          struct DsRepresentative
          {
            std::vector<double> rc;  ///< physical reduced costs d_j
            std::vector<double> low;  ///< physical col lower bounds
            std::vector<double> upp;  ///< physical col upper bounds
            SparseRow cut;  ///< representative's built cut
          };
          std::optional<DsRepresentative> ds_rep;
          // Synthesized-cut ledger for the `screened` re-solve pass.
          struct DsSynthEntry
          {
            std::size_t result_idx;  ///< index into `results`
            std::size_t prep_idx;  ///< index into `prepared`
            double corr;  ///< intercept correction (for |corr| ranking)
          };
          std::vector<DsSynthEntry> ds_synth;
          // Sticky sharing gate: a row-touching aperture update (a
          // profile element or an AR-mode FlowLP receiving an aperture
          // value — rewrites row RHS / coefficients, breaking the
          // shared-dual precondition of identical (A, b, c))
          // permanently disables synthesis for the remainder of the
          // chunk; every later aperture exact-solves, matching the
          // plain chunked semantics.
          bool ds_sharing_allowed = true;
          int ds_n_delta_fallback = 0;
          int ds_n_screen_resolved = 0;

          // ── Aperture data application (extracted from the loop) ────
          //
          // Applies `prep`'s stochastic data onto the shared clone via
          // the `HasUpdateAperture` visitor.  Returns true when any
          // NON-column-bound LP write may have occurred: the profile
          // elements rewrite a row RHS or matrix coefficient, and an
          // AR-mode `FlowLP` (`Flow.inflow_model`) rewrites the AR
          // equality-row RHS (flow_lp.cpp `update_aperture`); a write
          // happens exactly when the element's value_fn yields a
          // value.  Only a plain (non-AR) `FlowLP` writes column
          // bounds exclusively.  The detection wrap is applied only in
          // dual-shared modes so every other mode's call chain is
          // byte-identical.
          //
          // `sys.collections()` is populated on the main thread BEFORE
          // chunk tasks are dispatched.  Read-only access from many
          // threads is safe; mutating it here would race.
          auto apply_aperture_update = [&](const PreparedAperture& prep) -> bool
          {
            const auto& aperture = prep.aperture.get();
            const bool has_scen = prep.scenario_idx >= 0;
            const ScenarioLP* scen_ptr = has_scen
                ? &all_scenarios[static_cast<std::size_t>(prep.scenario_idx)]
                : nullptr;
            bool non_bound_write = false;

            auto visitor = [&](auto& e) -> bool
            {
              using E = std::remove_cvref_t<decltype(e)>;
              if constexpr (HasUpdateAperture<E>) {
                for (const auto& stage : phase_lp.stages()) {
                  ApertureValueFn value_fn;
                  if (scen_ptr != nullptr) {
                    const auto& ap_scen = *scen_ptr;
                    value_fn = [&e, &ap_scen](
                                   StageUid st,
                                   BlockUid bl) -> std::optional<double>
                    { return e.aperture_value(ap_scen.uid(), st, bl); };
                  } else {
                    const ScenarioUid ap_uid_val =
                        make_uid<Scenario>(aperture.source_scenario);
                    value_fn = [&e, &aperture_cache, ap_uid_val](
                                   StageUid st,
                                   BlockUid bl) -> std::optional<double>
                    {
                      return aperture_cache.lookup(
                          E::Element::class_name.full_name(),
                          e.id().second,
                          ap_uid_val,
                          st,
                          bl);
                    };
                  }
                  // Row-touching updater: record whether it actually
                  // receives a value (dual-shared precondition guard).
                  // Every non-FlowLP updater is row-touching; FlowLP is
                  // row-touching exactly when it carries AR(1) rows for
                  // this (scenario, stage) — the AR hydrology enters
                  // through the AR-row RHS, which Lemma AP2's
                  // column-bound correction cannot price (ledger F12).
                  // Conservative for a mixed AR/legacy stage: blocks
                  // without AR rows still take the bound pin, but any
                  // delivered value trips the gate → exact solves.
                  const bool row_touching = [&]
                  {
                    if constexpr (std::same_as<E, FlowLP>) {
                      return e.has_ar_rows(base_scenario.uid(), stage.uid());
                    } else {
                      return true;
                    }
                  }();
                  if (ds_mode && row_touching) {
                    value_fn = [inner = std::move(value_fn), &non_bound_write](
                                   StageUid st,
                                   BlockUid bl) -> std::optional<double>
                    {
                      auto v = inner(st, bl);
                      non_bound_write = non_bound_write || v.has_value();
                      return v;
                    };
                  }
                  [[maybe_unused]] const auto ok =
                      e.update_aperture(clone, base_scenario, value_fn, stage);
                }
              }
              return true;
            };
            visit_elements(sys.collections(), visitor);
            return non_bound_write;
          };

          for (const auto& entry : chunk) {
            // Each chunk_view entry indexes into `prepared` at the
            // same offset of its own contiguous range.  Because
            // `partition_apertures` returns contiguous subspans of
            // `prepared_view`, address-distance gives us the original
            // index into `prepared`.
            const auto prep_idx =
                static_cast<std::size_t>(&entry - prepared_view.data());
            const auto& prep = prepared[prep_idx];
            const auto& aperture = prep.aperture.get();
            const ApertureUid ap_uid = make_uid<Scenario>(aperture.uid);
            const double pf = aperture.probability_factor.value_or(1.0);
            if (pf <= 0.0) {
              SPDLOG_WARN(
                  "SDDP Aperture [i{} s{} p{} a{}]: non-positive "
                  "probability_factor {:.6f}, using 1.0 as fallback",
                  gtopt::uid_of(iteration_index),
                  scene_uid_val,
                  phase_uid_val,
                  ap_uid,
                  pf);
            }
            const double effective_pf = pf > 0.0 ? pf : 1.0;
            const double weight =
                static_cast<double>(prep.count) * effective_pf;

            // Per-chunk UID memo hit.
            if (auto it = std::ranges::find_if(
                    seen, [ap_uid](const auto& e) { return e.uid == ap_uid; });
                it != seen.end())
            {
              ApertureCutResult dup = results[it->idx];
              dup.weight = weight;  // use this instance's weight
              results.push_back(std::move(dup));
              continue;
            }

            const auto ap_start = std::chrono::steady_clock::now();

            // Always run the update (no is_base_scenario short-
            // circuit): in chunked mode the clone may carry the
            // previous aperture's bounds, so the base-scenario
            // aperture must re-overwrite to base values explicitly.
            // Per-element `update_aperture` overwrites its entries
            // (flow col bounds; AR-row RHS under `Flow.inflow_model`)
            // densely over its data coverage → full overwrite → no
            // snapshot/restore needed (same-coverage precondition,
            // see the chunk-task comment above).
            const bool non_bound_write = apply_aperture_update(prep);
            if (ds_mode && non_bound_write && ds_sharing_allowed) {
              ds_sharing_allowed = false;
              spdlog::info(
                  "SDDP Aperture [i{} s{} p{} a{}]: row-touching aperture "
                  "update (profile element or AR-mode flow) — dual-shared "
                  "synthesis disabled for this chunk, falling back to exact "
                  "solves",
                  gtopt::uid_of(iteration_index),
                  scene_uid_val,
                  phase_uid_val,
                  ap_uid);
            }

            // Configure solver log file for this aperture solve.
            const auto log_mode =
                aperture_opts.log_mode.value_or(SolverLogMode::nolog);
            if (log_mode == SolverLogMode::detailed && !log_directory.empty()) {
              clone.set_log_file((std::filesystem::path(log_directory)
                                  / as_label(clone.solver_name(),
                                             scene_uid_val,
                                             phase_uid_val,
                                             ap_uid))
                                     .string());
            }

            // lp_debug for apertures: dump the clone's LP (post-bound-
            // apply, pre-solve).  Caller applies the filter window.
            if (lp_debug_writer != nullptr && lp_debug_writer->is_active()) {
              const auto dbg_stem =
                  (std::filesystem::path(log_directory)
                   / std::format(sddp_file::debug_aperture_lp_fmt,
                                 scene_uid_val,
                                 phase_uid_val,
                                 ap_uid,
                                 iteration_index))
                      .string();
              lp_debug_writer->write(clone, dbg_stem);
            }

            // ── Dual-shared synthesis (no solve) ─────────────────────
            //
            // With a feasible representative resident, synthesize this
            // aperture's cut from the shared vertex duals: same slope,
            // intercept moved by the bound-delta correction (Lemma AP2,
            // `docs/formulation/sddp-cut-validity.md` §6).  On a dirty
            // delta (non-finite / ±sentinel — the helper returns
            // nullopt) fall through to the exact solve below.
            if (ds_mode && ds_rep.has_value() && ds_sharing_allowed) {
              const auto n_cols = ds_rep->rc.size();
              const auto low_view = clone.get_col_low();
              const auto upp_view = clone.get_col_upp();
              std::vector<double> ap_low;
              std::vector<double> ap_upp;
              ap_low.reserve(n_cols);
              ap_upp.reserve(n_cols);
              for (std::size_t j = 0; j < n_cols; ++j) {
                ap_low.push_back(low_view[j]);
                ap_upp.push_back(upp_view[j]);
              }
              const auto corr = dual_shared_bound_correction(
                  ds_rep->rc, ds_rep->low, ds_rep->upp, ap_low, ap_upp);
              if (corr.has_value()) {
                SparseRow cut = ds_rep->cut;
                cut.lowb += *corr;
                sddp_aperture_cut_tag.apply_to(cut);
                cut.variable_uid = phase_uid_val;
                cut.context = make_aperture_context(
                    scene_uid_val, phase_uid_val, ap_uid, total_cuts);
                if (spdlog::should_log(spdlog::level::trace)) {
                  spdlog::trace(
                      "SDDP Aperture [i{} s{} p{} a{}]: dual-shared cut "
                      "(corr={:.6g}) [thread {}]",
                      gtopt::uid_of(iteration_index),
                      scene_uid_val,
                      phase_uid_val,
                      ap_uid,
                      *corr,
                      std::hash<std::thread::id> {}(task_tid) % 10000);
                }
                results.push_back(ApertureCutResult {
                    .ap_uid = ap_uid,
                    .weight = weight,
                    .feasible = true,
                    .status = 0,
                    .cut = std::move(cut),
                });
                ds_synth.push_back({
                    .result_idx = results.size() - 1,
                    .prep_idx = prep_idx,
                    .corr = *corr,
                });
                // Memo: in-chunk duplicates copy THIS synthesized cut.
                // If the original is later screen-upgraded to an exact
                // cut, the duplicate keeps the shared version — both
                // are valid underestimators of the same Q_a, so mixing
                // them is sound (the duplicate is merely looser).
                seen.push_back({.uid = ap_uid, .idx = results.size() - 1});
                continue;
              }
              ++ds_n_delta_fallback;
              if (spdlog::should_log(spdlog::level::trace)) {
                spdlog::trace(
                    "SDDP Aperture [i{} s{} p{} a{}]: dual-shared delta "
                    "unusable (non-finite/sentinel), exact re-solve",
                    gtopt::uid_of(iteration_index),
                    scene_uid_val,
                    phase_uid_val,
                    ap_uid);
              }
            }

            // Solve.  Warm-starting is wired AND is the effective
            // default: `sddp_aperture_solve_mode()` resolves unset to
            // `warm` (planning_options_lp.hpp) — the within-chunk chain
            // above (dual simplex + advanced_basis + presolve OFF)
            // re-optimizes the resident basis in a few pivots, because
            // the `update_aperture` deltas (bounds / equality-row RHS)
            // preserve dual feasibility.  Under the default `basis_cross_mode =
            // full_cross` the chunk's FIRST aperture is additionally
            // dual-seeded from the forward-pass basis, so on
            // basis-capable backends (CPLEX/HiGHS) every aperture solve
            // is a warm dual re-solve.  plp2gtopt emits both settings
            // explicitly (`warm` + `full_cross`).  `cold` (barrier +
            // crossover per aperture) is the opt-out.
            //
            // Backend caveat (cuOpt ≥ 26.08, commit 52d3e0e9): the cuOpt
            // plugin has NO warm-start path at all — set_basis /
            // solution hints are documented no-ops and every resolve()
            // rebuilds cold — so `warm` mode silently degrades to cold
            // there.  Additionally `LPAlgo::barrier` maps to
            // CUOPT_METHOD_CONCURRENT (cuDSS barrier wedge on WSL2), so
            // the winning method may be PDLP, whose duals are ε-optimal
            // rather than vertex duals (crossover guarantees do not
            // apply; see the cut-validity ledger).  On such backends the
            // per-chunk clone saves only LP reconstruction, and skipping
            // re-solves entirely (Infanger–Morton dual cut sharing over
            // the bound deltas) is the remaining lever.
            //
            // Relax every integer column on the clone to continuous
            // before solving.  Benders / SDDP subproblem clones must
            // be convex LPs: cuts built from a MIP's dual / reduced
            // costs are not valid value-function supports for the
            // master.  Routed through the backend's bulk
            // `relax_all_integers()` (CPLEX `CPXchgprobtype`, HiGHS
            // `clearIntegrality`, Gurobi/OSI/MindOpt bulk attr setters)
            // so this is a single native call per clone, not a
            // per-column loop.  Idempotent on pure-LP problems and on
            // chunk-shared clones (cheap noop after the first
            // aperture).
            clone.relax_integers();

            // Cross-iteration seed (aperture_seed_basis) — applied to the
            // chunk's FIRST solve only, and crucially AFTER `relax_integers`
            // so a MILP→LP `CPXchgprobtype` cannot discard the copied basis
            // (and after `update_aperture`, since bound writes preserve a
            // basis but must not race the copy).  Installs the previous
            // iteration's first-aperture basis (reconciled to the current row
            // count — appended cut rows become basic slack) so this solve is a
            // dual warm start.  If `set_basis` is rejected (basis-less backend
            // / unbridgeable mismatch) the solve falls back to the cold anchor
            // and self-heals on capture below.
            bool seeded_first = false;
            if (first_solve_pending && effective_seed != nullptr
                && clone.set_basis(*effective_seed))
            {
              clone_has_basis = true;
              seeded_first = true;
            }

            // Warm-start when the clone holds a basis: the within-chunk chain
            // (`warm` mode), the cross-iteration seed just installed, or a
            // dual-shared fallback exact solve (bound/RHS deltas off the
            // representative's resident basis — same shape as `warm`).
            const bool use_warm = clone_has_basis
                && (aperture_solve_mode == ApertureSolveMode::warm
                    || seeded_first || ds_mode);
            // Cold anchor: the chunk's first (no-basis) solve under the
            // coordinated seed scheme uses barrier+crossover for a canonical
            // vertex; else the plain cold method (the .prm's, e.g. dual).
            const bool cold_anchor =
                !use_warm && capture_basis && first_solve_pending;
            first_solve_pending = false;
            const SolverOptions* solve_opts_ptr = &aperture_opts;
            if (use_warm) {
              solve_opts_ptr = &warm_opts;
            } else if (cold_anchor) {
              solve_opts_ptr = &cold_opts;
            }
            const SolverOptions& solve_opts = *solve_opts_ptr;
            const auto solve_t0 = std::chrono::steady_clock::now();
            [[maybe_unused]] auto solve_result = clone.resolve(solve_opts);
            const auto solve_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now()
                                              - solve_t0)
                    .count();
            const bool feasible = clone.is_optimal();
            if (use_warm) {
              warm_solve_s += solve_s;
              ++n_warm_solves;
            } else {
              cold_solve_s += solve_s;
              ++n_cold_solves;
            }
            // A successful solve leaves an optimal basis resident on the
            // shared clone (barrier+crossover on the cold seed, simplex on
            // the warm re-solves), so the next aperture can warm-start.
            if (feasible) {
              clone_has_basis = true;
            }
            // Capture THIS cell's first-aperture basis (first chunk only)
            // for next iteration's seed.  Right after the first solve, before
            // `update_aperture` overwrites the bounds for the next aperture.
            // `(n_cold + n_warm) == 1` is the first ACTUAL solve in the chunk;
            // chunk 0's first entry can never be a duplicate-memo skip (the
            // memo is empty on entry), and `partition_apertures` keeps chunk 0
            // first in wettest→driest order, so this is the cell's
            // representative (wettest) aperture by construction.
            if (do_capture && feasible
                && (n_cold_solves + n_warm_solves) == 1) {
              captured_first_basis = clone.get_basis();
            }
            if (!feasible) {
              const auto status = clone.get_status();
              if (spdlog::should_log(spdlog::level::trace)) {
                const auto ap_s =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - ap_start)
                        .count();
                spdlog::trace(
                    "SDDP Aperture [i{} s{} p{} a{}]: infeasible ({:.3f}s) "
                    "[thread {}]",
                    gtopt::uid_of(iteration_index),
                    scene_uid_val,
                    phase_uid_val,
                    ap_uid,
                    ap_s,
                    std::hash<std::thread::id> {}(task_tid) % 10000);
              }
              results.push_back(ApertureCutResult {
                  .ap_uid = ap_uid,
                  .weight = weight,
                  .feasible = false,
                  .status = status,
              });
              // Memo: future duplicates inside the chunk inherit the
              // infeasible status (and don't pay the resolve cost).
              seen.push_back({.uid = ap_uid, .idx = results.size() - 1});
              continue;
            }

            // Aperture-system path passes hybrid `cut_links` whose
            // `dependent_col` indexes this aperture `clone`; the regular
            // path uses the forward source phase's outgoing links.
            const std::span<const StateVarLink> links_for_cut =
                cut_links.empty()
                ? std::span<const StateVarLink>(src_state.outgoing_links)
                : cut_links;
            auto cut = build_benders_cut_physical(src_alpha_col,
                                                  links_for_cut,
                                                  clone,
                                                  clone.get_obj_value(),
                                                  cut_coeff_eps);
            sddp_aperture_cut_tag.apply_to(cut);
            cut.variable_uid = phase_uid_val;
            cut.context = make_aperture_context(
                scene_uid_val, phase_uid_val, ap_uid, total_cuts);

            // Dual-shared representative snapshot: the first feasible
            // exact solve anchors the shared dual point.  Captured
            // BEFORE the next aperture's `update_aperture` rewrites the
            // clone bounds — rc/low/upp are the state AT this optimum.
            if (ds_mode && !ds_rep.has_value()) {
              const auto n_cols = static_cast<std::size_t>(clone.get_numcols());
              const auto rc_view = clone.get_col_cost();
              const auto low_view = clone.get_col_low();
              const auto upp_view = clone.get_col_upp();
              DsRepresentative rep;
              rep.rc.reserve(n_cols);
              rep.low.reserve(n_cols);
              rep.upp.reserve(n_cols);
              for (std::size_t j = 0; j < n_cols; ++j) {
                rep.rc.push_back(rc_view[j]);
                rep.low.push_back(low_view[j]);
                rep.upp.push_back(upp_view[j]);
              }
              rep.cut = cut;  // copy — `cut` is moved into `results` below
              ds_rep = std::move(rep);
            }

            if (spdlog::should_log(spdlog::level::trace)) {
              const auto ap_s = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - ap_start)
                                    .count();
              spdlog::trace(
                  "SDDP Aperture [i{} s{} p{} a{}]: solved ({:.3f}s) "
                  "[thread {}]",
                  gtopt::uid_of(iteration_index),
                  scene_uid_val,
                  phase_uid_val,
                  ap_uid,
                  ap_s,
                  std::hash<std::thread::id> {}(task_tid) % 10000);
            }

            results.push_back(ApertureCutResult {
                .ap_uid = ap_uid,
                .weight = weight,
                .feasible = true,
                .status = 0,
                .cut = std::move(cut),
            });
            seen.push_back({.uid = ap_uid, .idx = results.size() - 1});
          }

          // ── Screened re-solve pass (mode `screened` only) ───────────
          //
          // Rank the synthesized cuts by |intercept correction| — the
          // apertures whose shared support moved furthest from the
          // representative are where the synthesized cut is loosest —
          // and re-solve the top `aperture_screen_count` exactly on the
          // resident basis (bound/RHS deltas, warm dual simplex),
          // replacing their synthesized cuts with exact ones.  In-chunk
          // duplicates that copied a synthesized cut BEFORE this pass
          // deliberately keep the shared version (see the memo comment
          // above — both cuts underestimate the same Q_a).
          if (aperture_solve_mode == ApertureSolveMode::screened
              && aperture_screen_count > 0 && !ds_synth.empty())
          {
            const auto n_screen =
                std::min(ds_synth.size(),
                         static_cast<std::size_t>(aperture_screen_count));
            std::ranges::partial_sort(
                ds_synth,
                ds_synth.begin() + static_cast<std::ptrdiff_t>(n_screen),
                std::ranges::greater {},
                [](const DsSynthEntry& e) { return std::abs(e.corr); });
            const std::span<const StateVarLink> links_for_cut =
                cut_links.empty()
                ? std::span<const StateVarLink>(src_state.outgoing_links)
                : cut_links;
            for (std::size_t i = 0; i < n_screen; ++i) {
              const auto& sel = ds_synth[i];
              auto& target = results[sel.result_idx];
              // Re-apply this aperture's bounds (the clone currently
              // carries the LAST processed aperture's bounds).
              (void)apply_aperture_update(prepared[sel.prep_idx]);
              clone.relax_integers();  // idempotent on the shared clone
              const SolverOptions& re_opts =
                  clone_has_basis ? warm_opts : aperture_opts;
              const auto solve_t0 = std::chrono::steady_clock::now();
              [[maybe_unused]] auto solve_result = clone.resolve(re_opts);
              warm_solve_s += std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - solve_t0)
                                  .count();
              ++n_warm_solves;
              if (!clone.is_optimal()) {
                // Exact re-solve failed (e.g. this aperture's LP is
                // primal-infeasible: Q_a = +∞).  The synthesized cut
                // stays — any finite cut underestimates +∞, and for
                // feasible-but-unsolved cases weak duality still holds.
                spdlog::debug(
                    "SDDP Aperture [i{} s{} p{} a{}]: screened re-solve "
                    "not optimal (status {}), keeping dual-shared cut",
                    gtopt::uid_of(iteration_index),
                    scene_uid_val,
                    phase_uid_val,
                    target.ap_uid,
                    clone.get_status());
                continue;
              }
              auto exact_cut = build_benders_cut_physical(src_alpha_col,
                                                          links_for_cut,
                                                          clone,
                                                          clone.get_obj_value(),
                                                          cut_coeff_eps);
              sddp_aperture_cut_tag.apply_to(exact_cut);
              exact_cut.variable_uid = phase_uid_val;
              exact_cut.context = make_aperture_context(
                  scene_uid_val, phase_uid_val, target.ap_uid, total_cuts);
              target.cut = std::move(exact_cut);
              ++ds_n_screen_resolved;
            }
          }

          // ── Dual-shared timing/coverage summary ─────────────────────
          if (ds_mode && !ds_synth.empty()
              && spdlog::should_log(spdlog::level::debug))
          {
            spdlog::debug(
                "SDDP Aperture dual-shared [s{} p{}]: {} exact solve(s) "
                "({} cold + {} warm), {} synthesized, {} delta fallback(s), "
                "{} screened re-solve(s)",
                scene_uid_val,
                phase_uid_val,
                n_cold_solves + n_warm_solves,
                n_cold_solves,
                n_warm_solves,
                ds_synth.size(),
                ds_n_delta_fallback,
                ds_n_screen_resolved);
          }

          // spdlog::trace function form — SPDLOG_TRACE macro is baked
          // to `(void)0` in this build (PCH compiles spdlog at INFO).
          // Gate explicitly so we don't materialise the chunk elapsed
          // time / std::format args on the common no-trace path.
          if (spdlog::should_log(spdlog::level::trace)) {
            const auto chunk_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now()
                                              - chunk_start)
                    .count();
            spdlog::trace(
                "SDDP Aperture [i{} s{} p{}]: chunk of {} done ({:.3f}s) "
                "[thread {}]",
                gtopt::uid_of(iteration_index),
                scene_uid_val,
                phase_uid_val,
                chunk.size(),
                chunk_s,
                std::hash<std::thread::id> {}(task_tid) % 10000);
          }

          // ── Warm-start timing summary (opt-in path only) ────────────
          // Reports the cold-seed vs warm-resolve cost so the speedup is
          // measurable from the logs without a profiler.
          if (aperture_solve_mode == ApertureSolveMode::warm
              && n_warm_solves > 0 && spdlog::should_log(spdlog::level::debug))
          {
            const double cold_avg_ms =
                n_cold_solves > 0 ? (cold_solve_s / n_cold_solves) * 1e3 : 0.0;
            const double warm_avg_ms = (warm_solve_s / n_warm_solves) * 1e3;
            spdlog::debug(
                "SDDP Aperture warm-start [s{} p{}]: {} cold {:.1f}ms/avg "
                "+ {} warm {:.1f}ms/avg (warm/cold={:.2f})",
                scene_uid_val,
                phase_uid_val,
                n_cold_solves,
                cold_avg_ms,
                n_warm_solves,
                warm_avg_ms,
                cold_avg_ms > 0.0 ? warm_avg_ms / cold_avg_ms : 0.0);
          }

          return results;
        }));
  }

  // ── Collect results ─────────────────────────────────────────────────

  std::vector<SparseRow> aperture_cuts;
  std::vector<double> aperture_weights;
  double total_weight = 0.0;
  int n_infeasible = 0;

  // Each future yields a chunk's worth of per-aperture results
  // (1..K elements depending on chunk_size).  Flatten as we collect.
  {
    // Release this task's worker slot for the duration of the blocking
    // chunk-future wait.  Without this, the master backward task holds
    // a slot while parked on `fut.get()`, the pool's CPU/thread gate
    // counts it as "active", and once the gate threshold trips the
    // pool wedges (every cell task waiting on chunk futures, no chunk
    // worker dispatched).  No-op when `pool_for_slot_release == nullptr`.
    auto slot_guard = (pool_for_slot_release != nullptr)
        ? pool_for_slot_release->release_slot_while_blocking()
        : SDDPWorkPool::SlotReleaseGuard {nullptr};

    for (auto& fut : futures) {
      auto chunk_results = fut.get();
      for (auto& result : chunk_results) {
        if (!result.feasible) {
          ++n_infeasible;
          if (aperture_timeout > 0.0
              && (result.status == 1 || result.status == 3))
          {
            spdlog::warn(
                "SDDP Aperture [i{} s{} p{} a{}]: timed out ({:.1f}s, "
                "status {}), treating as infeasible",
                gtopt::uid_of(iteration_index),
                scene_uid_val,
                phase_uid_val,
                result.ap_uid,
                aperture_timeout,
                result.status);
          } else if (spdlog::should_log(spdlog::level::trace)) {
            spdlog::trace(
                "SDDP Aperture [i{} s{} p{} a{}]: infeasible (status {}), "
                "skipping",
                gtopt::uid_of(iteration_index),
                scene_uid_val,
                phase_uid_val,
                result.ap_uid,
                result.status);
          }
          continue;
        }

        if (result.cut.has_value()) {
          aperture_cuts.push_back(std::move(*result.cut));
          aperture_weights.push_back(result.weight);
          total_weight += result.weight;
        }
      }
    }
  }

  // Persist this iteration's first-aperture basis for next iteration's seed.
  // Every chunk future has joined above, so the capturing write (first chunk's
  // worker) happens-before this read — no extra synchronisation needed.
  if (captured_basis_out != nullptr && captured_first_basis.has_value()) {
    *captured_basis_out = std::move(*captured_first_basis);
  }

  // Log summary — ONE line per (iter, scene, phase) at INFO so it
  // pairs symmetrically with the forward-pass per-phase line.
  // Together they form the canonical "one line per (scene, phase)"
  // backward visibility under the default aperture-enabled path:
  // forward emits its opex line, aperture emits this feasibility
  // line.  Removing this line removes per-phase backward visibility
  // entirely (the in-phase no-aperture `cut z=` line only fires
  // when apertures are explicitly disabled).
  //
  // Argument-formatting cost is paid only when the runtime log
  // level admits INFO: `sddp_log(...)` returns a lightweight
  // `SDDPLogTag` aggregate (string_view + uids, no alloc) and the
  // formatter that materialises the string is invoked by spdlog /
  // std::format AFTER the level filter.
  const auto n_total = effective_apertures.size();
  const auto n_feasible = aperture_cuts.size();
  const auto phase_elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - phase_start)
                                 .count();
  spdlog::info(
      "{}: {}/{} feasible, {} infeasible, {} skipped ({:.3f}s) "
      "[thread {}]",
      sddp_log("Aperture",
               gtopt::uid_of(iteration_index),
               scene_uid_val,
               phase_uid_val),
      n_feasible,
      n_total,
      n_infeasible,
      n_skipped,
      phase_elapsed,
      std::hash<std::thread::id> {}(caller_tid) % 10000);

  if (aperture_cuts.empty()) {
    return std::nullopt;
  }

  // Normalise weights
  if (total_weight > 0.0) {
    for (auto& w : aperture_weights) {
      w /= total_weight;
    }
  }

  // Compute the probability-weighted expected cut
  auto ecut = weighted_average_benders_cut(aperture_cuts, aperture_weights);
  sddp_ecut_tag.apply_to(ecut);
  ecut.variable_uid = phase_uid_val;
  ecut.context = make_iteration_context(
      scene_uid_val, phase_uid_val, uid_of(iteration_index), total_cuts);
  return ecut;
}

}  // namespace gtopt
