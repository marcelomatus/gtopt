/**
 * @file      monolithic_method.cpp
 * @brief     Implementation of MonolithicMethod::solve()
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <mutex>
#include <optional>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto MonolithicMethod::solve(PlanningLP& planning_lp, const SolverOptions& opts)
    -> std::expected<int, Error>
{
  // ── Monitoring setup ──
  const auto solve_start = std::chrono::steady_clock::now();
  const auto num_scenes = static_cast<Index>(planning_lp.systems().size());

  SPDLOG_INFO("MonolithicMethod: starting {} scene(s)", num_scenes);

  // ── Boundary cuts ──
  if (!boundary_cuts_file.empty()
      && boundary_cuts_mode != BoundaryCutsMode::noload)
  {
    SPDLOG_INFO("MonolithicMethod: loading boundary cuts from '{}'",
                boundary_cuts_file);

    // Build temporary SDDPOptions with boundary cut settings.
    SDDPOptions bc_opts;
    bc_opts.boundary_cuts_file = boundary_cuts_file;
    bc_opts.boundary_cuts_mode = boundary_cuts_mode;
    bc_opts.boundary_cut_sharing = boundary_cut_sharing;
    bc_opts.boundary_max_iterations = boundary_max_iterations;
    // Mirror the SDDP α-rebase opt-in / scale onto the monolithic path so
    // the efin-based offset `c` is computed identically (see TASK 2).  The
    // planning-level SDDP options are the single source of truth for both
    // methods.
    bc_opts.boundary_cuts_mean_shift =
        planning_lp.options().sddp_boundary_cuts_mean_shift();
    bc_opts.missing_cut_var_mode =
        planning_lp.options().sddp_missing_cut_var_mode();
    bc_opts.cut_coeff_eps = planning_lp.options().sddp_cut_coeff_eps();

    // ── Register α (future-cost) state variables ──
    // The boundary-cut loader installs each cut as `α + Σ wvᵣ·efinᵣ ≥ FCF`
    // and *skips* the cut entirely when `find_alpha_state_var` returns
    // nullptr.  SDDP registers α via `SDDPMethod::initialize_alpha_variables`;
    // the monolithic path never did, so every boundary cut was silently
    // dropped (0 "Boundary" rows in the assembled LP).  Register α here —
    // on every (scene, phase), mirroring `register_alpha_variables` — before
    // loading so the loader binds the cuts.  α carries the same objective
    // coefficient (respecting `scale_alpha` / auto-scale) and is freed from
    // its bootstrap pin by the cut install (`bound_alpha_for_cut`).
    // Auto-scale α from the boundary-cut coefficients — the SAME shared
    // computation SDDPMethod uses (`boundary_cut_scale_alpha`): average each
    // state column over all cut rows and round the max magnitude up to the
    // next power of ten.  Keeping a single entry point means α is scaled
    // identically regardless of solver method.
    const double scale_alpha =
        boundary_cut_scale_alpha(planning_lp,
                                 boundary_cuts_file,
                                 planning_lp.options().sddp_scale_alpha());
    bc_opts.scale_alpha = scale_alpha;
    SPDLOG_INFO("MonolithicMethod: boundary-cut scale_alpha {}", scale_alpha);
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      // cut_sharing stays `none` (monolithic does no intermediate-phase
      // sharing — it is one big LP); `boundary_cut_sharing` controls only
      // the TERMINAL α layout, so multicut lays down N terminal varphi_s
      // for the boundary-cut loader to route per source scenario.
      register_alpha_variables(planning_lp,
                               scene_index,
                               scale_alpha,
                               CutSharingMode::none,
                               boundary_cut_sharing);
    }

    // Build per-scene phase state info (alpha columns + outgoing links)
    const auto num_phases_bc = planning_lp.systems().empty()
        ? 0UZ
        : planning_lp.systems().front().size();

    // Freeze the base-row count on every (scene, phase) BEFORE installing the
    // boundary cuts below — mirroring SDDPMethod (sddp_method.cpp:509).  This
    // flips `is_cut_phase` (m_base_numrows_set_) true so each boundary-cut row
    // routes through `LinearInterface::compose_physical` (× col_scale,
    // ÷ scale_objective) instead of being emitted raw via `add_row_raw`.
    // Without it the monolithic boundary cuts installed UNSCALED at
    // scale_objective ≠ 1: α and the cut RHS stayed physical while the
    // objective had already been divided by scale_objective, leaving α
    // under-weighted by scale_objective — which inflated the reservoir
    // water-value duals (50–315×) and forced spurious water hoarding.
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index :
           iota_range<PhaseIndex>(0, static_cast<Index>(num_phases_bc)))
      {
        planning_lp.system(scene_index, phase_index)
            .linear_interface()
            .save_base_numrows();
      }
    }
    StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, PhaseStateInfo>>
        scene_phase_states;
    scene_phase_states.resize(
        static_cast<std::size_t>(num_scenes),
        StrongIndexVector<PhaseIndex, PhaseStateInfo>(num_phases_bc));

    const LabelMaker label_maker {};

    auto bc_result = load_boundary_cuts_csv(planning_lp,
                                            boundary_cuts_file,
                                            bc_opts,
                                            label_maker,
                                            scene_phase_states);
    if (bc_result) {
      SPDLOG_INFO(
          "MonolithicMethod: loaded {} boundary cuts (max iteration {})",
          bc_result->count,
          bc_result->max_iteration);

      // ── α-rebase restitution (mean-shift) ──
      // When `boundary_cuts_mean_shift` is on (default), the loader
      // subtracts the per-scene offset `c` (the cut value at each
      // reservoir's efin target — see TASK 2) from every cut RHS and
      // returns it in `alpha_offsets_per_scene`.  The substitution α =
      // α' + c shifts the LP's reported objective by −c, so we add `c`
      // back via `add_obj_constant` on each scene's last-phase
      // LinearInterface — exactly the Python oracle's `obj_constant`
      // restitution (`build_fcf_alpha_terms`).  This is now unified with
      // SDDP: `add_obj_constant` reaches the solver's NATIVE objective
      // offset (see `SolverBackend::set_obj_offset`), and `SDDPMethod`
      // performs the mirror fold into each scene's FIRST-phase master LP.
      // In monolithic there is one LP per scene, so the last phase IS the
      // whole LP.  Offsets are zero when the shift is disabled, making this
      // a no-op then.
      const auto& offsets = bc_result->alpha_offsets_per_scene;
      const auto last_phase = planning_lp.simulation().last_phase_index();
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        const auto si = static_cast<std::size_t>(scene_index);
        const double c = (si < offsets.size()) ? offsets[scene_index] : 0.0;
        if (c != 0.0) {
          planning_lp.system(scene_index, last_phase)
              .linear_interface()
              .add_obj_constant(c);
        }
      }

      // ── α finite box (GPU-clean) ──
      // The boundary-cut install released the last-phase α from its
      // bootstrap pin and floored it (`bound_alpha_for_cut`), but left the
      // UPPER bound at +∞ — α is the one remaining FREE column in the
      // assembled monolithic LP, which GPU first-order / heuristic backends
      // reject (cuOpt PDLP / feasibility-jump cannot project a free column;
      // it trips the "Free variable found" presolve warning).  α is SIGNED
      // (cost-to-go relative to the mean-shifted expected final state), so
      // it needs a finite BOX `[floor, ceil]`, not a one-sided floor.  Now
      // that the FULL boundary-cut set is installed, re-bound each scene's
      // last-phase α with `bound_above=true`: the floor and ceiling are both
      // derived from the cut's SIGNED subgradients × each referenced STATE
      // VARIABLE's signed [min,max] range — any state column (reservoir
      // efin, battery energy, …), not reservoirs only — via signed interval
      // arithmetic, in the rebased-physical frame (`cut.lowb` carries the
      // mean-shift `c`) and divided by `scale_alpha` by the physical bound
      // setters.  Only safe here — NOT in the SDDP per-cut path, where a
      // later cut can legitimately raise α above an early-cut-derived
      // ceiling.
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        apply_alpha_floor(planning_lp,
                          scene_index,
                          last_phase,
                          SystemKind::forward,
                          /*bound_above=*/true);
      }
    } else {
      SPDLOG_WARN("MonolithicMethod: failed to load boundary cuts: {}",
                  bc_result.error().message);
    }
  }

  auto effective_opts = opts;

  std::atomic<int> scenes_done {0};
  std::mutex times_mutex;
  std::vector<double> scene_times(num_scenes, 0.0);

  // The pool is created AFTER every local the scene tasks capture by
  // reference (effective_opts, scenes_done, times_mutex, scene_times):
  // on any exception / early-error unwinding path the pool destructor
  // then joins its still-running workers BEFORE those locals are
  // destroyed (reverse declaration order).  With the pool declared
  // first (the previous layout), a scene failing while its siblings
  // were mid-solve destroyed `scene_times` et al. under the running
  // tasks — a use-after-free.
  auto pool = make_solver_work_pool(
      /*cpu_factor=*/2.0,
      /*cpu_threshold_override=*/0.0,
      /*scheduler_interval=*/std::chrono::milliseconds(50),
      /*memory_limit_mb=*/planning_lp.options().sddp_pool_memory_limit_mb(),
      /*pool_label=*/"MonolithicMethod solve pool");

  SolverMonitor monitor(api_update_interval);
  if (enable_api && !api_status_file.empty()) {
    monitor.start(*pool, solve_start, "MonolithicMonitor");
  }

  try {
    using future_t = std::future<std::expected<void, Error>>;

    LpDebugWriter lp_writer(lp_debug ? lp_debug_directory : std::string {},
                            lp_debug_compression,
                            pool.get());
    if (lp_writer.is_active()) {
      std::filesystem::create_directories(lp_debug_directory);
      const auto lp_stem =
          (std::filesystem::path(lp_debug_directory) / "gtopt_lp").string();
      for (const auto& phase_systems : planning_lp.systems()) {
        for (const auto& system : phase_systems) {
          const auto su = system.scene().uid();
          const auto pu = system.phase().uid();
          if (!in_lp_debug_range(su,
                                 pu,
                                 lp_debug_scene_min,
                                 lp_debug_scene_max,
                                 lp_debug_phase_min,
                                 lp_debug_phase_max))
          {
            continue;
          }
          if (auto lp_result = system.write_lp(lp_stem)) {
            lp_writer.compress_async(*lp_result);
          } else {
            spdlog::warn("{}", lp_result.error().message);
          }
        }
      }
      spdlog::info(
          "MonolithicMethod: wrote LP debug file(s) to {}_*.lp{}",
          lp_stem,
          (lp_debug_compression.empty() || lp_debug_compression == "none"
           || lp_debug_compression == "uncompressed")
              ? ""
              : " (compressing async)");
    }

    std::vector<future_t> futures;
    futures.reserve(planning_lp.systems().size());

    for (auto&& [scene_index, phase_systems] :
         enumerate<SceneIndex>(planning_lp.systems()))
    {
      auto result = pool->submit(
          [&, scene_index]
          {
            SPDLOG_TRACE("MonolithicMethod: scene {} starting", scene_index);
            const auto t_scene = std::chrono::steady_clock::now();
            auto r = planning_lp.resolve_scene_phases(
                scene_index, phase_systems, effective_opts);
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now()
                                              - t_scene)
                    .count();
            {
              const std::scoped_lock lk(times_mutex);
              scene_times[scene_index] = elapsed;
            }
            ++scenes_done;
            SPDLOG_INFO("MonolithicMethod: scene {} done in {:.3f}s ({}/{})",
                        scene_index,
                        elapsed,
                        scenes_done.load(),
                        num_scenes);
            return r;
          });
      if (result.has_value()) {
        futures.push_back(std::move(*result));
      } else {
        return std::unexpected(Error {
            .code = ErrorCode::InternalError,
            .message = std::format("Failed to submit scene {} to work pool",
                                   scene_index),
        });
      }
    }

    // Join ALL scene futures BEFORE acting on any failure: an early
    // return while sibling scene tasks are still running would start
    // destroying this frame ahead of the pool's worker join.  The pool
    // now outlives the captured locals (see its declaration above), but
    // draining first also keeps the error deterministic — the FIRST
    // failed scene wins regardless of completion order.
    std::optional<Error> first_error;
    for (auto& future : futures) {
      try {
        if (auto result = future.get(); !result && !first_error.has_value()) {
          first_error = std::move(result.error());
        }
      } catch (const std::exception& e) {
        if (!first_error.has_value()) {
          first_error = Error {
              .code = ErrorCode::InternalError,
              .message = std::format("scene solve task threw: {}", e.what()),
          };
        }
      }
    }
    if (first_error.has_value()) {
      monitor.stop();
      return std::unexpected(std::move(*first_error));
    }

    // ── Kappa threshold checking ──
    {
      const auto& sim = planning_lp.planning().simulation;
      const auto kappa_mode =
          sim.kappa_warning.value_or(KappaWarningMode::warn);
      if (kappa_mode != KappaWarningMode::none) {
        constexpr double default_kappa_threshold = 1e9;
        const double threshold =
            sim.kappa_threshold.value_or(default_kappa_threshold);
        double max_kappa = -1.0;
        for (auto&& [si, phase_systems_ref] :
             enumerate<SceneIndex>(planning_lp.systems()))
        {
          for (auto&& [pi, system] : enumerate<PhaseIndex>(phase_systems_ref)) {
            const auto& li = system.linear_interface();
            const auto kappa_opt = li.get_kappa();
            if (!kappa_opt.has_value()) {
              continue;  // backend reported unavailable — skip entirely
            }
            const double kappa = *kappa_opt;
            max_kappa = std::max(max_kappa, kappa);
            if (kappa > threshold) {
              spdlog::warn(
                  "High kappa {:.2e} (threshold {:.2e}) at scene {} phase {}",
                  kappa,
                  threshold,
                  system.scene().uid(),
                  system.phase().uid());
              if (kappa_mode == KappaWarningMode::save_lp
                  && !lp_debug_directory.empty())
              {
                std::filesystem::create_directories(lp_debug_directory);
                const auto stem =
                    (std::filesystem::path(lp_debug_directory)
                     / as_label(
                         "kappa", system.scene().uid(), system.phase().uid()))
                        .string();
                if (auto r = li.write_lp(stem)) {
                  spdlog::warn("Saved high-kappa LP to {}.lp", stem);
                }
              }
            }
          }
        }
        planning_lp.set_sddp_summary({
            .max_kappa = max_kappa,
        });
      }
    }

    // Drain LP compression tasks (run in parallel with solve)
    lp_writer.drain();

    // ── Write monitoring status file ──
    monitor.stop();
    {
      const double total_elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - solve_start)
              .count();
      SPDLOG_INFO("MonolithicMethod: all {} scene(s) done in {:.3f}s",
                  num_scenes,
                  total_elapsed);
    }
    if (enable_api && !api_status_file.empty()) {
      const double elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - solve_start)
                                 .count();

      std::string json;
      json.reserve(1024);
      json += "{\n";
      json += std::format("  \"version\": 1,\n");
      json += std::format("  \"status\": \"done\",\n");
      json += std::format("  \"method\": \"monolithic\",\n");
      // Query the actual solver identity from the first available LP
      if (!planning_lp.systems().empty()
          && !planning_lp.systems().front().empty())
      {
        const auto& li =
            planning_lp.systems().front().front().linear_interface();
        json += std::format("  \"solver\": \"{}\",\n", li.solver_id());
      }
      json += std::format("  \"elapsed_s\": {:.3f},\n", elapsed);
      json += std::format("  \"total_scenes\": {},\n", num_scenes);
      json += std::format("  \"scenes_done\": {},\n", scenes_done.load());

      json += "  \"scene_times\": [";
      {
        const std::scoped_lock lk(times_mutex);
        for (const auto& [i, t] : enumerate(scene_times)) {
          if (i > 0) {
            json += ", ";
          }
          json += std::format("{:.4f}", t);
        }
      }
      json += "],\n";

      monitor.append_history_json(json);
      json += "}\n";

      SolverMonitor::write_status(json, api_status_file);
    }

    return static_cast<int>(futures.size());

  } catch (const std::exception& e) {
    monitor.stop();
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message = std::format("Unexpected error in resolve: {}", e.what()),
    });
  }
}

}  // namespace gtopt
