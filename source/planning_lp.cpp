/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_set>

#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ── Line reactance validation ────────────────────────────────────────────

void PlanningLP::validate_line_reactance(Planning& planning)
{
  // Real transmission lines have reactance X ≳ 1e-4 p.u.  Anything smaller
  // is almost always a data-entry mistake (stray "0.0001" becoming 1e-7,
  // missing unit conversion, zero-copy from an ideal-transformer model) and
  // produces a Kirchhoff coefficient x_tau = X/V² orders of magnitude
  // below the rest of the network.  Rather than poison LP conditioning, we
  // warn and rewrite the reactance schedule to scalar 0.0 — line_lp.cpp
  // then skips the theta constraint for that line (DC/HVDC branch).
  //
  // Mirrors the plp2gtopt battery_writer clamping of input/output_efficiency.
  constexpr double kReactanceMin = 1e-4;

  auto& sys = planning.system;
  size_t clamped = 0;

  for (auto& line : sys.line_array) {
    if (!line.reactance.has_value()) {
      continue;
    }
    auto& sched = *line.reactance;

    // Scalar schedule.
    if (std::holds_alternative<double>(sched)) {
      const double x = std::get<double>(sched);
      if (x != 0.0 && std::abs(x) < kReactanceMin) {
        spdlog::warn(
            "(line_reactance) Line '{}' (uid={}): reactance X={:.3e} is "
            "below {:.0e} — clamping to 0 (line will be treated as "
            "DC/HVDC, no Kirchhoff constraint).",
            line.name,
            line.uid,
            x,
            kReactanceMin);
        sched = 0.0;
        ++clamped;
      }
      continue;
    }

    // Vector schedule: clamp any offending entry in place.
    if (std::holds_alternative<std::vector<double>>(sched)) {
      auto& vec = std::get<std::vector<double>>(sched);
      bool any_small = false;
      double min_bad = std::numeric_limits<double>::infinity();
      for (auto& x : vec) {
        if (x != 0.0 && std::abs(x) < kReactanceMin) {
          any_small = true;
          min_bad = std::min(min_bad, std::abs(x));
          x = 0.0;
        }
      }
      if (any_small) {
        spdlog::warn(
            "(line_reactance) Line '{}' (uid={}): reactance schedule has "
            "entries below {:.0e} (min={:.3e}) — clamped to 0 (DC/HVDC "
            "for those stages).",
            line.name,
            line.uid,
            kReactanceMin,
            min_bad);
        ++clamped;
      }
      continue;
    }

    // FileSched — can't validate statically; leave alone.
  }

  if (clamped > 0) {
    spdlog::info(
        "  Line reactance validation: clamped {} line(s) with X below "
        "{:.0e} to zero.",
        clamped,
        kReactanceMin);
  }
}

// ── Adaptive scale_theta computation ──────────────────────────────────────

