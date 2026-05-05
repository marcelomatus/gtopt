/**
 * @file      sddp_method.cpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) method implementation
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the forward/backward iterative decomposition with multi-scene
 * support, iterative feasibility backpropagation, and optimality cut sharing.
 * See sddp_method.hpp for the algorithm description and the free-function
 * building blocks declared there.
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

// ─── Utilities ──────────────────────────────────────────────────────────────

// assign_padded / align_up moved to sddp_forward_pass.cpp

const StateVariable* find_alpha_state_var(const SimulationLP& sim,
                                          SceneIndex scene_index,
                                          PhaseIndex phase_index) noexcept
{
  auto svar = sim.state_variable(StateVariable::Key {
      .uid = sddp_alpha_uid,
      .col_name = sddp_alpha_col_name,
      .class_name = sddp_alpha_lp_class,
      .lp_key = {.scene_index = scene_index, .phase_index = phase_index},
  });
  return svar ? &svar->get() : nullptr;
}

CutSharingMode parse_cut_sharing_mode(std::string_view name)
{
  return enum_from_name<CutSharingMode>(name).value_or(CutSharingMode::none);
}

ElasticFilterMode parse_elastic_filter_mode(std::string_view name)
{
  return enum_from_name<ElasticFilterMode>(name).value_or(
      ElasticFilterMode::chinneck);
}

// ─── Free utility functions ──────────────────────────────────────────────────

std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible,
    ProbabilityRescaleMode rescale_mode)
{
  const auto num_scenes = scene_feasible.size();
  std::vector<double> weights(num_scenes, 0.0);
  double total = 0.0;

  for (const auto& [si, feasible] : enumerate(scene_feasible)) {
    if (feasible == 0U) {
      continue;  // Infeasible → weight stays 0
    }
    if (si < scenes.size()) {
      for (const auto& sc : scenes[si].scenarios()) {
        weights[si] += sc.probability_factor();
      }
    }
    if (weights[si] <= 0.0) {
      weights[si] = 1.0;  // fallback equal weight
    }
    total += weights[si];
  }

  // Runtime rescaling: normalize feasible-scene weights to sum 1.0
  if (rescale_mode == ProbabilityRescaleMode::runtime && total > 0.0) {
    for (auto& w : weights) {
      w /= total;
    }
  } else if (total > 0.0) {
    // Non-runtime modes: use raw probability weights (no re-normalization)
    // but still handle the all-infeasible fallback below
  } else {
    // All infeasible or zero probability → equal weight among feasible
    int feasible_count = 0;
    for (const auto& [si, feasible] : enumerate(scene_feasible)) {
      if (feasible != 0U) {
        ++feasible_count;
      }
    }
    if (feasible_count > 0) {
      const double eq_w = 1.0 / static_cast<double>(feasible_count);
      for (const auto& [si, feasible] : enumerate(scene_feasible)) {
        if (feasible != 0U) {
          weights[si] = eq_w;
        }
      }
    }
  }

  return weights;
}

double compute_convergence_gap(double upper_bound, double lower_bound) noexcept
{
  const double denom = std::max(1.0, std::abs(upper_bound));
  return (upper_bound - lower_bound) / denom;
}

// ─── Kappa threshold checking ───────────────────────────────────────────────

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

}  // namespace

// ─── SDDPMethod ─────────────────────────────────────────────────────────────

SDDPMethod::SDDPMethod(PlanningLP& planning_lp, SDDPOptions opts) noexcept
    : m_planning_lp_(planning_lp)
    , m_options_(std::move(opts))
    , m_aperture_cache_(
          [&]() -> ApertureDataCache
          {
            // Skip aperture cache in simulation mode (no backward pass)
            if (m_options_.max_iterations <= 0) {
              return {};
            }
            const auto dir = planning_lp.options().sddp_aperture_directory();
            if (dir.empty()) {
              return {};
            }
            // Resolve relative aperture directory against input_directory
            std::filesystem::path dir_path {dir};
            if (dir_path.is_relative()) {
              dir_path =
                  std::filesystem::path {
                      planning_lp.options().input_directory()}
                  / dir_path;
            }
            return ApertureDataCache {dir_path};
          }())
    , m_label_maker_ {}
{
}

bool SDDPMethod::check_sentinel_stop() const
{
  if (m_options_.sentinel_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.sentinel_file);
}

bool SDDPMethod::check_api_stop_request() const
{
  if (m_options_.api_stop_request_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.api_stop_request_file);
}

bool SDDPMethod::should_stop() const
{
  if (m_in_simulation_) {
    return false;  // simulation pass always runs to completion
  }
  return m_stop_requested_.load() || check_sentinel_stop()
      || check_api_stop_request();
}

// ── Coefficient updates ─────────────────────────────────────────────────────

auto SDDPMethod::validate_inputs() const -> std::optional<Error>
{
  const auto& sim = planning_lp().simulation();
  if (sim.scenes().empty()) {
    return Error {
        .code = ErrorCode::InvalidInput,
        .message = "No scenes in simulation",
    };
  }
  if (sim.phases().size() < 2) {
    return Error {
        .code = ErrorCode::InvalidInput,
        .message = "SDDP requires at least 2 phases",
    };
  }
  return std::nullopt;
}

auto SDDPMethod::initialize_solver() -> std::expected<void, Error>
{
  if (m_initialized_) {
    SPDLOG_DEBUG("SDDP: already initialized, skipping re-initialization");
    return {};
  }

  SPDLOG_INFO("SDDP: initializing solver (no initial solve pass)");

  // Clamp ``min_iterations`` to ``max_iterations`` so the new default
  // ``min_iterations = 3`` cannot stretch a deliberately tiny run
  // (e.g. ``max_iterations = 0`` for simulation-only mode, or
  // ``max_iterations = 1`` for fast integration tests).  Silent in
  // INFO; promoted to DEBUG so the `--trace` log records the clamp.
  if (m_options_.min_iterations > m_options_.max_iterations) {
    SPDLOG_DEBUG("SDDP: clamping min_iterations {} → {} (max_iterations cap)",
                 m_options_.min_iterations,
                 m_options_.max_iterations);
    m_options_.min_iterations = m_options_.max_iterations;
  }

  const auto init_start = std::chrono::steady_clock::now();

  const auto& sim = planning_lp().simulation();
  const auto num_scenes = sim.scene_count();
  const auto num_phases = sim.phase_count();

  SPDLOG_INFO("SDDP: {} scene(s), {} phase(s)", num_scenes, num_phases);

  // Loud warning when cross-scene cut sharing is requested on a
  // multi-scene run.  gtopt implements multi-cut SDDP (one α per
  // scene), so cuts from scene S only validly bound α^k_S; the
  // `accumulate`/`expected`/`max` modes broadcast S's cut onto every
  // other scene's α, which is mathematically valid only when the
  // scenes literally share the same sample-path realization (same
  // inflows, demands, etc. at every (phase, block)).  Production
  // runs (e.g. juan/gtopt_iplp with 16 distinct hydrology samples)
  // do not satisfy that condition; the broadcast cuts produce
  // LB > UB that compounds across iterations.  See
  // `docs/analysis/investigations/sddp/sddp_cut_sharing_fix_plan_2026-04-30.md`
  // and the regression guard at `test/source/test_sddp_bounds_sanity.cpp`.
  if (m_options_.cut_sharing != CutSharingMode::none && num_scenes > 1) {
    SPDLOG_WARN(
        "SDDP: cut_sharing={} on {} scenes — cross-scene broadcasting "
        "is mathematically valid only when scenes share IDENTICAL "
        "sample-path realizations (same inflows / demands / capacities "
        "at every phase and block).  Heterogeneous-realization runs "
        "may produce LB > UB.  Use cut_sharing=none for production "
        "multi-scenario runs.  See "
        "docs/analysis/investigations/sddp/"
        "sddp_cut_sharing_fix_plan_2026-04-30.md.",
        enum_name(m_options_.cut_sharing),
        num_scenes);
  }

  m_scene_phase_states_.resize(num_scenes);
  m_cut_store_.resize_scenes(num_scenes);
  m_infeasibility_counter_.resize(num_scenes);
  m_max_kappa_.resize(num_scenes);
  // Reset on every solve() — `forward_infeas_rollback` semantics start
  // fresh per call (`SceneRetryState{}` clears the
  // global_cuts_at_last_failure optional).
  m_scene_retry_state_.assign(num_scenes, SceneRetryState {});
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    m_infeasibility_counter_[scene_index].resize(num_phases, 0);
    m_max_kappa_[scene_index].resize(num_phases, -1.0);
  }

  // ── Low-memory mode: configure all SystemLPs ──────────────────────────────
  if (m_options_.low_memory_mode != LowMemoryMode::off) {
    const auto codec = select_codec(m_options_.memory_codec);
    SPDLOG_INFO("SDDP: low_memory mode {} (codec: {})",
                enum_name(m_options_.low_memory_mode),
                codec_name(codec));
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
        planning_lp()
            .system(scene_index, phase_index)
            .set_low_memory(m_options_.low_memory_mode, codec);
      }
    }
  }

  // Auto-scale alpha: tie scale_alpha to the expected α magnitude at
  // master optimum so that the LP-space α coefficient lands O(1)
  // after the cut row's α-pivot equilibration (see
  // ``LinearInterface::add_row`` step 3 + ``SparseRow::pivot_col``).
  //
  // Estimator:
  //   α_phys at convergence ≈ Σ_t z_t = sum of per-phase block
  //   costs at the master's optimal trajectory.  We don't have z_t
  //   pre-solve, but a robust upper-bound proxy is
  //
  //     α_estimate = max_lp_coef × scale_objective × num_phases²
  //
  //   where:
  //     - max_lp_coef = max ``lp_stats_max_abs`` across all cells
  //       (pre-equilibration semantic max; post-equilibration would
  //       be 1.0 — both work as a conservative size proxy)
  //     - scale_objective folds back to physical
  //     - num_phases² is the triangular weight (each later phase's
  //       cost contributes to all earlier α's, so Σ α_t scales as
  //       N(N+1)/2 of the per-phase magnitude)
  //
  // Rounded UP to the next power of 10 (mirrors
  // ``auto_scale_reservoirs``'s ``scale_for()`` helper) so the LP
  // coefficient log shows clean numbers (e.g. ``scale_alpha = 1e9``
  // not ``2.6e9``) — easier to debug and compare across runs.
  //
  // The previous heuristic (`max state var_scale`, e.g. juan
  // reservoir 1453) tied scale_alpha to the wrong axis: state
  // variables drop out of cut-row LP coefficients (s_v cancels), so
  // their magnitude doesn't drive cut-row κ.  α's magnitude does.
  if (m_options_.scale_alpha <= 0.0) {
    double max_lp_coef = 1.0;
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
        const auto& li =
            planning_lp().system(scene_index, phase_index).linear_interface();
        max_lp_coef = std::max(max_lp_coef, li.lp_stats_max_abs());
      }
    }
    const auto scale_obj = planning_lp().options().scale_objective();
    const auto num_phases_d = static_cast<double>(num_phases);
    // Linear-in-N heuristic:
    //   scale_alpha ≈ scale_obj × num_phases × max_lp_coef
    // Rationale: α_phys at master optimum ≈ Σ_t block_cost_t = O(N)
    // per-phase magnitudes.  Going LARGER than necessary is also
    // harmful: α_LP = α_phys / scale_alpha underflows, and α's obj
    // coefficient (scale_alpha / scale_obj) balloons relative to
    // other obj coefs — both hurt solver precision.  Per-case
    // tuning via the explicit ``scale_alpha`` JSON option overrides
    // this default.
    const auto raw_estimate = max_lp_coef * scale_obj * num_phases_d;
    const auto raw_clamped = std::max(raw_estimate, 1.0);
    m_options_.scale_alpha = std::pow(10.0, std::ceil(std::log10(raw_clamped)));
    SPDLOG_INFO(
        "SDDP: auto scale_alpha = {:.2e} (raw_estimate={:.3e} = "
        "max_lp_coef={:.3e} × scale_obj={:.0f} × num_phases={})",
        m_options_.scale_alpha,
        raw_estimate,
        max_lp_coef,
        scale_obj,
        num_phases);
  }

  // The setup steps below (add_col for alpha, get_col bounds for
  // state links, save_base_numrows, cut loading) trigger the
  // rebuild-callback or reconstruct-from-snapshot transparently
  // per-cell when the backend is released — so no eager build pass
  // is needed here regardless of low_memory mode.

  SPDLOG_INFO("SDDP: adding alpha variables and collecting state links");
  // Per-scene setup is independent across scenes: each writes to its
  // own slot of m_scene_phase_states_.  Under rebuild mode every
  // add_col(alpha) / bound-read triggers a full flatten+load_flat per
  // cell via ensure_backend, so a serial loop over 16 scenes × 51
  // phases = ~100s wall on large problems.  Dispatch per-scene to the
  // solver work pool for ~16× speedup on that critical path.
  {
    auto pool = make_solver_work_pool(
        m_options_.pool_cpu_factor,
        /*cpu_threshold_override=*/0.0,
        /*scheduler_interval=*/std::chrono::milliseconds(50),
        /*memory_limit_mb=*/m_options_.pool_memory_limit_mb);
    std::vector<std::future<void>> futures;
    futures.reserve(num_scenes);
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto result = pool->submit(
          [this, scene_index, num_phases]
          {
            initialize_alpha_variables(scene_index);
            collect_state_variable_links(scene_index);
            // Save per-(scene, phase) base row counts in the same
            // scope so we amortize the ensure_backend rebuilds across
            // all per-cell setup work.
            for (const auto phase_index :
                 iota_range<PhaseIndex>(0, num_phases)) {
              auto& li = planning_lp()
                             .system(scene_index, phase_index)
                             .linear_interface();
              li.save_base_numrows();
              m_scene_phase_states_[scene_index][phase_index].base_nrows =
                  li.base_numrows();
            }
            SPDLOG_DEBUG(
                "SDDP: scene {} initialized ({} state links)",
                uid_of(scene_index),
                m_scene_phase_states_[scene_index].empty()
                    ? 0
                    : m_scene_phase_states_[scene_index][first_phase_index()]
                          .outgoing_links.size());
          });
      if (result.has_value()) {
        futures.push_back(std::move(*result));
      }
    }
    for (auto& fut : futures) {
      fut.get();
    }
  }

  // ── Load hot-start cuts and track max iteration for offset ────────────────
  // Recovery cuts (explicit input, scene recovery) require
  // recovery_mode >= cuts.  Without --recover, the solver starts cold.
  // All individual load events are logged at DEBUG; one summary INFO line
  // is emitted at the end so a user can tell cold-vs-warm at a glance.
  m_iteration_offset_ = IterationIndex {};

  std::size_t cuts_count = 0;
  std::string cuts_source;  // "" → cold; otherwise file or dir path
  std::size_t boundary_count = 0;
  std::size_t named_count = 0;
  bool state_loaded = false;

  // Resolve relative SDDP file paths against `input_directory` (mirrors the
  // aperture-directory resolution at sddp_method.cpp:179-186).  Without
  // this, gtopt invoked from a cwd other than the case dir silently skips
  // boundary / named / hot-start cut files even when the JSON wires them
  // up correctly — observed on `support/juan/gtopt_iplp_plain` as
  // `boundary=none` in the HotStart summary and α=0 throughout the last
  // phase.  Absolute paths pass through unchanged.
  const auto resolve_input = [&](const std::string& path) -> std::string
  {
    if (path.empty()) {
      return path;
    }
    std::filesystem::path p {path};
    if (p.is_absolute()) {
      return path;
    }
    return (std::filesystem::path {planning_lp().options().input_directory()}
            / p)
        .string();
  };

  if (m_options_.recovery_mode >= RecoveryMode::cuts) {
    if (!m_options_.cuts_input_file.empty()) {
      const auto cuts_path = resolve_input(m_options_.cuts_input_file);
      auto result = load_cuts(cuts_path);
      if (result.has_value()) {
        // load_cuts() already updated m_iteration_offset_ via next() so
        // the first new iteration is strictly past result->max_iteration.
        cuts_count = result->count;
        cuts_source = cuts_path;
        SPDLOG_DEBUG("SDDP hot-start: loaded {} cuts (max_iter={})",
                     result->count,
                     result->max_iteration);
      } else {
        SPDLOG_WARN("SDDP hot-start: could not load cuts: {}",
                    result.error().message);
      }
    } else if (m_options_.cut_recovery_mode != HotStartMode::none
               && !m_options_.cuts_output_file.empty())
    {
      const auto cut_dir =
          std::filesystem::path(m_options_.cuts_output_file).parent_path();
      if (!cut_dir.empty() && std::filesystem::exists(cut_dir)) {
        auto result = load_scene_cuts_from_directory(cut_dir.string());
        if (result.has_value() && result->count > 0) {
          // Start strictly past the highest loaded iteration; otherwise
          // the first new save_cuts_for_iteration would collide with a
          // loaded cut at the same index in m_cut_store_.
          m_iteration_offset_ =
              std::max(m_iteration_offset_, next(result->max_iteration));
          cuts_count = result->count;
          cuts_source = cut_dir.string();
          SPDLOG_DEBUG("SDDP hot-start: loaded {} cuts from {} (max_iter={})",
                       result->count,
                       cut_dir.string(),
                       result->max_iteration);
        }
      }
    }
  }

  // ── Apply caller-supplied offset hint (e.g. cascade level base) ───────
  // Composed with the hot-start offset via std::max, so hot-start cuts
  // always win when they demand a higher offset.  This is what gives
  // each cascade level a disjoint iteration-index range in m_cut_store_
  // without relying on inherited cuts to carry the offset forward.
  if (const auto hint = m_options_.iteration_offset_hint; hint.has_value()) {
    m_iteration_offset_ = std::max(m_iteration_offset_, *hint);
  }

  // ── Load boundary cuts (future-cost function for last phase) ──────────
  // Boundary cuts are part of the problem specification (analogous to
  // PLP's "planos de embalse"), NOT recovery state.  They do NOT
  // affect the iteration offset — the solver always starts from
  // iteration 0 (or from the recovery offset if recovery cuts exist).
  // Missing file with a non-empty config path = WARN (silent skip used
  // to mask common "cwd ≠ case dir" mistakes).
  if (!m_options_.boundary_cuts_file.empty()) {
    const auto bc_path = resolve_input(m_options_.boundary_cuts_file);
    if (std::filesystem::exists(bc_path)) {
      auto result = load_boundary_cuts(bc_path);
      if (result.has_value()) {
        boundary_count = result->count;
        SPDLOG_DEBUG(
            "SDDP: loaded {} boundary cuts from {}", result->count, bc_path);
      } else {
        SPDLOG_WARN("SDDP: could not load boundary cuts: {}",
                    result.error().message);
      }
    } else {
      SPDLOG_WARN(
          "SDDP: boundary_cuts_file '{}' not found (resolved to '{}') — "
          "α at the last phase will stay pinned at 0",
          m_options_.boundary_cuts_file,
          bc_path);
    }
  }

  // ── Load named cuts (all phases, named state variables) ────────────────
  // Named cuts are also part of the problem specification — always load,
  // but do NOT affect the iteration offset.
  if (!m_options_.named_cuts_file.empty()) {
    const auto nc_path = resolve_input(m_options_.named_cuts_file);
    if (std::filesystem::exists(nc_path)) {
      auto result = load_named_cuts(nc_path);
      if (result.has_value()) {
        named_count = result->count;
        SPDLOG_DEBUG(
            "SDDP: loaded {} named cuts from {}", result->count, nc_path);
      } else {
        SPDLOG_WARN("SDDP: could not load named cuts: {}",
                    result.error().message);
      }
    } else {
      SPDLOG_WARN(
          "SDDP: named_cuts_file '{}' not found (resolved to '{}') — "
          "no named hot-start cuts will be installed",
          m_options_.named_cuts_file,
          nc_path);
    }
  }

  // ── Load state variable column solutions ──────────────────────────────────
  // Recover state when recovery_mode is "full" and cuts were loaded.
  if (m_options_.recovery_mode == RecoveryMode::full
      && (m_options_.cut_recovery_mode != HotStartMode::none
          || m_iteration_offset_ > 0)
      && !m_options_.cuts_output_file.empty())
  {
    const auto cut_dir =
        std::filesystem::path(m_options_.cuts_output_file).parent_path();
    const auto state_file = (cut_dir / sddp_file::state_cols).string();
    if (std::filesystem::exists(state_file)) {
      auto result = load_state(state_file);
      if (result.has_value()) {
        state_loaded = true;
        SPDLOG_DEBUG("SDDP: loaded state variables from {}", state_file);
      } else {
        SPDLOG_WARN("SDDP: could not load state variables: {}",
                    result.error().message);
      }
    }
  }

  // ── One-line hot-start summary so cold-vs-warm is visible at a glance ──
  {
    const auto fmt_count = [](std::size_t n) -> std::string
    { return n == 0 ? std::string {"none"} : std::to_string(n); };
    SPDLOG_INFO(
        "SDDP HotStart: cuts={} boundary={} named={} state={} iter_offset={}",
        cuts_source.empty() ? std::string {"cold"} : fmt_count(cuts_count),
        fmt_count(boundary_count),
        fmt_count(named_count),
        state_loaded ? "loaded" : "none",
        m_iteration_offset_);
  }

  // ── Low-memory: release every per-cell backend after initialization ─────
  //
  // `release_backend` is a no-op under `LowMemoryMode::off`, so the loop
  // runs unconditionally.  Cut loaders already populated `m_active_cuts_`
  // via `record_cut_row`, so the persistent state needed to reconstruct
  // each cell is intact.  Eager release here keeps the memory profile
  // uniform across compress/rebuild: the forward pass's per-phase
  // `ensure_lp_built` will reload each cell on first touch.
  //
  // Under `compress` each release does real work (zstd/lz4 compression
  // of a multi-MB flat LP).  For 800+ cells that's seconds of CPU.
  // Dispatch across the solver work pool when more than one cell is
  // in the grid; otherwise fall through to a direct call.
  if (m_options_.low_memory_mode != LowMemoryMode::off
      && num_scenes * num_phases > 1)
  {
    auto pool = make_solver_work_pool(
        m_options_.pool_cpu_factor,
        /*cpu_threshold_override=*/0.0,
        /*scheduler_interval=*/std::chrono::milliseconds(50),
        /*memory_limit_mb=*/m_options_.pool_memory_limit_mb);
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<std::size_t>(num_scenes)
                    * static_cast<std::size_t>(num_phases));
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
        auto result = pool->submit(
            [this, scene_index, phase_index]
            {
              planning_lp().system(scene_index, phase_index).release_backend();
            });
        if (result.has_value()) {
          futures.push_back(std::move(*result));
        }
      }
    }
    for (auto& fut : futures) {
      fut.get();
    }
  } else {
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
        planning_lp().system(scene_index, phase_index).release_backend();
      }
    }
  }

  // ── Build preallocated iteration vector ───────────────────────────────────
  {
    // `next(offset, n)` returns the exclusive upper bound of a training
    // run starting at `m_iteration_offset_` and taking `max_iterations`
    // steps — the strong-index analogue of `offset + n` kept strongly
    // typed so the iota_range and array subscripts below don't need
    // any cast back from `Index`.
    const auto total_iterations =
        next(m_iteration_offset_, m_options_.max_iterations);
    m_iterations_.resize(total_iterations);
    for (const auto iidx :
         iota_range<IterationIndex>(IterationIndex {0}, total_iterations))
    {
      m_iterations_[iidx] = IterationLP {
          Iteration {
              .index = value_of(iidx),
          },
          iidx,
      };
    }
    // Overlay user-specified entries from simulation.iteration_array
    for (const auto& iter : sim.simulation().iteration_array) {
      const auto idx = IterationIndex {iter.index};
      if (iter.index >= 0 && iter.index < total_iterations) {
        m_iterations_[idx] = IterationLP {
            iter,
            idx,
        };
      }
    }
    SPDLOG_DEBUG("SDDP: preallocated {} iteration entries ({} user-specified)",
                 total_iterations,
                 sim.simulation().iteration_array.size());
  }

  m_initialized_ = true;

  const auto init_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - init_start)
                          .count();
  SPDLOG_INFO("SDDP: initialization complete ({:.2f}s)", init_s);
  return {};
}

