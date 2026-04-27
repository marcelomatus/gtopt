/**
 * @file      sddp_forward_pass.cpp
 * @brief     SDDP forward pass implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp — implements the forward pass that
 * solves each phase LP in sequence, propagating state variables and
 * handling elastic fallback for infeasible phases.
 */

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <thread>

#include <gtopt/as_label.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
}  // namespace

auto SDDPMethod::forward_pass(SceneIndex scene_index,
                              const SolverOptions& opts,
                              IterationIndex iteration_index)
    -> std::expected<double, Error>
{
  const auto& phases = planning_lp().simulation().phases();
  auto& phase_states = m_scene_phase_states_[scene_index];
  double total_opex = 0.0;
  const auto fwd_scene_start = std::chrono::steady_clock::now();

  // Apply solve_timeout to the solver options if configured
  auto effective_opts = opts;
  if (m_options_.solve_timeout > 0.0) {
    effective_opts.time_limit = m_options_.solve_timeout;
  }

  // Per-scene "starting" trace dropped to DEBUG: the aggregate
  // "SDDP Forward [iN]: dispatching M scene(s) ..." line in
  // sddp_method.cpp already covers the user-facing fan-out, and
  // emitting one INFO line per scene per iteration was just noise
  // (16 lines at the same millisecond on this run).
  [[maybe_unused]] const auto fwd_tid = std::this_thread::get_id();
  SPDLOG_DEBUG("SDDP Forward [i{} s{}]: starting ({} phases) [thread {}]",
               iteration_index,
               uid_of(scene_index),
               phases.size(),
               std::hash<std::thread::id> {}(fwd_tid) % 10000);

  // Check once whether update_lp should run for this iteration
  // (respects explicit skip/force flags and global skip count).
  const bool do_update = should_dispatch_update_lp(iteration_index);

  // PLP-style backtracking Benders forward pass.
  // `phase_idx` is an Index (signed), not a PhaseIndex, so we can
  // decrement on elastic (backtrack to p-1) without signed-underflow
  // concerns at phase 0.  Each loop iteration is one solve attempt;
  // the total is capped by `forward_max_attempts` so a pathological
  // backtrack cycle cannot run forever.  Matches the control flow of
  // `plp-faseprim.f::faseprim` (CALL AgrFact → IEta = IEta - 1 → CYCLE).
  const auto num_phases = static_cast<Index>(phases.size());
  const auto max_attempts =
      (m_options_.forward_max_attempts > 0 ? m_options_.forward_max_attempts
                                           : 100);
  Index phase_idx = 0;
  int attempts = 0;
  while (phase_idx < num_phases) {
    ++attempts;
    if (attempts > max_attempts) {
      const auto cur_phase = PhaseIndex {phase_idx};
      SPDLOG_WARN(
          "SDDP Forward [i{} s{}]: exceeded forward_max_attempts={} "
          "(last phase p{}); declaring scene infeasible for this iteration",
          iteration_index,
          uid_of(scene_index),
          max_attempts,
          uid_of(cur_phase));
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "{}: forward pass exceeded forward_max_attempts={} "
              "(last phase p{})",
              sddp_log("Forward", iteration_index, uid_of(scene_index)),
              max_attempts,
              uid_of(cur_phase)),
      });
    }

    const auto phase_index = PhaseIndex {phase_idx};
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format("{}: cancelled",
                                 sddp_log("Forward",
                                          iteration_index,
                                          uid_of(scene_index),
                                          uid_of(phase_index))),
      });
    }

    auto& system = planning_lp().system(scene_index, phase_index);
    auto& state = phase_states[phase_index];

    // Ensure the LP backend is live.  No-op when mode=off or when a
    // prior task already reloaded this cell; otherwise reconstructs
    // from snapshot (snapshot/compress) or re-flattens from collections
    // (rebuild).  dispatch_update_lp() runs after this point, so
    // coefficient updates are replayed naturally.
    system.ensure_lp_built();

    auto& li = system.linear_interface();

    // Propagate state variables from previous phase.
    // Trial values are read from each link's source StateVariable
    // (col_sol() mirror, populated by the previous phase's solve via
    // `capture_state_variable_values`).  Mode-agnostic: works identically
    // across LowMemoryMode off / snapshot / compress / rebuild.
    if (phase_index) {
      const auto prev_phase_index = previous(phase_index);
      auto& prev_st = phase_states[prev_phase_index];

      propagate_trial_values(prev_st.outgoing_links, li);

      SPDLOG_TRACE(
          "SDDP Forward [i{} s{} p{}]: propagated {} state vars from phase {}",
          iteration_index,
          uid_of(scene_index),
          uid_of(phase_index),
          prev_st.outgoing_links.size(),
          uid_of(prev_phase_index));
    }

    // Update volume-dependent LP coefficients (discharge limits, turbine
    // efficiency, seepage, production factor) AFTER state propagation so
    // that physical_eini reflects the actual forward-pass reservoir volume
    // rather than default_volume.
    if (do_update) {
      const auto updated = update_lp_for_phase(scene_index, phase_index);
      if (updated > 0) {
        SPDLOG_TRACE("SDDP Forward [i{} s{} p{}]: updated {} LP elements",
                     iteration_index,
                     uid_of(scene_index),
                     uid_of(phase_index),
                     updated);
      }
    }

    // If lp_debug is enabled, write LP file (pre-solve state) then optionally
    // submit gzip compression as a fire-and-forget async task.
    // Selective filters (scene/phase range) skip non-matching LPs.
    if (m_options_.lp_debug) {
      const bool in_range = in_lp_debug_range(uid_of(scene_index),
                                              uid_of(phase_index),
                                              m_options_.lp_debug_scene_min,
                                              m_options_.lp_debug_scene_max,
                                              m_options_.lp_debug_phase_min,
                                              m_options_.lp_debug_phase_max);
      if (in_range) {
        // Include `attempts` in the filename so backtrack re-entries
        // of the same (scene, phase, iter) cell produce distinct
        // snapshots.  Lets the post-mortem reconstruct the fcut
        // accumulation chronology across the cascade.
        const auto dbg_stem = (std::filesystem::path(m_options_.log_directory)
                               / std::format(sddp_file::debug_lp_fmt,
                                             uid_of(scene_index),
                                             uid_of(phase_index),
                                             iteration_index,
                                             attempts))
                                  .string();
        m_lp_debug_writer_.write(li, dbg_stem);
      }
    }

    // Configure solver log file based on log_mode.
    // Configure per-solve log file when log_mode is detailed.
    if (effective_opts.log_mode.value_or(SolverLogMode::nolog)
            == SolverLogMode::detailed
        && !m_options_.log_directory.empty())
    {
      std::filesystem::create_directories(m_options_.log_directory);
      li.set_log_file(
          (std::filesystem::path(m_options_.log_directory)
           / as_label(
               li.solver_name(), uid_of(scene_index), uid_of(phase_index)))
              .string());
    }

    // Tag the LP with the SDDP Forward key so fallback warnings emitted
    // by LinearInterface::resolve() carry the same context as the
    // surrounding SDDP info logs.
    li.set_log_tag(sddp_log(
        "Forward", iteration_index, uid_of(scene_index), uid_of(phase_index)));

    // Solve directly — already running in a pool thread.
    auto result = li.resolve(effective_opts);

    // Capture status before any release — release loses specific codes.
    const auto solve_status = li.get_status();

    if (!result.has_value() || solve_status != 0) {
      if (m_options_.solve_timeout > 0.0
          && (solve_status == 1 || solve_status == 3))
      {
        spdlog::critical(
            "SDDP Forward [i{} s{} p{}]: solve timeout ({:.1f}s) (status {})",
            iteration_index,
            uid_of(scene_index),
            uid_of(phase_index),
            m_options_.solve_timeout,
            solve_status);
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: solve timeout ({:.1f}s) (status {})",
                                   sddp_log("Forward",
                                            iteration_index,
                                            uid_of(scene_index),
                                            uid_of(phase_index)),
                                   m_options_.solve_timeout,
                                   solve_status),
            .status = solve_status,
        });
      }

      // Pre-elastic notice demoted to DEBUG: the final outcome line
      // ("elastic ok" + "installed fcut") at INFO level is the one-line
      // summary callers expect per infeasible LP.  Keep the status code
      // accessible under `-v` / trace log for deep post-mortems.
      SPDLOG_DEBUG(
          "SDDP Forward [i{} s{} p{}]: non-optimal (status {}), trying elastic"
          " solve",
          iteration_index,
          uid_of(scene_index),
          uid_of(phase_index),
          solve_status);

      // Optional pre-elastic LP dump — gated on `lp_debug` + the user's
      // `lp_debug_{scene,phase}_{min,max}` window (same convention as
      // aperture_pass.cpp:412-421) so it does NOT fire on every
      // cascade backtrack by default.  Captures the phase LP with the
      // trial state from the previous phase FIXED onto state-variable
      // columns — the first infeasible LP of a cascade, complementing
      // the existing `error_*.lp` path (which only dumps when elastic
      // recovery gives up at the deepest backtrack).  Filename:
      //   <log_directory>/preelastic_s<scene>_p<phase>_i<iter>.lp
      if (m_options_.lp_debug && !m_options_.log_directory.empty()
          && in_lp_debug_range(uid_of(scene_index),
                               uid_of(phase_index),
                               m_options_.lp_debug_scene_min,
                               m_options_.lp_debug_scene_max,
                               m_options_.lp_debug_phase_min,
                               m_options_.lp_debug_phase_max))
      {
        std::error_code mkdir_ec;
        std::filesystem::create_directories(m_options_.log_directory, mkdir_ec);
        const auto pre_stem = (std::filesystem::path(m_options_.log_directory)
                               / std::format("preelastic_s{}_p{}_i{}",
                                             uid_of(scene_index),
                                             uid_of(phase_index),
                                             iteration_index))
                                  .string();
        if (auto wr = li.write_lp(pre_stem); !wr) {
          spdlog::warn(
              "SDDP Forward [i{} s{} p{}]: failed to save pre-elastic LP "
              "to {}: {}",
              iteration_index,
              uid_of(scene_index),
              uid_of(phase_index),
              pre_stem,
              wr.error().message);
        }
      }

      // Clone the LP, apply elastic filter, and solve the clone.
      // The original LP remains unmodified (PLP clone pattern).
      auto elastic_result = elastic_solve(scene_index, phase_index, opts);

      // Release solver backend — no-op when low_memory is off.
      system.release_backend();

      if (elastic_result.has_value() && elastic_result->solved) {
        m_phase_grid_.record(iteration_index,
                             uid_of(scene_index),
                             phase_index,
                             GridCell::Elastic);
        // Track max kappa from elastic solve (single use of clone).
        update_max_kappa(
            scene_index, phase_index, elastic_result->clone, iteration_index);
        // Increment infeasibility counter for this (scene, phase)
        ++m_infeasibility_counter_[scene_index][phase_index];

        // INVARIANT: state_var sol/rcost AND `state.forward_*` fields
        // are updated ONLY from an optimal solve of the ORIGINAL forward
        // LP (see feasible branch below).  The elastic clone carries a
        // Chinneck Phase-1 objective (all original coefficients zeroed
        // + unit slack costs), so its primal/dual values represent the
        // minimum feasibility gap, not the economic dispatch.  Writing
        // them onto StateVariable or `state.forward_full_obj_physical`
        // would contaminate downstream consumers — next phase's trial
        // propagation and the backward-pass bcut fallback that reads
        // `state_var.reduced_cost_physical()` and
        // `target_state.forward_full_obj_physical`.  The clone is used
        // only to build the fcut below, then destroyed when
        // `elastic_result` goes out of scope at the end of the branch.

        // PLP-style: install a Benders feasibility cut on phase p-1
        // right from the forward pass, so the next forward iteration
        // avoids regenerating the same infeasible trial point.  Uses
        // reduced costs on the dependent state-var columns from the
        // elastic clone (cost_raw at optimum of the relaxed LP).  When
        // the per-(scene, phase) infeasibility counter exceeds
        // multi_cut_threshold, additionally install one bound cut per
        // activated slack (`build_multi_cuts`).
        int mc_added = 0;
        std::string state_elems;
        if (phase_index) {
          const auto prev_phase_index = previous(phase_index);
          auto& prev_sys = planning_lp().system(scene_index, prev_phase_index);
          const auto& prev_state = phase_states[prev_phase_index];
          prev_sys.ensure_lp_built();
          // prev_sys.linear_interface() no longer accessed directly here —
          // `add_cut_row` routes through the prev-phase system internally.

          const auto infeas_count =
              m_infeasibility_counter_[scene_index][phase_index];

          // D11 — PLP convention: aggregated π-weighted feasibility cut
          // AND per-reservoir Farkas-ray multi-cuts are **mutually
          // exclusive**.  The PLP `FOneFeasRay` boolean in
          // `plp-agrespd.f::AgrElastici` switches between the two
          // modes at the call site; we replicate that here.
          //
          // Stacking both (gtopt's pre-2026-04 behaviour) over-
          // constrains p_{t-1}: the aggregated cut already excludes
          // the infeasible trial point via a single hyperplane, and
          // adding per-reservoir bound cuts on top introduces
          // potentially inconsistent Farkas-ray slices that can
          // make p_{t-1} itself infeasible — the observed collapse
          // on plp_2_years phase 27.
          //
          // Threshold ≤ 0: immediately use multi-cut; > 0: wait for
          // `infeas_count ≥ threshold` cumulative events at this
          // (scene, phase) before switching.  The counter is
          // persistent (does not reset on successful solves).
          const bool use_multi_cut =
              (m_options_.elastic_filter_mode == ElasticFilterMode::multi_cut)
              || (m_options_.multi_cut_threshold == 0)
              || (m_options_.multi_cut_threshold > 0
                  && infeas_count >= m_options_.multi_cut_threshold);

          // Aggregated feasibility cut — emitted ONLY when we are
          // NOT in multi-cut mode (D11 exclusivity).  Uses the α-free
          // Birge-Louveaux π-weighted builder from commit ae4ba13d,
          // which reads row duals of the fixing equations (not
          // reduced costs) from the elastic clone.
          auto feas_cut = !use_multi_cut
              ? build_feasibility_cut_physical(prev_state.outgoing_links,
                                               elastic_result->link_infos,
                                               elastic_result->clone,
                                               m_options_.cut_coeff_eps,
                                               static_cast<int>(infeas_count))
              : SparseRow {};

          if (!use_multi_cut) {
            feas_cut.class_name = sddp_alpha_class_name;
            feas_cut.constraint_name = sddp_fcut_constraint_name;
            // variable_uid = prev phase UID from master (#426) — without
            // this the row carries unknown_uid=-1 which serialises as
            // `sddp_fcut_-1_…`, rejected by CoinLpIO's row-name validator.
            feas_cut.variable_uid = uid_of(prev_phase_index);
            feas_cut.context =
                make_iteration_context(uid_of(scene_index),
                                       uid_of(phase_index),
                                       gtopt::uid_of(iteration_index),
                                       infeas_count);
            // α stays pinned at `[0, 0]` (bootstrap) on a feasibility
            // cut.  Feasibility cuts only assert "these master states
            // cause downstream infeasibility" — they convey no lower
            // bound on the true future cost.  α is only freed by
            // aperture/Benders backward-pass optimality cuts.
            const auto cut_row = add_cut_row(planning_lp(),
                                             scene_index,
                                             prev_phase_index,
                                             CutType::Feasibility,
                                             feas_cut,
                                             m_options_.cut_coeff_eps);
            store_cut(scene_index,
                      prev_phase_index,
                      feas_cut,
                      CutType::Feasibility,
                      cut_row);
          }

          if (use_multi_cut) {
            auto mc_cuts = build_multi_cuts(
                *elastic_result,
                prev_state.outgoing_links,
                make_iteration_context(uid_of(scene_index),
                                       uid_of(phase_index),
                                       gtopt::uid_of(iteration_index),
                                       infeas_count),
                m_options_.cut_coeff_eps,
                static_cast<int>(infeas_count));
            for (auto& mc : mc_cuts) {
              const auto cut_row = add_cut_row(planning_lp(),
                                               scene_index,
                                               prev_phase_index,
                                               CutType::Feasibility,
                                               mc);
              store_cut(scene_index,
                        prev_phase_index,
                        mc,
                        CutType::Feasibility,
                        cut_row);
              ++mc_added;
            }

            // P0-B fallback (2026-04-23): if `build_multi_cuts`
            // filtered every link (all |π| < slack_tol — happens
            // on near-degenerate elastic solves where the dual is
            // numerically zero at the vertex), fall back to the
            // aggregated π-weighted `build_feasibility_cut_physical`.
            // Without this fallback the forward pass would silently
            // install zero cuts, then loop until
            // `forward_max_attempts` exhausts — a silent convergence
            // failure indistinguishable from a successful cut install
            // in the logs.  Mirrors PLP's `AgrElastici` which always
            // emits a single aggregated Farkas-ray cut.
            if (mc_added == 0) {
              SPDLOG_WARN(
                  "SDDP Forward [i{} s{} p{}]: multi_cut produced 0 cuts "
                  "(all |π| < slack_tol) — falling back to aggregated "
                  "Benders fcut on p{} to avoid silent convergence loop",
                  iteration_index,
                  uid_of(scene_index),
                  uid_of(phase_index),
                  uid_of(prev_phase_index));

              feas_cut = build_feasibility_cut_physical(
                  prev_state.outgoing_links,
                  elastic_result->link_infos,
                  elastic_result->clone,
                  m_options_.cut_coeff_eps,
                  static_cast<int>(infeas_count));
              feas_cut.class_name = sddp_alpha_class_name;
              feas_cut.constraint_name = sddp_fcut_constraint_name;
              // Same uid invariant as the non-multi-cut path (master #426)
              // — avoids `sddp_fcut_-1_…` rows that CoinLpIO rejects.
              feas_cut.variable_uid = uid_of(prev_phase_index);
              feas_cut.context =
                  make_iteration_context(uid_of(scene_index),
                                         uid_of(phase_index),
                                         gtopt::uid_of(iteration_index),
                                         infeas_count);

              const auto cut_row = add_cut_row(planning_lp(),
                                               scene_index,
                                               prev_phase_index,
                                               CutType::Feasibility,
                                               feas_cut,
                                               m_options_.cut_coeff_eps);
              store_cut(scene_index,
                        prev_phase_index,
                        feas_cut,
                        CutType::Feasibility,
                        cut_row);
              ++mc_added;
            }
          }

          // List the state-variable elements actually present in the
          // installed cut (survivors of cut_coeff_eps filtering), so
          // the diagnostic points at the elements driving the fcut.
          // Format: "Class:uid:col_name, ..." (e.g. "Reservoir:8:efin").
          for (const auto& link : prev_state.outgoing_links) {
            if (!use_multi_cut && !feas_cut.cmap.contains(link.source_col)) {
              continue;
            }
            if (!state_elems.empty()) {
              state_elems += ", ";
            }
            // Prefer the human-readable name (e.g. "Reservoir:LMAULE:efin")
            // when resolvable via the AMPL element-name registry; fall back
            // to numeric uid when the element has no registered name
            // (test fixtures, PAMPL-less JSON input).
            if (!link.name.empty()) {
              state_elems += std::format(
                  "{}:{}:{}", link.class_name, link.name, link.col_name);
            } else {
              state_elems += std::format(
                  "{}:{}:{}", link.class_name, Index {link.uid}, link.col_name);
            }
          }

          // One-line per infeasible LP, emitted *after* the fcut (and any
          // multi-cuts) have been installed on prev_li.  Timing is
          // deliberate — the log signals a completed cut install, not
          // merely the detection of infeasibility.  obj / opex values are
          // deliberately absent: the clone's objective is a feasibility
          // gap, not a cost, and mixing it into the forward-pass log
          // would invite misinterpretation.  Trailing tag distinguishes
          // the two control-flow modes:
          //   `fail-stop` (default): scene's forward pass exits this
          //     iteration after the fcut is installed; the next
          //     iteration restarts from p1 with the new cut available
          //     in the global cut store.
          //   `backtrack→p{}` (legacy): phase_idx is decremented and
          //     p-1 is re-solved under the new fcut.  Matches PLP's
          //     "retrocedemos a la etapa anterior" log in faseprim.
          if (m_options_.forward_fail_stop) {
            SPDLOG_INFO(
                "SDDP Forward [i{} s{} p{}]: elastic → fcut on p{} "
                "(infeas_count={}{}) state=[{}] fail-stop scene",
                iteration_index,
                uid_of(scene_index),
                uid_of(phase_index),
                uid_of(prev_phase_index),
                infeas_count,
                mc_added > 0 ? std::format(" +{}mc", mc_added) : "",
                state_elems);

            // Scene-level fail-stop: the fcut on p-1 has been installed
            // and persists in the global cut store across iterations.
            // Returning Error here causes `run_forward_pass_all_scenes`
            // to set `scene_feasible[scene_index] = 0` for this iter
            // (other scenes continue uninterrupted); the next iteration
            // restarts every scene's forward pass from p1 with the
            // freshly accumulated cuts.
            return std::unexpected(Error {
                .code = ErrorCode::SolverError,
                .message =
                    std::format("{}: fail-stop after elastic fcut on p{} "
                                "(infeas_count={})",
                                sddp_log("Forward",
                                         iteration_index,
                                         uid_of(scene_index),
                                         uid_of(phase_index)),
                                uid_of(prev_phase_index),
                                infeas_count),
            });
          }

          SPDLOG_INFO(
              "SDDP Forward [i{} s{} p{}]: elastic → fcut on p{} "
              "(infeas_count={}{}) state=[{}] backtrack→p{}",
              iteration_index,
              uid_of(scene_index),
              uid_of(phase_index),
              uid_of(prev_phase_index),
              infeas_count,
              mc_added > 0 ? std::format(" +{}mc", mc_added) : "",
              state_elems,
              uid_of(prev_phase_index));

          // PLP-style backtrack: step phase_idx back to p-1 and re-solve
          // it with the new fcut.  If p-1 is now infeasible too, the
          // next iteration of the while loop will install another fcut
          // on p-2 and recurse — bounded by `forward_max_attempts`.
          --phase_idx;
          continue;
        }
        // `elastic_result` (and its clone) is destroyed here.  No value
        // from the Chinneck Phase-1 LP survives this block.
      } else {
        // Elastic filter could not produce a feasibility cut.  Two
        // mutually exclusive reasons:
        //   A. phase_index == 0 (no predecessor phase to cut on)
        //   B. the state-variable-relaxed clone was itself infeasible
        //      (infeasibility rooted outside state-variable bounds —
        //       e.g. demand balance, flow limits, or a pre-installed
        //       cut row — which the elastic filter cannot relax)
        // Per SDDP theory, no recovery is possible from either — the
        // scene is declared infeasible for this iteration.  The caller
        // (run_forward_pass_all_scenes) sets scene_feasible[s] = 0 when
        // it sees the returned Error; compute_iteration_bounds then
        // excludes this scene from UB/LB.  Retries on the same state
        // cannot change this outcome.
        //
        // Dump the original (pre-elastic) LP to <log_directory>/
        // error_s<scene_uid>_p<phase_uid>_i<iteration_index>.lp so the
        // user can diagnose offline.  `elastic_solve` never touches the
        // original LP (PLP clone pattern), so `system.linear_interface()`
        // still holds the state that failed.  `release_backend()` above
        // may have dropped the solver object under low_memory; rebuild
        // it before writing.  Failure to write is non-fatal — we log a
        // warning and continue returning the original error.
        std::string saved_error_lp;
        std::string saved_elastic_lp;
        {
          std::error_code mkdir_ec;
          std::filesystem::create_directories(m_options_.log_directory,
                                              mkdir_ec);
          system.ensure_lp_built();
          const auto stem = (std::filesystem::path(m_options_.log_directory)
                             / std::format(sddp_file::error_lp_fmt,
                                           uid_of(scene_index),
                                           uid_of(phase_index),
                                           iteration_index))
                                .string();
          // `linear_interface().write_lp` appends ".lp" but keeps the
          // stem verbatim.  `SystemLP::write_lp` would additionally
          // tack on `_scene_X_phase_Y`, duplicating the UIDs already
          // baked into our stem — so we bypass it.
          if (auto wr = system.linear_interface().write_lp(stem); wr) {
            saved_error_lp = stem + ".lp";
          } else {
            spdlog::warn(
                "SDDP Forward [i{} s{} p{}]: failed to save error LP "
                "to {}: {}",
                iteration_index,
                uid_of(scene_index),
                uid_of(phase_index),
                stem,
                wr.error().message);
          }

          // Also persist the elastic clone (if elastic_solve returned
          // one) so post-mortem can diagnose WHY the state-variable
          // relaxation alone couldn't restore feasibility — the
          // original LP + the unsolved clone together pinpoint rows
          // the filter cannot relax.  Suffix `_elastic` so the two
          // files are colocated and trivially distinguishable.
          if (elastic_result.has_value()) {
            const auto elastic_stem = stem + "_elastic";
            if (auto wr = elastic_result->clone.write_lp(elastic_stem); wr) {
              saved_elastic_lp = elastic_stem + ".lp";
            } else {
              spdlog::warn(
                  "SDDP Forward [i{} s{} p{}]: failed to save elastic "
                  "clone LP to {}: {}",
                  iteration_index,
                  uid_of(scene_index),
                  uid_of(phase_index),
                  elastic_stem,
                  wr.error().message);
            }
          }
        }

        spdlog::warn(
            "SDDP Forward [i{} s{} p{}]: elastic filter produced no "
            "feasibility cut — declaring phase p{} and scene s{} "
            "infeasible for iter i{} (solver status {}, reason: {}){}",
            iteration_index,
            uid_of(scene_index),
            uid_of(phase_index),
            uid_of(phase_index),
            uid_of(scene_index),
            iteration_index,
            solve_status,
            !phase_index ? "no predecessor phase to cut on"
                         : "relaxed clone infeasible",
            saved_error_lp.empty() && saved_elastic_lp.empty()
                ? std::string {}
                : std::format(
                      " [LP{} saved to {}{}{}]",
                      saved_elastic_lp.empty() ? "" : "s",
                      saved_error_lp,
                      (saved_error_lp.empty() || saved_elastic_lp.empty())
                          ? ""
                          : " + ",
                      saved_elastic_lp));
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: elastic filter produced no "
                                   "feasibility cut (status {})",
                                   sddp_log("Forward",
                                            iteration_index,
                                            uid_of(scene_index),
                                            uid_of(phase_index)),
                                   solve_status),
        });
      }
    } else {
      // Phase solved normally — counter is intentionally NOT reset.
      // Persistent counter (PLP convention): a (scene, phase) that flaps
      // between feasible and infeasible across iterations should still
      // accumulate towards the multi_cut auto-switch threshold; the
      // intermittent successes don't mean the elastic cut history is
      // adequate.  See docs/methods/sddp.md §S5.4.
      m_phase_grid_.record(
          iteration_index, uid_of(scene_index), phase_index, GridCell::Forward);
      // Track max kappa from forward solve
      update_max_kappa(scene_index, phase_index, li, iteration_index);

      // Cache solution data for the backward pass — must happen before
      // release_backend() which may discard cached solution vectors.
      // Store the objective in physical ($) space so the backward pass
      // can call `build_benders_cut_physical` without re-applying
      // `scale_objective`.  We also use the physical view for the
      // ``forward_objective`` accumulation below: the alpha column's
      // physical contribution is ``alpha_col_LP * scale_alpha``
      // (= ``alpha_val``), so subtracting it from a physical ``obj``
      // produces a unit-consistent ``opex`` regardless of
      // ``scale_objective``.  The earlier code subtracted a LP-raw
      // ``obj`` (= physical / scale_objective) from a physical
      // ``alpha_val``, which silently broke ``upper_bound`` whenever
      // ``scale_objective != 1`` after the second SDDP iteration once
      // alpha started carrying inherited-cut weight.
      const auto obj_physical = li.get_obj_value();
      state.forward_full_obj_physical = obj_physical;

      // Physical-space, optimal-only bound-clamped view (see
      // `LinearInterface::get_col_sol`) — scrubs at-bound solver noise
      // so the next phase's `propagate_trial_values` pin stays inside
      // the target column's physical bound box.
      const auto sol_phys = li.get_col_sol();
      const auto rc = li.get_col_cost_raw();

      // Mirror per-state-variable runtime values onto the persistent
      // StateVariable objects.  These replace the former per-PhaseState
      // full-vector caches for both next-phase trial-value propagation
      // and backward-pass cut construction.  Cuts always use reduced
      // costs, so row duals are never needed here.
      capture_state_variable_values(scene_index, phase_index, sol_phys, rc);

      const auto sa = m_options_.scale_alpha;
      const auto* alpha_svar = find_alpha_state_var(
          planning_lp().simulation(), scene_index, phase_index);
      const auto alpha_val =
          (alpha_svar != nullptr) ? alpha_svar->col_sol() * sa : 0.0;
      state.forward_objective = obj_physical - alpha_val;

      // Release solver backend — no-op when low_memory is off.
      //
      // The flat-LP snapshot IS retained here even during simulation
      // Pass 1, because a later phase's solve may hit the elastic
      // branch and need to `ensure_lp_built()` an earlier phase to
      // install an fcut on it (see the elastic → fcut path above).
      // That reconstruct needs the earlier phase's snapshot intact.
      //
      // Aggressive snapshot drop runs AFTER Pass 1's retry loop has
      // converged — at that point no further in-pass reconstruct is
      // possible and only `PlanningLP::write_out` remains, which reads
      // from the Phase-2a cache and re-flattens `System` element
      // arrays (no snapshot needed).  See `drop_sim_snapshots()` on
      // PlanningLP called by `SDDPMethod::simulation_pass`.
      system.release_backend();

      // Guard against solver returning "optimal" with NaN values
      // (can happen when inherited cuts cause ill-conditioning).
      if (std::isnan(state.forward_objective)) {
        SPDLOG_WARN(
            "SDDP Forward [i{} s{} p{}]: solve returned optimal but"
            " forward_objective is NaN (obj={}, alpha={})",
            iteration_index,
            uid_of(scene_index),
            uid_of(phase_index),
            obj_physical,
            alpha_val);
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: optimal status but NaN in solution",
                                   sddp_log("Forward",
                                            iteration_index,
                                            uid_of(scene_index),
                                            uid_of(phase_index))),
        });
      }

      SPDLOG_TRACE(
          "SDDP Forward [i{} s{} p{}]: obj={:.4f} alpha={:.4f}"
          " opex={:.4f}",
          iteration_index,
          uid_of(scene_index),
          uid_of(phase_index),
          obj_physical,
          alpha_val,
          state.forward_objective);

      // Advance: phase_idx moves forward to p+1.  Backtracking paths
      // decrement phase_idx instead (see elastic branch above).
      ++phase_idx;
    }
  }

  // Sum per-phase opex AFTER the backtracking loop exits successfully
  // (phase_idx == num_phases).  Under PLP-style backtracking a phase
  // may be solved multiple times before moving forward, so
  // accumulating total_opex inside the loop would double-count.  Each
  // `state.forward_objective` holds the value from that phase's FINAL
  // (forward-moving) optimal solve — the sum here is the scene's UB
  // contribution.
  for (const auto& st : phase_states) {
    total_opex += st.forward_objective;
  }

  // Guard against NaN in total forward-pass cost.  Can happen when
  // inherited cuts cause solver ill-conditioning (barrier failure with
  // subsequent dual fallback producing NaN values).
  if (std::isnan(total_opex)) {
    SPDLOG_WARN(
        "SDDP Forward [i{} s{}]: total_opex is NaN —"
        " solver ill-conditioning",
        iteration_index,
        uid_of(scene_index));
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::format(
            "{}: forward pass total cost is NaN",
            sddp_log("Forward", iteration_index, uid_of(scene_index))),
    });
  }

  const auto fwd_scene_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now()
                                    - fwd_scene_start)
          .count();
  SPDLOG_INFO(
      "SDDP Forward [i{} s{}]: done, total_opex={:.4f} ({:.3f}s) [thread {}]",
      iteration_index,
      uid_of(scene_index),
      total_opex,
      fwd_scene_s,
      std::hash<std::thread::id> {}(fwd_tid) % 10000);
  return total_opex;
}

}  // namespace gtopt