void PlanningLP::auto_scale_theta(Planning& planning)
{
  // Only compute when scale_theta is not explicitly set at any level.
  auto& opts = planning.options;
  if (opts.scale_theta.has_value()
      || opts.model_options.scale_theta.has_value())
  {
    return;
  }

  // Don't auto-scale when Kirchhoff is disabled or single-bus mode.
  const auto& mo = opts.model_options;
  if ((opts.use_kirchhoff.has_value() && !*opts.use_kirchhoff)
      || (mo.use_kirchhoff.has_value() && !*mo.use_kirchhoff))
  {
    return;
  }
  if ((opts.use_single_bus.has_value() && *opts.use_single_bus)
      || (mo.use_single_bus.has_value() && *mo.use_single_bus))
  {
    return;
  }

  // Collect scalar x_tau = X / V² values from all lines.
  //
  // The Kirchhoff row emits theta_coeff = 1/|x_tau| (see line_lp.cpp:65-88).
  // The theta column is then multiplied by scale_theta in flatten() (see
  // linear_problem.cpp:358), so the assembled theta coefficient is
  // scale_theta / |x_tau|. To drive this to ~1 for the median line — which
  // makes row-max equilibration produce a near-identity Kirchhoff row — we
  // must pick scale_theta = median(|x_tau|) = median(X/V²).
  //
  // Collecting median(X) directly (the previous heuristic) is wrong because
  // it is computed in a space orthogonal to the true susceptance spread on
  // a mixed-voltage network: high-voltage lines (500 kV) have tiny x_tau
  // but arbitrary raw X, and the median picks the raw-X middle, not the
  // x_tau middle, yielding assembled coefficients ~1e7 instead of ~1.
  const auto sched_to_scalar = [](const auto& sched) -> double
  {
    if (std::holds_alternative<double>(sched)) {
      return std::get<double>(sched);
    }
    if (std::holds_alternative<std::vector<double>>(sched)) {
      const auto& vec = std::get<std::vector<double>>(sched);
      if (!vec.empty()) {
        return vec.front();
      }
    }
    return 0.0;
  };

  std::vector<double> x_taus;
  for (const auto& line : planning.system.line_array) {
    if (!line.reactance.has_value()) {
      continue;
    }
    const double x = sched_to_scalar(*line.reactance);
    if (x <= 0.0) {
      continue;
    }
    // Voltage defaults to 1.0 (per-unit mode) when omitted — matches the
    // convention in line_lp.cpp:62 and include/gtopt/line.hpp:63-65.
    double v = 1.0;
    if (line.voltage.has_value()) {
      const double vs = sched_to_scalar(*line.voltage);
      if (vs > 0.0) {
        v = vs;
      }
    }
    const double x_tau = x / (v * v);
    if (x_tau > 0.0) {
      x_taus.push_back(x_tau);
    }
  }

  if (x_taus.empty()) {
    return;
  }

  // Compute median x_tau = median(X/V²).
  std::ranges::sort(x_taus);
  const auto n = x_taus.size();
  const double median_x_tau = (n % 2 == 0)
      ? (x_taus[(n / 2) - 1] + x_taus[n / 2]) / 2.0
      : x_taus[n / 2];

  // scale_theta = median(|x_tau|) so that the assembled theta coefficient
  // scale_theta / |x_tau| ≈ 1 for the median line, and row-max
  // equilibration converges to a near-identity Kirchhoff row.
  opts.model_options.scale_theta = median_x_tau;
  spdlog::info(
      "  Auto scale_theta = {:.6g} (median of {} line x_tau = X/V² values)",
      median_x_tau,
      n);

  // P0-3 — unit-consistency validation.
  //
  // The Kirchhoff coefficient x_tau = τ·X/V² must live in a single unit
  // system across the whole network. When a model mixes per-unit lines
  // (V=1) with physical lines (V in kV, X in Ω), the x_tau values span
  // many orders of magnitude and no choice of scale_theta can rescue
  // conditioning. Catch that here, at load time, so the user sees the
  // problem before a spurious LP is solved.
  const double min_x_tau = x_taus.front();
  const double max_x_tau = x_taus.back();
  if (min_x_tau > 0.0) {
    const double spread = max_x_tau / min_x_tau;
    constexpr double kSpreadWarn = 1e4;
    constexpr double kSpreadError = 1e6;
    if (spread > kSpreadError) {
      spdlog::error(
          "  Line x_tau = X/V² spread is {:.3e} (max={:.3e}, min={:.3e}) "
          "across {} lines — this strongly suggests mixed unit systems "
          "(per-unit vs kV/Ω). LP conditioning will be unrecoverable. "
          "Check Line.voltage and Line.reactance fields.",
          spread,
          max_x_tau,
          min_x_tau,
          n);
    } else if (spread > kSpreadWarn) {
      spdlog::warn(
          "  Line x_tau = X/V² spread is {:.3e} (max={:.3e}, min={:.3e}) "
          "across {} lines — this may indicate inconsistent unit systems "
          "and will cap LP conditioning quality. Check Line.voltage and "
          "Line.reactance fields.",
          spread,
          max_x_tau,
          min_x_tau,
          n);
    }
  }
}

// ── Adaptive reservoir energy scaling ────────────────────────────────────

void PlanningLP::auto_scale_reservoirs(Planning& planning)
{
  auto& opts = planning.options;
  auto& sys = planning.system;

  // Build a set of UIDs already covered by explicit variable_scales entries.
  auto has_entry = [&](Uid uid) -> bool
  {
    return std::ranges::any_of(opts.variable_scales,
                               [uid](const VariableScale& vs)
                               {
                                 return vs.class_name == "Reservoir"
                                     && vs.variable == "energy"
                                     && vs.uid == uid;
                               });
  };

  // Helper: extract a representative scalar from a FieldSched optional.
  auto scalar_of = [](const OptTRealFieldSched& fs) -> std::optional<double>
  {
    if (!fs.has_value()) {
      return std::nullopt;
    }
    if (std::holds_alternative<double>(*fs)) {
      return std::get<double>(*fs);
    }
    if (std::holds_alternative<std::vector<Real>>(*fs)) {
      const auto& vec = std::get<std::vector<Real>>(*fs);
      if (!vec.empty()) {
        return *std::ranges::max_element(vec);
      }
    }
    return std::nullopt;  // FileSched — can't resolve statically
  };

  size_t count = 0;
  for (const auto& rsv : sys.reservoir_array) {
    if (has_entry(rsv.uid)) {
      continue;
    }
    const auto emax = scalar_of(rsv.emax);
    if (!emax.has_value() || *emax <= 1000.0) {
      continue;
    }
    // 10^ceil(log10(emax / 1000)) — round up to next power of 10.
    const double raw = *emax / 1000.0;
    const double energy_scale = std::pow(10.0, std::ceil(std::log10(raw)));
    const double flow_scale = energy_scale / 1000.0;

    opts.variable_scales.push_back(VariableScale {
        .class_name = "Reservoir",
        .variable = "energy",
        .uid = rsv.uid,
        .scale = energy_scale,
        .name = rsv.name,
    });
    opts.variable_scales.push_back(VariableScale {
        .class_name = "Reservoir",
        .variable = "flow",
        .uid = rsv.uid,
        .scale = flow_scale,
        .name = rsv.name,
    });
    ++count;
  }

  if (count > 0) {
    spdlog::info(
        "  Auto scale_reservoir: computed energy/flow scales for {} reservoirs",
        count);
  }
}