void SDDPMethod::reset_live_state() noexcept
{
  // Publish a fresh default-initialised snapshot.  Callers of the
  // live-query API see `(iteration=0, gap=1.0, lb=0.0, ub=0.0,
  // converged=false)` as a single coherent reset.
  publish_live_metrics_(LiveMetrics {});
  m_current_pass_.store(0);
  m_scenes_done_.store(0);
}

auto SDDPPlanningMethod::solve(PlanningLP& planning_lp,
                               const SolverOptions& opts)
    -> std::expected<int, Error>
{
  const auto num_scenes = static_cast<int>(planning_lp.systems().size());
  const auto num_phases = num_scenes > 0
      ? static_cast<int>(planning_lp.systems().front().size())
      : 0;
  SPDLOG_INFO(
      "SDDPMethod: starting {} scene(s) × {} phase(s)", num_scenes, num_phases);

  SDDPMethod sddp(planning_lp, m_sddp_opts_);
  auto results = sddp.solve(opts);

  if (!results.has_value()) {
    return std::unexpected(std::move(results.error()));
  }

  m_last_results_ = std::move(*results);

  // Populate the SDDP summary on planning_lp for write_out() consumption.
  if (!m_last_results_.empty()) {
    const auto& last = m_last_results_.back();
    // Count only training iterations (all but the final simulation pass).
    const auto n = std::ssize(m_last_results_);
    const auto training_iters = n > 1 ? n - 1 : n;
    planning_lp.set_sddp_summary({
        .gap = last.gap,
        .gap_change = last.gap_change,
        .lower_bound = last.lower_bound,
        .upper_bound = last.upper_bound,
        .max_kappa = sddp.global_max_kappa(),
        .iterations = training_iters,
        .converged = last.converged,
        .stationary_converged = last.stationary_converged,
        .statistical_converged = last.statistical_converged,
    });
  }

  // Return 0 on success.
  // Reaching max_iterations without convergence is still a valid result:
  // the simulation pass already produced a final forward-pass solution,
  // so we return success to allow write_out() and exit(0).
  if (!m_last_results_.empty()) {
    const auto& last = m_last_results_.back();
    if (last.converged) {
      SPDLOG_INFO("SDDP: converged (gap={:.2f}%)", 100.0 * last.gap);
    } else {
      SPDLOG_WARN(
          "SDDP: max_iterations reached without convergence "
          "(gap={:.2f}%), returning best solution",
          100.0 * last.gap);
    }
    return 0;
  }

  return std::unexpected(Error {
      .code = ErrorCode::SolverError,
      .message = "SDDP produced no iteration results",
  });
}

}  // namespace gtopt
