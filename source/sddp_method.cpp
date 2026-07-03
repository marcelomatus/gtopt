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
#include <cstdint>
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
#include <gtopt/decision_variable_lp.hpp>
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
                                          PhaseIndex phase_index,
                                          SystemKind kind) noexcept
{
  auto svar = sim.state_variable(StateVariable::Key {
      .uid = sddp_alpha_uid,
      .col_name = sddp_alpha_col_name,
      .class_name = sddp_alpha_lp_class,
      .lp_key =
          {
              .scene_index = scene_index,
              .phase_index = phase_index,
              .kind = kind,
          },
  });
  return svar ? &svar->get() : nullptr;
}

const StateVariable* find_alpha_state_var(const SimulationLP& sim,
                                          SceneIndex scene_index,
                                          PhaseIndex phase_index,
                                          SceneIndex source_scene,
                                          SystemKind kind) noexcept
{
  // Multicut: the α column dedicated to `source_scene` is keyed by
  // `uid = sddp_alpha_uid + source_scene` (offset 0 == the legacy single α).
  const auto alpha_uid = static_cast<Uid>(
      sddp_alpha_uid
      + static_cast<Uid>(static_cast<std::size_t>(source_scene)));
  auto svar = sim.state_variable(StateVariable::Key {
      .uid = alpha_uid,
      .col_name = sddp_alpha_col_name,
      .class_name = sddp_alpha_lp_class,
      .lp_key =
          {
              .scene_index = scene_index,
              .phase_index = phase_index,
              .kind = kind,
          },
  });
  return svar ? &svar->get() : nullptr;
}

const StateVariable* find_user_alpha_state_var(const SimulationLP& sim,
                                               SceneIndex scene_index,
                                               PhaseIndex phase_index,
                                               Uid user_alpha_uid,
                                               SystemKind kind) noexcept
{
  // The user α is the AMPL `state`/`link` DecisionVariable registered under the
  // DEDICATED state class (`DecisionVariableLP::StateClassName` =
  // "UserStateVar", col_name = ValueName = "value") with `uid ==
  // user_alpha_uid`.  Distinct from the built-in α (`sddp_alpha_lp_class`), so
  // the two never collide.
  //
  // Unlike `find_alpha_state_var` (whose key carries the default `unknown`
  // scenario/stage uids, matching how the built-in α is registered), the user α
  // was registered with CONCRETE `scenario_uid` / `stage_uid` (via
  // `StateVariable::key(scenario, rep_stage, ...)` in
  // `DecisionVariableLP::build_cell_col`).  Those uids are not known here, so a
  // direct keyed `state_variable(Key)` lookup would miss.  Scan the cell's
  // (scene, phase, kind) map and match on the stable triple
  // `(class_name, uid, col_name)` instead — exactly one entry per cell.
  for (const auto& [key, svar] :
       sim.state_variables(scene_index, phase_index, kind))
  {
    if (key.class_name == DecisionVariableLP::StateClassName
        && key.uid == user_alpha_uid
        && key.col_name == DecisionVariableLP::ValueName)
    {
      return &svar;
    }
  }
  return nullptr;
}