void PlanningLP::tighten_scene_phase_links(phase_systems_t& phase_systems,
                                           SimulationLP& simulation)
{
  for (auto& sys : phase_systems) {
    auto& links = sys.pending_state_links();
    for (const auto& link : links) {
      auto prev_var = simulation.state_variable(link.prev_key);
      if (prev_var) {
        prev_var->get().add_dependent_variable(link.here_key, link.here_col);
      } else {
        // Producer-side StateVariable was never registered: the previous
        // phase's element either didn't run or chose not to publish an
        // efin/state column for this (scenario, stage).  Mirrors the
        // build-time warning that lived in storage_lp.hpp before the
        // deferred-linking refactor — kept here so cross-phase coupling
        // gaps remain visible.
        SPDLOG_WARN(
            "tighten_scene_phase_links: no producer StateVariable for "
            "deferred link (class='{}' col='{}' uid={} prev_stage_uid={} "
            "scene={} prev_phase={}). Cross-phase state coupling will be "
            "missing for this element.",
            link.prev_key.class_name,
            link.prev_key.col_name,
            link.prev_key.uid,
            link.prev_key.stage_uid,
            link.prev_key.lp_key.scene_index,
            link.prev_key.lp_key.phase_index);
      }
    }
    links.clear();
    links.shrink_to_fit();
  }
}

