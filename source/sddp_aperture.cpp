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
#include <filesystem>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/collection.hpp>
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
      ++(it->second);
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

// ─── partition_apertures ────────────────────────────────────────────────────

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
    int chunk_size) -> std::optional<SparseRow>
{
  const auto& phase_li = sys.linear_interface();

  // Apply aperture timeout to solver options if configured.
  // Crossover policy:
  //   - simplex (primal/dual/default): crossover is a no-op for the
  //     simplex solution path, leave whatever the user set.
  //   - barrier: aperture cuts are built from reduced costs
  //     (`get_col_cost_raw`); barrier-without-crossover RC noise is
  //     at solver tolerance and filtered by `cut_coeff_eps`.  We
  //     used to hard-clear crossover here, but that prevented users
  //     from explicitly requesting vertex duals (e.g. for diagnosis
  //     or for stricter cut precision).  Honour the user's setting.
  auto aperture_opts = opts;
  if (aperture_timeout > 0.0) {
    aperture_opts.time_limit = aperture_timeout;
  }

  // Build the effective aperture list for this phase
  auto effective_apertures =
      build_effective_apertures(aperture_defs, phase_apertures);

  if (effective_apertures.empty()) {
    return std::nullopt;
  }

  // ── Submit all aperture tasks to the pool ────────────────────────────
  //
  // Each task is a complete unit: clone → update → warm-start → solve →
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
    const auto idx = (scen_it == all_scenarios.end())
        ? std::ptrdiff_t {-1}
        : std::distance(all_scenarios.begin(), scen_it);
    prepared.push_back(
        {.aperture = ap_ref, .count = ap_count, .scenario_idx = idx});
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
  const auto chunks = partition_apertures(prepared_view, chunk_size);

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
  //         per-element ``update_aperture`` writes flow col bounds
  //         densely, so apertures fully overwrite each other — no
  //         snapshot/restore needed.
  //      c. Solves; build cut from reduced costs.
  //   3. Returns one ``ApertureCutResult`` per inner aperture in
  //      input order.
  std::vector<std::future<ApertureChunkResult>> futures;
  futures.reserve(chunks.size());
  for (const auto chunk : chunks) {
    futures.push_back(submit_fn(
        [&, chunk, clone_mutex, use_manual_clone]() -> ApertureChunkResult
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
                  LinearInterface::CloneKind::shallow,
                  /*with_replay=*/true);
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
            const bool has_scen = prep.scenario_idx >= 0;
            const ScenarioLP* scen_ptr = has_scen
                ? &all_scenarios[static_cast<std::size_t>(prep.scenario_idx)]
                : nullptr;

            // Always run the visitor (no is_base_scenario short-
            // circuit): in chunked mode the clone may carry the
            // previous aperture's bounds, so the base-scenario
            // aperture must re-overwrite to base values explicitly.
            // Per-element `update_aperture` writes flow col bounds
            // densely → full overwrite → no snapshot/restore needed.
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
                  [[maybe_unused]] const auto ok =
                      e.update_aperture(clone, base_scenario, value_fn, stage);
                }
              }
              return true;
            };
            // `sys.collections()` is populated on the main thread
            // BEFORE chunk tasks are dispatched.  Read-only access
            // from many threads is safe; mutating it here would race.
            visit_elements(sys.collections(), visitor);

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

            // Solve.  After the first aperture in the chunk, every
            // subsequent solve warm-starts from the previous one's
            // basis on the shared clone — bound-only deltas keep
            // the dual basis valid → typically converges in a
            // fraction of a cold barrier.
            [[maybe_unused]] auto solve_result = clone.resolve(aperture_opts);
            const bool feasible = clone.is_optimal();
            if (!feasible) {
              const auto status = clone.get_status();
              const auto ap_s = std::chrono::duration<double>(
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

            auto cut = build_benders_cut_physical(src_alpha_col,
                                                  src_state.outgoing_links,
                                                  clone,
                                                  clone.get_obj_value(),
                                                  cut_coeff_eps);
            sddp_aperture_cut_tag.apply_to(cut);
            cut.variable_uid = phase_uid_val;
            cut.context = make_aperture_context(
                scene_uid_val, phase_uid_val, ap_uid, total_cuts);

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

            results.push_back(ApertureCutResult {
                .ap_uid = ap_uid,
                .weight = weight,
                .feasible = true,
                .status = 0,
                .cut = std::move(cut),
            });
            seen.push_back({.uid = ap_uid, .idx = results.size() - 1});
          }

          const auto chunk_s =
              std::chrono::duration<double>(std::chrono::steady_clock::now()
                                            - chunk_start)
                  .count();
          // spdlog::trace function form — SPDLOG_TRACE macro is baked
          // to `(void)0` in this build (PCH compiles spdlog at INFO),
          // which would also leave `chunk_s` unused → compile error.
          spdlog::trace(
              "SDDP Aperture [i{} s{} p{}]: chunk of {} done ({:.3f}s) "
              "[thread {}]",
              gtopt::uid_of(iteration_index),
              scene_uid_val,
              phase_uid_val,
              chunk.size(),
              chunk_s,
              std::hash<std::thread::id> {}(task_tid) % 10000);

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
        } else {
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