std::vector<std::pair<ColIndex, Uid>> alpha_cols_on_cell(
    const SimulationLP& sim,
    SceneIndex scene_index,
    PhaseIndex phase_index,
    SystemKind kind)
{
  std::vector<std::pair<ColIndex, Uid>> cols;
  for (const auto src : iota_range<SceneIndex>(0, sim.scene_count())) {
    const auto* svar =
        find_alpha_state_var(sim, scene_index, phase_index, src, kind);
    if (svar == nullptr) {
      break;  // contiguous α uids from sddp_alpha_uid — first gap ends it
    }
    const auto alpha_uid = static_cast<Uid>(
        sddp_alpha_uid + static_cast<Uid>(static_cast<std::size_t>(src)));
    cols.emplace_back(svar->col(), alpha_uid);
  }
  return cols;
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

double SDDPMethod::effective_pool_cpu_factor() const noexcept
{
  // Explicit user setting always wins.
  if (m_options_.pool_cpu_factor_user_set) {
    return m_options_.pool_cpu_factor;
  }

  // Scene-aware auto default (see header doc): the async backward pass
  // spawns num_scenes × aperture-chunks concurrent tasks, so a run with
  // enough scenes already saturates every core.  Above the machine-scaled
  // threshold the historical 4× over-commit only buys scheduler/futex
  // contention (A/B: ~4% slower at 18/30 scenes), so cap to 1.0.  Below
  // it, keep the resolved over-commit to hide clone-mutex / solver
  // blocking on few-task runs.
  const auto num_scenes =
      static_cast<unsigned>(planning_lp().simulation().scene_count());
  const auto scene_threshold = std::max(2U, physical_concurrency() / 4U);
  return num_scenes >= scene_threshold ? 1.0 : m_options_.pool_cpu_factor;
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

void SDDPMethod::populate_future_cost_output()
{
  // Publish the per-scene α-rebase constants c̄ onto the persistent
  // SimulationLP so `FutureCostLP::add_to_output` can SELF-FIND them at write
  // time (the α columns themselves are read straight from the persistent
  // state-variable registry via `alpha_cols_on_cell`).  Called at the END of
  // solve(), sync + async.  Read-only w.r.t. the LP (no bound/row/col change →
  // SDDP bounds unaffected).  Works under ALL low_memory modes: the simulation
  // outlives the per-cell LP rebuild that `write_out` performs under compress,
  // unlike the former per-cell FutureCostLP stash.
  auto& sim = planning_lp().simulation();
  StrongIndexVector<SceneIndex, double> offsets(
      static_cast<std::size_t>(sim.scene_count()), 0.0);
  for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
    offsets[si] = scene_alpha_offset(si);
  }
  sim.set_alpha_offsets(std::move(offsets));
}

auto SDDPMethod::initialize_solver() -> std::expected<void, Error>
{
  if (m_initialized_) {
    SPDLOG_DEBUG("SDDP: already initialized, skipping re-initialization");
    return {};
  }

  SPDLOG_INFO("SDDP: initializing solver (no initial solve pass)");

  // Clamp ``min_iterations`` to ``max_iterations`` so any user-set
  // ``min_iterations`` cannot stretch a deliberately tiny run
  // (e.g. ``max_iterations = 0`` for simulation-only mode, or
  // ``max_iterations = 1`` for fast integration tests).  The current
  // default (``min_iterations = 1``) only matters for users who
  // explicitly bump it (cascade L0 sets ``min_iterations = 3``).
  // Silent in INFO; promoted to DEBUG so the `--trace` log records
  // the clamp.
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
  if (m_options_.cut_sharing != CutSharingMode::none
      && m_options_.cut_sharing != CutSharingMode::multicut && num_scenes > 1)
  {
    SPDLOG_WARN(
        "SDDP: cut_sharing={} on {} scenes — cross-scene broadcasting onto "
        "the destination scene's own α is mathematically valid only when "
        "scenes share IDENTICAL sample-path realizations (same inflows / "
        "demands / capacities at every phase and block).  Heterogeneous-"
        "realization runs may produce LB > UB.  Use cut_sharing=none (or the "
        "PLP-faithful multicut) for production multi-scenario runs.  See "
        "docs/analysis/investigations/sddp/"
        "sddp_cut_sharing_fix_plan_2026-04-30.md.",
        enum_name(m_options_.cut_sharing),
        num_scenes);
  }

  m_scene_phase_states_.resize(num_scenes);
  m_cut_store_.resize_scenes(num_scenes);
  m_infeasibility_counter_.resize(num_scenes);
  m_max_kappa_.resize(num_scenes);
  m_max_kappa_iter_.resize(num_scenes);
  // Reset on every solve() — `forward_infeas_rollback` semantics start
  // fresh per call (`SceneRetryState{}` clears the
  // global_cuts_at_last_failure optional).
  m_scene_retry_state_.assign(num_scenes, SceneRetryState {});
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    m_infeasibility_counter_[scene_index].resize(num_phases, 0);
    m_max_kappa_[scene_index].resize(num_phases, -1.0);
    m_max_kappa_iter_[scene_index].resize(num_phases, IterationIndex {});
  }

  // ── Resolve aperture_chunk_size (sentinel → concrete K) ────────────────
  //
  // Sentinel encoding (from `SddpOptions::aperture_chunk_size`):
  //   *  0 → auto: pick K so that the resulting per-phase task count
  //         (`scenes × ⌈A/K⌉`) tracks the work pool's target
  //         concurrency (`parallel_factor × physical_concurrency`).
  //   *  1 → legacy 1-aperture-per-task path.
  //   * >1 → use this K verbatim.
  //   * -1 → cap at A_max per phase (one task per scene, fully serial
  //         inside).
  //
  // Computed once at setup time and stored back into
  // `m_options_.aperture_chunk_size` so every per-phase call site
  // (`solve_apertures_for_phase`) reads the same resolved value.
  // A_max uses the per-phase aperture override when present
  // (`PhaseLP::apertures()`), otherwise falls back to the global
  // `simulation.aperture_array` size — matches the runtime resolution
  // in `solve_apertures_for_phase`.
  {
    const auto& global_aps = sim.apertures();
    int max_aps_per_phase = 0;
    for (const auto& plp : sim.phases()) {
      // PhaseLP::apertures() returns a vector<Uid> by reference;
      // empty means "use the global aperture_array", non-empty
      // overrides per phase (and may include duplicates which carry
      // weight semantics).  Either way, the chunk-size budget tracks
      // the count the runtime will actually iterate.
      const auto& per_phase = plp.apertures();
      const int n = !per_phase.empty() ? static_cast<int>(per_phase.size())
                                       : static_cast<int>(global_aps.size());
      max_aps_per_phase = std::max(n, max_aps_per_phase);
    }
    const int requested = m_options_.aperture_chunk_size;
    int resolved = 1;
    std::string_view source = "user";
    if (max_aps_per_phase <= 0) {
      // No apertures anywhere — chunk_size doesn't matter; pick 1 for safety.
      resolved = 1;
      source = "no-apertures";
    } else if (requested == 0) {
      // Auto path: apply the work-pool-saturation formula then round
      // DOWN to the nearest power of two (1, 2, 4, 8, 16, …) so chunk
      // shapes are balanced.  On juan/IPLP 16-scene × 16-aperture ×
      // 51-phase under barrier + split crossover + the bulk-API
      // `set_col_bounds_bulk` path (b85011e1) the auto-selected K=4
      // matches K=8 = 142.6 s, both ahead of legacy K=1 = 147.3 s.
      // Users who want a specific K override via
      // `aperture_chunk_size: K` in JSON or `--aperture-chunk-size K`
      // on the CLI; "auto" (or 0) maps here.
      resolved = compute_auto_aperture_chunk_size(
          max_aps_per_phase,
          static_cast<int>(num_scenes),
          static_cast<int>(physical_concurrency()),
          /*parallel_factor=*/2.0);
      source = "auto";
    } else if (requested == -1) {
      resolved = max_aps_per_phase;
      source = "fully-serial";
    } else {
      resolved = std::min(std::max(requested, 1), max_aps_per_phase);
      source = "user";
    }
    SPDLOG_INFO(
        "SDDP Aperture: chunk_size={} ({}: A_max={} S={} cores={} pf=2.0)",
        resolved,
        source,
        max_aps_per_phase,
        num_scenes,
        physical_concurrency());
    m_options_.aperture_chunk_size = resolved;
  }

  // ── Low-memory mode: configure all SystemLPs ──────────────────────────────
  if (m_options_.low_memory_mode != LowMemoryMode::off) {
    const auto codec = select_codec(m_options_.memory_codec);
    // Under a configured process memory limit, also drop the disposable XLP
    // collection wrappers on every `release_backend()` so the resident
    // floor is bounded to the active working set instead of all cells'
    // ~30 MB wrappers (the dominant compress-mode floor on large models).
    // Off when no limit — keeps the P3 keep-resident speed default.
    const bool drop_collections = m_options_.pool_memory_limit_mb > 0.0;
    SPDLOG_INFO("SDDP: low_memory mode {} (codec: {}){}",
                enum_name(m_options_.low_memory_mode),
                codec_name(codec),
                drop_collections
                    ? " + drop-collections-on-release (memory limit set)"
                    : "");
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
        auto& sys = planning_lp().system(scene_index, phase_index);
        sys.set_low_memory(m_options_.low_memory_mode, codec);
        sys.set_drop_collections_on_release(drop_collections);
      }
    }
  }

  // Resolve relative SDDP file paths against `input_directory` (mirrors the
  // aperture-directory resolution at sddp_method.cpp:179-186).  Defined here
  // (ahead of the α-scale block) because the boundary-cut path is needed to
  // derive scale_alpha below; the cut loaders further down reuse the same
  // lambda.  Without this, gtopt invoked from a cwd other than the case dir
  // silently skips boundary / named / hot-start cut files even when the JSON
  // wires them up correctly.  Absolute paths pass through unchanged.
  const auto resolve_input = [&](const std::string& path) -> std::string
  {
    if (path.empty()) {
      return path;
    }
    const std::filesystem::path p {path};
    if (p.is_absolute()) {
      return path;
    }
    return (std::filesystem::path {planning_lp().options().input_directory()}
            / p)
        .string();
  };

  // ── FutureCost element config consolidation (piece 5 step 2b) ──────────
  // When an active FutureCost element authors boundary-cut fields, they win
  // over the equivalent SDDPOptions fields (the element is the single source of
  // truth).  Read-site only — each override is gated on the element having
  // EXPLICITLY set the field, so when there is no FutureCost element (or it
  // leaves a field unset) `m_options_` is byte-for-byte unchanged (backward-
  // compatible — `test_sddp_boundary_cuts_mean_shift` stays green).  Skipped
  // entirely under `use_user_alpha` (the user α replaces boundary cuts, handled
  // separately below).
  if (const auto* fc = active_future_cost(planning_lp());
      fc != nullptr && !fc->use_user_alpha.value_or(false))
  {
    const auto cfg = boundary_config(*fc);
    if (cfg.cuts_file.has_value()) {
      m_options_.boundary_cuts_file = *cfg.cuts_file;
    }
    if (cfg.scale_alpha.has_value()) {
      m_options_.scale_alpha = *cfg.scale_alpha;
    }
    if (cfg.mean_shift.has_value()) {
      m_options_.boundary_cuts_mean_shift = *cfg.mean_shift;
    }
    if (cfg.sharing.has_value()) {
      m_options_.boundary_cut_sharing = *cfg.sharing;
    }
    if (cfg.mode.has_value()) {
      m_options_.boundary_cuts_mode = *cfg.mode;
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
    // Boundary-cut workflow: derive scale_alpha from the cut coefficients
    // through the SAME shared computation MonolithicMethod uses
    // (`boundary_cut_scale_alpha`).  The rule no longer depends on any
    // per-method state, so α is scaled identically regardless of solver
    // method.  Only when no boundary-cut file is supplied (cuts are produced
    // dynamically by the backward pass) do we fall back to the α-magnitude
    // proxy below — there are no input coefficients to read in that case.
    double cut_scale = 0.0;
    if (!m_options_.boundary_cuts_file.empty()) {
      const auto bc_path = resolve_input(m_options_.boundary_cuts_file);
      if (std::filesystem::exists(bc_path)) {
        cut_scale = boundary_cut_scale_alpha(
            planning_lp(), bc_path, /*option_scale_alpha=*/0.0);
      }
    }

    if (cut_scale > 0.0) {
      m_options_.scale_alpha = cut_scale;
      SPDLOG_INFO("SDDP: auto scale_alpha = {:.2e} (from boundary-cut coeffs)",
                  m_options_.scale_alpha);
    } else {
      // No input cuts — proxy α's magnitude from the LP coefficient scale.
      //   scale_alpha ≈ scale_obj × num_phases × max_lp_coef
      // Rationale: α_phys at master optimum ≈ Σ_t block_cost_t = O(N)
      // per-phase magnitudes, rounded UP to the next power of ten.
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
      const auto raw_estimate = max_lp_coef * scale_obj * num_phases_d;
      const auto raw_clamped = std::max(raw_estimate, 1.0);
      m_options_.scale_alpha =
          std::pow(10.0, std::ceil(std::log10(raw_clamped)));
      SPDLOG_INFO(
          "SDDP: auto scale_alpha = {:.2e} (raw_estimate={:.3e} = "
          "max_lp_coef={:.3e} × scale_obj={:.0f} × num_phases={})",
          m_options_.scale_alpha,
          raw_estimate,
          max_lp_coef,
          scale_obj,
          num_phases);
    }
  }

  // The setup steps below (add_col for alpha, get_col bounds for
  // state links, save_base_numrows, cut loading) trigger the
  // reconstruct-from-snapshot transparently per-cell when the backend
  // is released — so no eager build pass is needed here regardless of
  // low_memory mode.

  SPDLOG_INFO("SDDP: adding alpha variables and collecting state links");
  // Per-scene setup is independent across scenes: each writes to its
  // own slot of m_scene_phase_states_.  Under compress mode every
  // add_col(alpha) / bound-read triggers a snapshot reconstruct per
  // cell via ensure_backend, so a serial loop over 16 scenes × 51
  // phases = ~100s wall on large problems.  Dispatch per-scene to the
  // solver work pool for ~16× speedup on that critical path.
  {
    auto pool = make_solver_work_pool(
        effective_pool_cpu_factor(),
        /*cpu_threshold_override=*/0.0,
        /*scheduler_interval=*/std::chrono::milliseconds(50),
        /*memory_limit_mb=*/m_options_.pool_memory_limit_mb,
        /*pool_label=*/"SDDP alpha-setup pool");
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

  // `resolve_input` is defined above the α-scale block (it is also used to
  // locate the boundary-cut file for scale_alpha).

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

  // ── User-overridable FCF (piece 5 step 2a) ────────────────────────────
  // When an active FutureCost element carries `use_user_alpha`, the modeller's
  // global `state`/`link` α DecisionVariable + global UserConstraint cut row(s)
  // replace the built-in boundary-cut FCF.  The built-in α was already
  // registered INERT (cost 0, not a state var) in `initialize_alpha_variables`.
  // Here we (1) reject the both-active combination, (2) check the user α exists
  // and is priced, (3) skip the boundary-cut load entirely (the user's
  // UserConstraint cuts already built the recourse during the LP-build pass).
  if (const auto* fc = active_future_cost(planning_lp());
      fc != nullptr && fc->use_user_alpha.value_or(false))
  {
    // (1) Mutual exclusion vs the built-in boundary-cut FCF.
    if (!m_options_.boundary_cuts_file.empty()) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "FutureCost '{}': use_user_alpha and boundary_cuts_file are "
              "mutually exclusive — the user-authored α + cuts replace the "
              "boundary-cut FCF, so supplying both is ambiguous",
              fc->name),
      });
    }

    // (2) The user α DecisionVariable must exist AND be priced (cost != 0):
    //     a zero-cost α is never minimised, so the FCF has no objective weight
    //     and the master is effectively unbounded below in the future-cost
    //     dimension.  `user_alpha_uid` is REQUIRED.
    const auto ua_uid = fc->user_alpha_uid;
    if (!ua_uid.has_value()) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "FutureCost '{}': use_user_alpha requires user_alpha_uid (the "
              "uid of the global state/link α DecisionVariable)",
              fc->name),
      });
    }
    // Read the user α DecisionVariable's cost from the System INPUT data (not
    // the LP collection, which compress mode drops for planning-only elements).
    double user_alpha_cost = 0.0;
    bool found_user_alpha = false;
    {
      const auto& sys0 =
          planning_lp().system(SceneIndex {0}, PhaseIndex {0}).system();
      for (const auto& dv : sys0.decision_variable_array) {
        if (dv.uid == *ua_uid) {
          found_user_alpha = true;
          user_alpha_cost = dv.cost.value_or(0.0);
          break;
        }
      }
    }
    if (!found_user_alpha) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "FutureCost '{}': user_alpha_uid={} does not match any "
              "DecisionVariable",
              fc->name,
              static_cast<std::uint64_t>(*ua_uid)),
      });
    }
    if (user_alpha_cost == 0.0) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "FutureCost '{}': the user α DecisionVariable (uid={}) has "
              "cost == 0 — an unpriced cost-to-go leaves the master unbounded "
              "below in the future-cost dimension; set a positive cost (1.0 "
              "for the canonical 1:1 FCF weight)",
              fc->name,
              static_cast<std::uint64_t>(*ua_uid)),
      });
    }

    // (3) `use_user_alpha && multicut` is rejected (piece 5 step 2c, increment
    //     A scope).  The user α is a SINGLE column priced `cost`, whereas
    //     `CutSharingMode::multicut` expects N dedicated `varphi_s` α columns
    //     (one per source scene) to route per-scenario cuts onto.  The
    //     user-α backward dispatch puts every scene's cut on the same single
    //     user-α column, so multicut routing has no `varphi_s` to target.
    //     Multicut support over a user α is DEFERRED (increment D).
    if (m_options_.cut_sharing == CutSharingMode::multicut) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "FutureCost '{}': use_user_alpha is incompatible with "
              "cut_sharing=multicut — the user α is a single column, not N "
              "per-scene varphi_s columns.  Use cut_sharing=none (multicut "
              "support over a user α is deferred)",
              fc->name),
      });
    }

    // (4) `use_user_alpha && mean_shift` → mean_shift is meaningless (no
    //     boundary cuts to rebase); warn it is ignored.
    if (fc->mean_shift.value_or(false) || m_options_.boundary_cuts_mean_shift) {
      SPDLOG_WARN(
          "FutureCost '{}': mean_shift is ignored under use_user_alpha (there "
          "are no boundary cuts to rebase)",
          fc->name);
    }

    SPDLOG_INFO(
        "SDDP: use_user_alpha active (FutureCost '{}', user α uid={}, cost={}) "
        "— skipping boundary-cut load; built-in α is inert",
        fc->name,
        static_cast<std::uint64_t>(*ua_uid),
        user_alpha_cost);
    // m_scene_alpha_offsets_ stays zero (no rebase under user α).
  } else if (!m_options_.boundary_cuts_file.empty()) {
    // ── Load boundary cuts (future-cost function for last phase) ──────────
    // Boundary cuts are part of the problem specification (analogous to
    // PLP's "planos de embalse"), NOT recovery state.  They do NOT
    // affect the iteration offset — the solver always starts from
    // iteration 0 (or from the recovery offset if recovery cuts exist).
    // Missing file with a non-empty config path = WARN (silent skip used
    // to mask common "cwd ≠ case dir" mistakes).
    const auto bc_path = resolve_input(m_options_.boundary_cuts_file);
    if (std::filesystem::exists(bc_path)) {
      auto result = load_boundary_cuts(bc_path);
      if (result.has_value()) {
        boundary_count = result->count;
        // Capture the per-scene α-rebase offsets (zero unless the
        // mean-shift opt-in fired and the scene received cuts).
        // Used by SDDP UB / LB display sites to add c̄_scene back so
        // the user sees the pre-shift physical objective regardless
        // of the LP-side cut storage convention.
        m_scene_alpha_offsets_ = std::move(result->alpha_offsets_per_scene);
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

  // FutureCost α-output is enabled at the END of solve() by
  // populate_future_cost_output(), which publishes the per-scene rebase
  // offsets onto the persistent SimulationLP.  FutureCostLP::add_to_output
  // then SELF-FINDS its α columns + rebase at write time, so the output works
  // under ALL low_memory modes.

  // ── One-line hot-start summary so cold-vs-warm is visible at a glance ──
  // The "named" hot-start path was retired in 2026-05; internal cuts
  // travel via the Parquet ``cuts_input_file`` route only.  Boundary
  // cuts (CSV) are still loaded above when ``boundary_cuts_file`` is
  // set.
  {
    const auto fmt_count = [](std::size_t n) -> std::string
    { return n == 0 ? std::string {"none"} : std::to_string(n); };
    SPDLOG_INFO(
        "SDDP HotStart: cuts={} boundary={} iter_offset={}",
        cuts_source.empty() ? std::string {"cold"} : fmt_count(cuts_count),
        fmt_count(boundary_count),
        m_iteration_offset_);
  }

  // ── Low-memory: release every per-cell backend after initialization ─────
  //
  // `release_backend` is a no-op under `LowMemoryMode::off`, so the loop
  // runs unconditionally.  Cut loaders already populated `m_active_cuts_`
  // via `record_cut_row`, so the persistent state needed to reconstruct
  // each cell is intact.  Eager release here keeps the memory profile
  // uniform under compress: the forward pass's per-phase
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
        effective_pool_cpu_factor(),
        /*cpu_threshold_override=*/0.0,
        /*scheduler_interval=*/std::chrono::milliseconds(50),
        /*memory_limit_mb=*/m_options_.pool_memory_limit_mb,
        /*pool_label=*/"SDDP backend-release pool");
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