auto PlanningLP::create_systems(System& system,
                                SimulationLP& simulation,
                                const PlanningOptionsLP& options,
                                const LpMatrixOptions& flat_opts)
    -> scene_phase_systems_t
{
  system.expand_batteries();
  system.expand_reservoir_constraints();
  system.setup_reference_bus(options);

  // Enable per-cell AMPL variable registration when user constraints
  // exist (or any future consumer that calls find_ampl_col).  Without
  // user constraints, the map stays empty and add_ampl_variable is a
  // no-op — saving allocation/hashing overhead.
  if (!system.user_constraint_array.empty()) {
    simulation.set_need_ampl_variables(true);
  }

  // Note: AMPL element-name and compound registries are populated by
  // SystemLP's constructor under std::call_once on
  // SimulationLP::ampl_registry_flag(), so the registry is filled
  // exactly once regardless of whether construction goes through
  // PlanningLP or directly via tests.

  auto&& scenes = simulation.scenes();
  auto&& phases = simulation.phases();

  const auto num_scenes = static_cast<int>(scenes.size());
  const auto num_phases = static_cast<int>(phases.size());

  // Pre-resolve the solver name on the main thread so worker threads
  // don't race through the plugin registry in parallel.  This triggers
  // the "Loaded solver plugin" log before the "Building LP" banner so
  // the output order reflects setup → pool → build.
  auto resolved_opts = flat_opts;
  if (resolved_opts.solver_name.empty()) {
    resolved_opts.solver_name =
        std::string(SolverRegistry::instance().default_solver());
  }

  // Propagate low-memory hint from planning options into flat_opts so
  // SystemLP::create_lp can branch on it.  SDDP and cascade methods both
  // honor `sddp_options.low_memory_mode`; the monolithic method leaves
  // it at `off`.  When non-`off`, SystemLP defers the initial load_flat
  // and the backend is reconstructed lazily on first use, saving one
  // full backend population per (scene, phase).
  if (resolved_opts.low_memory_mode == LowMemoryMode::off) {
    const auto method = options.method_type_enum();
    if (method == MethodType::sddp || method == MethodType::cascade) {
      resolved_opts.low_memory_mode = options.sddp_low_memory();
      resolved_opts.memory_codec = options.sddp_memory_codec();
    }
  }

  PlanningLP::scene_phase_systems_t all_systems(scenes.size());

  // `BuildMode` selects the granularity of LP assembly parallelism:
  //   - `serial`:         no work pool; build every (scene, phase) cell
  //                        in the calling thread.  Matches a genuine
  //                        pre-parallel baseline — no pool submit, no
  //                        build buffer, no move/merge overhead.
  //                        Ignores `--cpu-factor`.
  //   - `scene_parallel`: one pool task per scene; each task builds its
  //                        scene's phases sequentially into its own
  //                        `phase_systems_t` and runs
  //                        `tighten_scene_phase_links` before returning.
  //                        Coarser granularity, lower pool-submit and
  //                        malloc-arena contention — the pre-00c605d7
  //                        default and current default.
  //   - `full_parallel`:  one pool task per (scene × phase) cell,
  //                        materialised into a `build_buf` and moved
  //                        into `all_systems` during a sequential merge
  //                        pass.  Maximum concurrency, highest per-cell
  //                        overhead.  The custom SystemLP move-ctor
  //                        re-points the embedded SystemContext back-
  //                        reference and rebuilds its
  //                        `m_collection_ptrs_` interior-pointer table
  //                        to the new owner, so post-move SystemLPs are
  //                        valid even though the build buffer holds
  //                        them at a different address.
  //
  // Cross-phase state-variable links are queued (`PendingStateLink`)
  // inside each SystemLP and resolved by `tighten_scene_phase_links`.
  // That routine only touches state variables for a given scene, so it
  // is safe to run in-thread under `scene_parallel` and `serial`, and
  // in a sequential merge pass under `full_parallel`.
  //
  // Honors the CLI `--cpu-factor` flag (routed through
  // `sddp_options.pool_cpu_factor`) for both parallel modes.
  const auto build_mode = options.build_mode_enum();
  const auto build_cpu_factor = options.build_pool_cpu_factor();

  SPDLOG_INFO("  Building LP: {} scene(s) × {} phase(s) [mode={}]",
              num_scenes,
              num_phases,
              enum_name(build_mode));

  const auto build_start = std::chrono::steady_clock::now();

  // Parallelism instrumentation: track peak concurrent cell-builders,
  // total per-cell CPU time, and the set of worker thread IDs that
  // actually ran cells.  After the join, we log
  //   parallelism = total_cell_cpu / wall_time
  // so the user can see at a glance whether the pool is truly running
  // tasks concurrently or whether some hidden serialization (global
  // mutex, plugin loader, etc.) is forcing cells through one at a time.
  // Under `serial` the peak stays at 1 and `worker_threads` reports the
  // single calling thread — which is the whole point of that mode.
  std::atomic<int> active_cells {0};
  std::atomic<int> peak_active {0};
  std::atomic<std::int64_t> total_cell_us {0};
  std::atomic<std::size_t> cells_done {0};
  std::mutex tid_mutex;
  std::unordered_set<std::size_t> worker_tids;

  // Shared per-cell timing helper used by all three build modes so the
  // summary log stays consistent regardless of granularity.  Callers
  // pass an `emplace_fn` that performs the actual SystemLP
  // construction into whatever storage slot this mode uses (build_buf
  // cell for `full_parallel`, `phase_systems_t::emplace_back` for the
  // other two).
  auto build_cell = [&]([[maybe_unused]] const auto& scene_ref,
                        [[maybe_unused]] const auto& phase_ref,
                        auto&& emplace_fn)
  {
    const auto cell_start = std::chrono::steady_clock::now();
    const auto cur = active_cells.fetch_add(1, std::memory_order_relaxed) + 1;
    // Monotonically raise peak_active to max(peak_active, cur).
    int prev_peak = peak_active.load(std::memory_order_relaxed);
    while (cur > prev_peak
           && !peak_active.compare_exchange_weak(
               prev_peak, cur, std::memory_order_relaxed))
    {}

    const auto tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
    {
      const std::scoped_lock lock(tid_mutex);
      worker_tids.insert(tid);
    }

    SPDLOG_TRACE("    Building LP scene_uid={} phase_uid={} tid={}",
                 scene_ref.uid(),
                 phase_ref.uid(),
                 tid);
    emplace_fn();

    const auto cell_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - cell_start)
            .count();
    total_cell_us.fetch_add(cell_elapsed, std::memory_order_relaxed);
    active_cells.fetch_sub(1, std::memory_order_relaxed);
    const auto done = cells_done.fetch_add(1, std::memory_order_relaxed) + 1;
    // First-cell marker: proves at least one cell has been built — in
    // the parallel modes, that at least one pool task has started
    // executing before the join loop blocks; in serial mode, that the
    // in-thread loop has made forward progress.
    if (done == 1) {
      SPDLOG_INFO("    First LP cell built on worker tid={} ({:.3f}s)",
                  tid,
                  static_cast<double>(cell_elapsed) / 1e6);
    }
  };

  if (build_mode == BuildMode::serial) {
    // ── Serial path ────────────────────────────────────────────────
    // No pool, no build_buf, no futures.  Directly emplace every cell
    // into its final `phase_systems_t` in the calling thread.  This is
    // the honest serial baseline — `--cpu-factor` is ignored.
    for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
      auto& phase_systems = all_systems[scene_index];
      phase_systems.reserve(phases.size());
      for (const auto& phase : phases) {
        build_cell(scene,
                   phase,
                   [&]
                   {
                     phase_systems.emplace_back(
                         system, simulation, phase, scene, resolved_opts);
                   });
      }
      tighten_scene_phase_links(phase_systems, simulation);
    }
  } else if (build_mode == BuildMode::direct_parallel) {
    // ── Direct-parallel path ─────────────────────────────────────────
    // One jthread per hardware thread; cells are distributed via an
    // atomic counter.  No WorkPool, no scheduling mutex, no CPU/memory
    // monitoring — pure work-stealing with minimal synchronization.
    // Useful for benchmarking whether WorkPool overhead or malloc arena
    // contention is the dominant cost at high thread counts.
    using build_row_t = StrongIndexVector<PhaseIndex, std::optional<SystemLP>>;
    StrongIndexVector<SceneIndex, build_row_t> build_buf(scenes.size());
    for (auto& row : build_buf) {
      row.resize(phases.size());
    }

    const auto total_cells = scenes.size() * phases.size();
    std::atomic<std::size_t> next_cell {0};

    const auto n_threads =
        std::min(static_cast<std::size_t>(std::thread::hardware_concurrency()),
                 total_cells);

    std::vector<std::jthread> workers;
    workers.reserve(n_threads);
    for (std::size_t t = 0; t < n_threads; ++t) {
      workers.emplace_back(
          [&]
          {
            while (true) {
              const auto cell_id =
                  next_cell.fetch_add(1, std::memory_order_relaxed);
              if (cell_id >= total_cells) {
                break;
              }
              const auto si = SceneIndex {
                  static_cast<int>(cell_id / phases.size()),
              };
              const auto pi = PhaseIndex {
                  static_cast<int>(cell_id % phases.size()),
              };
              const auto& scene = scenes[si];
              const auto& phase = phases[pi];
              build_cell(scene,
                         phase,
                         [&]
                         {
                           build_buf[si][pi].emplace(
                               system, simulation, phase, scene, resolved_opts);
                         });
            }
          });
    }
    // jthread destructor joins automatically.
    workers.clear();

    // Sequential merge + cross-phase link resolution.
    for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
      PlanningLP::phase_systems_t phase_systems;
      phase_systems.reserve(phases.size());
      for (auto& slot : build_buf[scene_index]) {
        phase_systems.emplace_back(std::move(slot).value());
      }
      tighten_scene_phase_links(phase_systems, simulation);
      all_systems[scene_index] = std::move(phase_systems);
    }
  } else {
    // Both WorkPool modes share the pool allocation.
    auto pool = make_solver_work_pool(build_cpu_factor);

    if (build_mode == BuildMode::scene_parallel) {
      // ── Scene-parallel path (pre-00c605d7 behavior, current default) ──
      // One pool task per scene; each task builds its phases in order
      // into its own `phase_systems_t` and resolves cross-phase links
      // inside the worker — no shared build buffer, no post-merge
      // pass.  Writes go directly into `all_systems[scene_index]`,
      // which is safe because every task owns a distinct slot of the
      // pre-sized outer vector.
      std::vector<std::future<void>> futures;
      futures.reserve(scenes.size());
      for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
        // `scene` rebinds on each iteration; capture by pointer so the
        // lambda sees its own stable reference.
        const auto* const scene_ptr = &scene;
        auto result = pool->submit(
            [&, scene_index, scene_ptr]
            {
              auto& phase_systems = all_systems[scene_index];
              phase_systems.reserve(phases.size());
              for (const auto& phase : phases) {
                build_cell(
                    *scene_ptr,
                    phase,
                    [&]
                    {
                      phase_systems.emplace_back(
                          system, simulation, phase, *scene_ptr, resolved_opts);
                    });
              }
              tighten_scene_phase_links(phase_systems, simulation);
            });
        if (!result.has_value()) {
          throw std::runtime_error(std::format(
              "Failed to submit scene {} to work pool", scene_index));
        }
        futures.push_back(std::move(*result));
      }
      SPDLOG_INFO(
          "  Submitted {} scene tasks to work pool, waiting for workers...",
          futures.size());
      for (auto& fut : futures) {
        fut.get();
      }
    } else {
      // ── Full-parallel path (post-00c605d7, opt-in) ────────────────
      // One pool task per (scene, phase) cell.  Each task builds into a
      // pre-allocated slot in `build_buf`; a sequential merge pass then
      // moves cells into their final `phase_systems_t` and runs
      // `tighten_scene_phase_links` per scene.
      using build_row_t =
          StrongIndexVector<PhaseIndex, std::optional<SystemLP>>;
      StrongIndexVector<SceneIndex, build_row_t> build_buf(scenes.size());
      for (auto& row : build_buf) {
        row.resize(phases.size());
      }

      std::vector<std::future<void>> futures;
      futures.reserve(scenes.size() * phases.size());
      for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
        for (auto&& [phase_index, phase] : enumerate<PhaseIndex>(phases)) {
          const auto* const phase_ptr = &phase;
          const auto* const scene_ptr = &scene;
          auto result = pool->submit(
              [&, scene_index, phase_index, phase_ptr, scene_ptr]
              {
                build_cell(*scene_ptr,
                           *phase_ptr,
                           [&]
                           {
                             build_buf[scene_index][phase_index].emplace(
                                 system,
                                 simulation,
                                 *phase_ptr,
                                 *scene_ptr,
                                 resolved_opts);
                           });
              });
          if (!result.has_value()) {
            throw std::runtime_error(
                std::format("Failed to submit scene {} phase {} to work pool",
                            scene_index,
                            phase_index));
          }
          futures.push_back(std::move(*result));
        }
      }
      SPDLOG_INFO(
          "  Submitted {} LP cells to work pool, waiting for workers...",
          futures.size());
      for (auto& fut : futures) {
        fut.get();
      }

      // Sequentially merge cells into per-scene phase_systems_t and
      // resolve deferred cross-phase state-variable links.
      for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
        PlanningLP::phase_systems_t phase_systems;
        phase_systems.reserve(phases.size());
        for (auto& slot : build_buf[scene_index]) {
          phase_systems.emplace_back(std::move(slot).value());
        }
        tighten_scene_phase_links(phase_systems, simulation);
        all_systems[scene_index] = std::move(phase_systems);
      }
    }
  }

  // After all add_to_lp calls, dispatch a single initial update_lp pass
  // so that volume-dependent LP elements are set from the reservoir eini
  // values before any solver is called.
  //
  // Skipped under low-memory mode: each (scene, phase) backend is
  // currently deferred (no load_flat called yet) and the very first
  // solve pass already calls `dispatch_update_lp` after reconstructing
  // the backend, so this pre-pass would only force a wasted
  // reconstruct_backend → load_flat → set_col_*** roundtrip and then
  // throw the bounds away on the next release.  See
  // sddp_forward_pass.cpp's per-phase reconstruct + dispatch_update_lp.
  if (resolved_opts.low_memory_mode == LowMemoryMode::off) {
    for (auto& phase_systems : all_systems) {
      for (auto& sys : phase_systems) {
        std::ignore = sys.update_lp();
      }
    }
  }

  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - build_start)
                             .count();

  // Parallelism summary.  `parallelism` ≈ 1.0 means cells ran
  // effectively serialized through the pool (contention, global lock,
  // or max_threads≈1).  A value >> 1 means wall time was compressed by
  // concurrent execution.  `peak_parallel` is the highest number of
  // cells observed simultaneously in-flight, and `worker_threads` is
  // the count of distinct OS threads that actually executed a cell.
  const auto total_cpu_s =
      static_cast<double>(total_cell_us.load(std::memory_order_relaxed)) / 1e6;
  const auto n_workers = [&]
  {
    const std::scoped_lock lock(tid_mutex);
    return worker_tids.size();
  }();
  const auto parallelism = elapsed > 0.0 ? (total_cpu_s / elapsed) : 0.0;
  SPDLOG_INFO(
      "  Building LP done in {:.3f}s "
      "(mode={}, cells={}, peak_parallel={}, worker_threads={}, "
      "total_cell_cpu={:.3f}s, parallelism={:.2f}x)",
      elapsed,
      enum_name(build_mode),
      cells_done.load(std::memory_order_relaxed),
      peak_active.load(std::memory_order_relaxed),
      n_workers,
      total_cpu_s,
      parallelism);

  return all_systems;
}

void PlanningLP::write_lp(const std::string& filename) const
{
  for (auto&& phase_systems : m_systems_) {
    for (auto&& system : phase_systems) {
      auto result = system.write_lp(filename);
      if (!result) {
        spdlog::warn("{}", result.error().message);
        return;
      }
    }
  }
}

void PlanningLP::write_out()
{
  const auto num_scenes = static_cast<int>(m_systems_.size());
  const auto num_phases =
      num_scenes > 0 ? static_cast<int>(m_systems_.front().size()) : 0;
  SPDLOG_INFO(
      "  Writing output: {} scene(s) × {} phase(s)", num_scenes, num_phases);

  // Submit one task per (scene, phase) pair to the work pool so that
  // output files are written in parallel.  Tasks use Low priority
  // (non-LP I/O work) to avoid competing with solver tasks.
  auto pool = make_solver_work_pool();

  std::vector<std::future<void>> futures;
  futures.reserve(static_cast<std::size_t>(num_scenes)
                  * static_cast<std::size_t>(num_phases));

  for (auto&& [scene_num, phase_systems] : enumerate<SceneIndex>(m_systems_)) {
    for (auto&& [phase_num, system] : enumerate<PhaseIndex>(phase_systems)) {
      SPDLOG_DEBUG("  Submitting write_out scene {}/{} phase {}/{}",
                   scene_num + 1,
                   num_scenes,
                   phase_num + 1,
                   num_phases);
      // Capture `system` via an explicit pointer into `m_systems_` (a
      // stable member container) rather than `[&system]`.  The structured
      // binding `system` is loop-local and rebound on the next iteration;
      // capturing it by reference would leave the queued task reading a
      // binding that has already moved on to a different (scene, phase).
      // Mirrors the same fix in `create_systems` above.
      auto* const sys_ptr = &system;
      auto result = pool->submit(
          [sys_ptr] { sys_ptr->write_out(); },
          {
              .priority = TaskPriority::Low,
              .name = std::format("write_out_s{}_p{}", scene_num, phase_num),
          });
      if (result.has_value()) {
        futures.push_back(std::move(*result));
      } else {
        // Fall back to synchronous if the pool rejects the task.
        SPDLOG_WARN(
            "Failed to submit write_out task for scene {} phase {},"
            " running synchronously",
            scene_num,
            phase_num);
        system.write_out();
      }
    }
  }

  for (auto& fut : futures) {
    fut.get();
  }

  // ── Write consolidated solution.csv ─────────────────────────────────────
  // All parallel write tasks have completed; now collect solution values
  // from every (scene, phase) system and write solution.csv in scene/phase
  // order.  This avoids any concurrent file access and guarantees a
  // deterministic, sorted output regardless of task completion order.

  struct SolutionRow
  {
    SceneUid scene_uid;
    PhaseUid phase_uid;
    int status;
    double obj_value;
    double kappa;
    double max_kappa;
    double gap;  ///< Final SDDP gap (0.0 for monolithic)
    double
        gap_change;  ///< Final SDDP stationary gap-change (1.0 for monolithic)
  };

  std::vector<SolutionRow> rows;
  rows.reserve(static_cast<std::size_t>(num_scenes)
               * static_cast<std::size_t>(num_phases));

  const auto& sddp = m_sddp_summary_;

  for (const auto& phase_systems : m_systems_) {
    for (const auto& system : phase_systems) {
      const auto& li = system.linear_interface();
      rows.push_back({
          .scene_uid = system.scene().uid(),
          .phase_uid = system.phase().uid(),
          .status = li.get_status(),
          .obj_value = li.get_obj_value_physical(),
          .kappa = li.get_kappa(),
          .max_kappa = sddp.max_kappa,
          .gap = sddp.gap,
          .gap_change = sddp.gap_change,
      });
    }
  }

  // Sort rows by (scene_uid, phase_uid) for a deterministic output order.
  std::ranges::sort(rows,
                    [](const SolutionRow& a, const SolutionRow& b)
                    {
                      if (a.scene_uid != b.scene_uid) {
                        return a.scene_uid < b.scene_uid;
                      }
                      return a.phase_uid < b.phase_uid;
                    });

  const auto out_dir = std::filesystem::path(m_options_.output_directory());
  const auto sol_path = out_dir / "solution.csv";

  std::ofstream sol_file(sol_path.string(), std::ios::out);
  if (!sol_file) [[unlikely]] {
    SPDLOG_CRITICAL("Cannot open solution file '{}' for writing",
                    sol_path.string());
    return;
  }

  // Status names: CLP convention (0=optimal, 1=primal infeasible,
  // 2=dual infeasible/unbounded, 3=iteration limit, 4=error, 5=not solved)
  static constexpr auto status_name = [](int s) constexpr -> std::string_view
  {
    switch (s) {
      case 0:
        return "optimal";
      case 1:
        return "infeasible";
      case 2:
        return "unbounded";
      case 3:
        return "iteration_limit";
      case 4:
        return "error";
      default:
        return "unknown";
    }
  };

  sol_file << "scene,phase,status,status_name,obj_value,kappa,max_kappa,"
              "gap,gap_change\n";
  for (const auto& row : rows) {
    sol_file << std::format("{},{},{},{},{},{},{},{},{}\n",
                            row.scene_uid,
                            row.phase_uid,
                            row.status,
                            status_name(row.status),
                            row.obj_value,
                            row.kappa,
                            row.max_kappa,
                            row.gap,
                            row.gap_change);
    SPDLOG_DEBUG(
        "  solution.csv: scene={} phase={} status={} obj_value={} "
        "kappa={} max_kappa={} gap={} gap_change={}",
        row.scene_uid,
        row.phase_uid,
        row.status,
        row.obj_value,
        row.kappa,
        row.max_kappa,
        row.gap,
        row.gap_change);
  }
}

std::expected<void, Error> PlanningLP::resolve_scene_phases(
    SceneIndex scene_index,
    phase_systems_t& phase_systems,
    const SolverOptions& lp_opts)
{
  [[maybe_unused]] const auto num_phases =
      static_cast<int>(phase_systems.size());
  SPDLOG_DEBUG("  Solving scene {} ({} phase(s))", scene_index, num_phases);

  // Configure per-scene/phase solver log files when detailed mode is active
  const auto log_mode = lp_opts.log_mode.value_or(SolverLogMode::nolog);
  const auto log_dir = m_options_.log_directory();

  for (auto&& [phase_index, system_sp] : enumerate<PhaseIndex>(phase_systems)) {
    SPDLOG_TRACE("    Solving scene {} phase {} ({} cols, {} rows)",
                 scene_index,
                 phase_index,
                 system_sp.linear_interface().get_numcols(),
                 system_sp.linear_interface().get_numrows());

    if (log_mode == SolverLogMode::detailed && !log_dir.empty()) {
      std::filesystem::create_directories(log_dir);
      auto& li = system_sp.linear_interface();
      li.set_log_file(
          (std::filesystem::path(log_dir)
           / std::format(
               "{}_sc{}_ph{}", li.solver_name(), scene_index, phase_index))
              .string());
    }

    // Tag the LP with the Monolithic context so fallback warnings carry
    // the same (scene, phase) key as the surrounding info logs.
    system_sp.linear_interface().set_log_tag(
        std::format("Monolithic [s{} p{}]", scene_index, phase_index));

    if (auto result = system_sp.resolve(lp_opts); !result) {
      // Log the solver error with scene/phase context before writing the LP
      spdlog::warn("  Scene {} phase {}: {}",
                   scene_index,
                   phase_index,
                   result.error().message);

      // On error, write the problematic model to the log directory for
      // debugging
      const auto log_dir = m_options_.log_directory();
      std::filesystem::create_directories(log_dir);
      const auto filename =
          (std::filesystem::path(log_dir)
           / std::format("error_{}_{}", scene_index, phase_index))
              .string();
      if (auto lp_result = system_sp.write_lp(filename)) {
        spdlog::error("  Infeasible LP written to: {}", *lp_result);
      } else {
        spdlog::warn("{}", lp_result.error().message);
      }

      // LP diagnostic analysis is performed by run_gtopt after the solver
      // exits.  It scans the log directory for error*.lp files and runs
      // gtopt_check_lp on them with richer output (AI, IIS, parallel).

      auto error = std::move(result.error());
      error.message += std::format(
          ", failed at scene {} phase {}", scene_index, phase_index);

      return std::unexpected(std::move(error));
    }

    SPDLOG_DEBUG("    Scene {} phase {} solved ok", scene_index, phase_index);

    // update state variable dependents with the last solution
    const auto& solution_vector =
        system_sp.linear_interface().get_col_sol_raw();

    for (auto&& state_var :
         simulation().state_variables(scene_index, phase_index)
             | std::views::values)
    {
      const double solution_value = solution_vector[state_var.col()];

      for (auto&& dep_var : state_var.dependent_variables()) {
        auto& target_system =
            system(dep_var.scene_index(), dep_var.phase_index());
        target_system.linear_interface().set_col_raw(dep_var.col(),
                                                     solution_value);
      }
    }
  }

  return {};
}

auto PlanningLP::resolve(const SolverOptions& lp_opts)
    -> std::expected<int, Error>
{
  const auto num_phases = simulation().phases().size();
  auto solver = make_planning_method(m_options_, num_phases);
  return solver->solve(*this, lp_opts);
}

}  // namespace gtopt
