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
      .class_name = sddp_alpha_class_name,
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
               scene_uid(scene_index),
               phase_uid(phase_index),
               kappa,
               threshold);

  const bool should_save_lp =
      (mode == KappaWarningMode::save_lp || mode == KappaWarningMode::diagnose);
  if (should_save_lp && !m_options_.log_directory.empty()) {
    std::filesystem::create_directories(m_options_.log_directory);
    const auto stem = (std::filesystem::path(m_options_.log_directory)
                       / as_label("kappa",
                                  scene_uid(scene_index),
                                  phase_uid(phase_index),
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
      "Kappa", iteration_index, scene_uid(scene_index), phase_uid(phase_index));

  // Collect cut rows for this (scene, phase) from the cut store
  const auto& scene_cuts = m_cut_store_.scene_cuts();
  const auto phase_uid_val = phase_uid(phase_index);

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

void SDDPMethod::clear_stored_cuts() noexcept
{
  m_cut_store_.clear();
}

void SDDPMethod::forget_first_cuts(std::ptrdiff_t count)
{
  m_cut_store_.forget_first_cuts(count, planning_lp());
}

void SDDPMethod::update_stored_cut_duals()
{
  m_cut_store_.update_stored_cut_duals(planning_lp());
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPMethod::initialize_alpha_variables(SceneIndex scene_index)
{
  auto& sim = planning_lp().simulation();
  const auto& phases = sim.phases();
  const auto last_phase_index = sim.last_phase_index();

  auto& phase_states = m_scene_phase_states_[scene_index];
  phase_states.resize(phases.size());

  // Add α (future-cost) variable to every phase except the last
  for (auto&& [pi, _phase] : enumerate<PhaseIndex>(phases)) {
    if (pi == last_phase_index) {
      break;
    }
    auto& li = planning_lp().system(scene_index, pi).linear_interface();

    const auto sa = m_options_.scale_alpha;
    const auto alpha_sparse = SparseCol {
        .lowb = m_options_.alpha_min / sa,
        .uppb = m_options_.alpha_max / sa,
        .cost = sa,
        .is_state = true,
        .scale = sa,
        .class_name = sddp_alpha_class_name,
        .variable_name = sddp_alpha_col_name,
        .context =
            make_scene_phase_context(scene_uid(scene_index), _phase.uid()),
    };
    const auto alpha_col = li.add_col(alpha_sparse);

    // Track dynamic column for low_memory reconstruction
    planning_lp().system(scene_index, pi).record_dynamic_col(alpha_sparse);

    // Register alpha as a regular state variable so all label-based
    // machinery (state CSV I/O, cut CSV I/O, cross-level resolution)
    // treats it uniformly with reservoir/storage state vars.  Without
    // this, cascade level transitions would need a separate resolve path
    // for alpha, inviting stale-col-index bugs.
    std::ignore = sim.add_state_variable(
        StateVariable::Key {
            .uid = sddp_alpha_uid,
            .col_name = sddp_alpha_col_name,
            .class_name = sddp_alpha_class_name,
            .lp_key = {.scene_index = scene_index, .phase_index = pi},
        },
        alpha_col,
        0.0,  // scost: no elastic penalty on alpha
        sa,  // var_scale: same as SparseCol.scale
        alpha_sparse.context);
  }
}

void SDDPMethod::collect_state_variable_links(SceneIndex scene_index)
{
  const auto& sim = planning_lp().simulation();
  const auto& phases = sim.phases();
  const auto last_phase_index = sim.last_phase_index();

  auto& phase_states = m_scene_phase_states_[scene_index];

  for (auto&& [phase_index, _ph] : enumerate<PhaseIndex>(phases)) {
    // The last phase produces no outgoing state-variable links to a
    // next phase (there is no next phase), so there is nothing to
    // collect and we break.  Incidentally this also avoids touching
    // the last phase's backend — which, under low_memory modes, is
    // still released at this point because `initialize_alpha_variables`
    // only added alpha (and thus only reconstructed) for non-last
    // phases — but the real reason is structural.
    if (phase_index == last_phase_index) {
      break;
    }

    auto& state = phase_states[phase_index];

    // Read column bounds from the source phase LP
    const auto& src_li =
        planning_lp().system(scene_index, phase_index).linear_interface();
    const auto col_lo = src_li.get_col_low_raw();
    const auto col_hi = src_li.get_col_upp_raw();
    const auto scale_obj = src_li.scale_objective();

    const auto next_phase_index = next(phase_index);

    for (const auto& [key, svar] :
         sim.state_variables(scene_index, phase_index))
    {
      // Per-variable state cost from StateVariable (set at registration time
      // by ReservoirLP, BatteryLP, etc.).  Pre-divide by scale_objective so
      // it is consistent with the global penalty.
      const auto link_scost =
          (svar.scost() > 0.0) ? svar.scost() / scale_obj : 0.0;

      for (const auto& dep : svar.dependent_variables()) {
        if (dep.phase_index() != next_phase_index
            || dep.scene_index() != scene_index)
        {
          continue;
        }

        state.outgoing_links.push_back(StateVarLink {
            .source_col = svar.col(),
            .dependent_col = dep.col(),
            .source_phase_index = phase_index,
            .target_phase_index = dep.phase_index(),
            .source_low = col_lo[svar.col()],
            .source_upp = col_hi[svar.col()],
            .var_scale = svar.var_scale(),
            .scost = link_scost,
            // Raw pointer into the simulation's state-variable registry
            // (flat_map, stable for the full solver lifetime — same
            // lifetime that already couples source and dependent LP cols).
            .state_var = &svar,
            // Identity for diagnostic logs (e.g. "Reservoir:8:efin").
            .class_name = key.class_name,
            .col_name = key.col_name,
            .uid = key.uid,
        });
      }
    }

    // Coverage audit: one TRACE line per (scene, phase) with both
    // counts shown side-by-side.  Readers grep for `(N/M)` where N !=
    // M to spot skip-ahead couplings or — more importantly — state
    // variables that were registered but have no dependent link in
    // the next phase, which would silently defeat the elastic filter.
    SPDLOG_TRACE(
        "SDDP: scene {} phase {} outgoing state-variable links: {}/{} "
        "(links/registered state vars)",
        scene_index,
        phase_index,
        state.outgoing_links.size(),
        sim.state_variables(scene_index, phase_index).size());
  }
}

void SDDPMethod::capture_state_variable_values(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    std::span<const double> col_sol,
    std::span<const double> reduced_costs) const noexcept
{
  const auto& sim = planning_lp().simulation();

  // 1. Always write col_sol for every state variable in THIS phase.
  //    Consumed by the next phase's propagate_trial_values().
  for (const auto& [key, svar] : sim.state_variables(scene_index, phase_index))
  {
    const auto col = svar.col();
    if (col < col_index_size(col_sol)) {
      svar.set_col_sol(col_sol[col]);
    }
  }

  // 2. Write per-link reduced_cost onto the *source* state variables
  //    in the previous phase (whose outgoing_links have dependent_col
  //    in THIS phase's LP).  No previous phase on phase 0.
  if (!phase_index) {
    return;
  }
  const auto prev_phase_index = previous(phase_index);
  const auto& prev_state = m_scene_phase_states_[scene_index][prev_phase_index];

  for (const auto& link : prev_state.outgoing_links) {
    if (link.state_var == nullptr) {
      continue;
    }
    const auto dep = link.dependent_col;
    if (dep < col_index_size(reduced_costs)) {
      link.state_var->set_reduced_cost(reduced_costs[dep]);
    }
  }
}

// ── Elastic filter via LP clone (PLP pattern) ───────────────────────────────

std::optional<SDDPMethod::ElasticResult> SDDPMethod::elastic_solve(
    SceneIndex scene_index, PhaseIndex phase_index, const SolverOptions& opts)
{
  if (!phase_index) {
    return std::nullopt;
  }

  const auto& li =
      planning_lp().system(scene_index, phase_index).linear_interface();
  const auto prev_phase_index = previous(phase_index);
  const auto& prev_state = m_scene_phase_states_[scene_index][prev_phase_index];

  // Delegate to BendersCut member (uses work pool when set).
  auto elastic_opts = opts;
  elastic_opts.crossover = false;

  // Scale the elastic penalty by cost_factor so it is consistent with all
  // other LP objective coefficients that go through stage_ecost / cost_factor.
  // The per-variable physical-unit scaling (var_scale) is applied inside
  // relax_fixed_state_variable() using each link's var_scale field.
  const auto scale_obj = li.scale_objective();
  const auto scaled_penalty = m_options_.elastic_penalty / scale_obj;

  // Chinneck IIS mode runs an extra re-solve to filter non-essential
  // relaxed bounds before cut construction.  Other modes use the regular
  // elastic filter (cuts may be averaged via build_multi_cuts at the
  // call site).
  auto result = (m_options_.elastic_filter_mode == ElasticFilterMode::chinneck)
      ? chinneck_filter_solve(
            li, prev_state.outgoing_links, scaled_penalty, elastic_opts)
      : m_benders_cut_.elastic_filter_solve(
            li, prev_state.outgoing_links, scaled_penalty, elastic_opts);

  if (result.has_value()) {
    // The clone's solve activity (resolve, fallbacks, kappa, wall
    // time) lives on the clone's own SolverStats.  Fold it back into
    // the owning system so the end-of-run aggregate reflects the true
    // backend workload, including elastic retries.
    planning_lp()
        .system(scene_index, phase_index)
        .merge_solver_stats(result->clone.solver_stats());

    SPDLOG_TRACE(
        "SDDP elastic: scene {} phase {} solved via clone "
        "(obj={:.4f})",
        scene_index,
        phase_index,
        result->clone.get_obj_value());
  }

  return result;
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

  // Set previous phase's SystemLP so that update_lp elements
  // (seepage, production factor, discharge limit) can look up the
  // previous phase's efin when computing reservoir volume via
  // physical_eini.  In warm_start mode, skip this — physical_eini
  // falls back to the warm solution or vini instead.
  const auto lookup = planning_lp().options().sddp_state_variable_lookup_mode();
  if (phase_index && lookup == StateVariableLookupMode::cross_phase) {
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
                   scene_uid(scene_index),
                   phase_uid(phase_index),
                   updated);
    }
  }
}

// ── Forward pass ────────────────────────────────────────────────────────────

// ── SDDP task priority helpers ───────────────────────────────────────────────

namespace
{

/// Build an `SDDPTaskKey` tuple for an SDDP LP solve task.
///
/// The key is `(iteration, is_backward, phase, is_nonlp)` where:
///  - `is_backward`: 0 = forward pass, 1 = backward pass
///  - `is_nonlp`:    0 = LP solve/resolve, 1 = other (e.g. write_lp)
///
/// With the default `std::less<SDDPTaskKey>` comparator (lexicographic),
/// smaller tuples have **higher** execution priority:
///  - Lower iteration → higher priority
///  - Forward pass (0) → higher priority than backward (1)
///  - Lower phase index → higher priority
///  - LP solve (0) → higher priority than non-LP (1)
///
/// Both forward and backward LP solves use `TaskPriority::Medium`.
/// The tuple key alone provides the full SDDP ordering, removing the
/// need for the old High/Medium tier split.

BasicTaskRequirements<SDDPTaskKey> make_forward_lp_task_req(
    IterationIndex iteration_index, PhaseIndex phase_index) noexcept
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(iteration_index,
                                         SDDPPassDirection::forward,
                                         phase_index,
                                         SDDPTaskKind::lp),
      .name = {},
  };
}

BasicTaskRequirements<SDDPTaskKey> make_backward_lp_task_req(
    IterationIndex iteration_index, PhaseIndex phase_index) noexcept
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(iteration_index,
                                         SDDPPassDirection::backward,
                                         phase_index,
                                         SDDPTaskKind::lp),
      .name = {},
  };
}

}  // namespace

// ── forward_pass() — now in sddp_forward_pass.cpp ───────────────────────────

// ── Helper: store a cut for sharing and persistence (thread-safe) ───────────

void SDDPMethod::store_cut(SceneIndex scene_index,
                           PhaseIndex src_phase_index,
                           const SparseRow& cut,
                           CutType type,
                           RowIndex row)
{
  // Track cut for low_memory reconstruction
  planning_lp().system(scene_index, src_phase_index).record_cut_row(cut);

  m_cut_store_.store_cut(scene_index,
                         src_phase_index,
                         cut,
                         type,
                         row,
                         scene_uid(scene_index),
                         phase_uid(src_phase_index));
}

// ── Helper: resolve an LP via the work pool (avoids naked direct calls) ─────

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

  const auto sa = m_options_.scale_alpha;
  const auto ceps = m_options_.cut_coeff_eps;
  const auto cmax = m_options_.cut_coeff_max;
  // No-span cut builder: reduced costs are read directly from each
  // link's back-pointer to the source StateVariable, avoiding the
  // per-phase full-vector cache that used to live on PhaseStateInfo
  // (`forward_col_cost`).
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
  auto cut = build_benders_cut(src_alpha_col,
                               src_state.outgoing_links,
                               target_state.forward_full_obj,
                               sa,
                               ceps,
                               scale_obj);
  cut.class_name = "Sddp";
  cut.constraint_name = "scut";
  cut.context = make_iteration_context(scene_uid(scene_index),
                                       phase_uid(phase_index),
                                       iteration_index,
                                       cut_offset);
  rescale_benders_cut(cut, src_alpha_col, cmax);
  filter_cut_coefficients(cut, src_alpha_col, ceps);
  const auto dt_build = elapsed_s(t_build);

  const auto t_add_row = Clock::now();
  const auto cut_row = src_li.add_row(cut);
  const auto dt_add_row = elapsed_s(t_add_row);

  const auto t_store = Clock::now();
  store_cut(scene_index, prev_phase_index, cut, CutType::Optimality, cut_row);
  const auto dt_store = elapsed_s(t_store);

  ++cuts_added;
  m_phase_grid_.record(
      iteration_index, scene_uid(scene_index), phase_index, GridCell::Backward);

  SPDLOG_TRACE("SDDP Backward [i{} s{} p{}]: cut for phase {} rhs={:.4f}",
               iteration_index,
               scene_uid(scene_index),
               phase_uid(phase_index),
               phase_uid(prev_phase_index),
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
                                scene_uid(scene_index),
                                phase_uid(prev_phase_index)));
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
                   scene_uid(scene_index),
                   phase_uid(prev_phase_index)),
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
               scene_uid(scene_index),
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
                                          scene_uid(scene_index),
                                          phase_uid(phase_index))),
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
               scene_uid(scene_index),
               total_cuts);
  return total_cuts;
}

// ── Cut sharing (delegated to sddp_cut_sharing.hpp free function) ───────────

void SDDPMethod::share_cuts_for_phase(
    PhaseIndex phase_index,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    [[maybe_unused]] IterationIndex iteration_index)
{
  gtopt::share_cuts_for_phase(
      phase_index, scene_cuts, m_options_.cut_sharing, planning_lp(), {});
}

// ── Cut pruning ─────────────────────────────────────────────────────────────

void SDDPMethod::prune_inactive_cuts()
{
  m_cut_store_.prune_inactive_cuts(
      m_options_, planning_lp(), m_scene_phase_states_);
}

// ── Cut capping ─────────────────────────────────────────────────────────────

void SDDPMethod::cap_stored_cuts()
{
  m_cut_store_.cap_stored_cuts(m_options_, planning_lp());
}

std::vector<StoredCut> SDDPMethod::build_combined_cuts() const
{
  return m_cut_store_.build_combined_cuts(planning_lp());
}

// ── Cut persistence (delegated to sddp_cut_io.hpp free functions) ───────────

auto SDDPMethod::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  // Single source of truth: build the combined view from per-scene
  // vectors.  The `single_cut_storage` option is no longer load-
  // bearing for storage (only per-scene vectors exist); kept in the
  // options struct for backward compatibility.
  const auto combined = m_cut_store_.build_combined_cuts(planning_lp());
  return save_cuts_csv(combined, planning_lp(), filepath);
}

auto SDDPMethod::save_scene_cuts(SceneIndex scene_index,
                                 const std::string& directory) const
    -> std::expected<void, Error>
{
  return save_scene_cuts_csv(m_cut_store_.scene_cuts()[scene_index],
                             scene_index,
                             scene_uid(scene_index),
                             planning_lp(),
                             directory);
}

auto SDDPMethod::save_all_scene_cuts(const std::string& directory) const
    -> std::expected<void, Error>
{
  const auto num_scenes = planning_lp().simulation().scene_count();

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    auto result = save_scene_cuts(scene_index, directory);
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

auto SDDPMethod::load_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  auto result = load_cuts_csv(planning_lp(),
                              filepath,
                              m_options_.scale_alpha,
                              m_label_maker_,
                              &m_scene_phase_states_);
  // Keep m_iteration_offset_ coherent with whatever was just loaded: the
  // first newly-generated cut must have an iteration_index strictly
  // greater than every loaded one, otherwise save_cuts_for_iteration
  // would stack two cuts under the same index in m_cut_store_.
  if (result.has_value() && result->count > 0) {
    m_iteration_offset_ =
        std::max(m_iteration_offset_, next(result->max_iteration));
  }
  return result;
}

auto SDDPMethod::load_scene_cuts_from_directory(const std::string& directory)
    -> std::expected<CutLoadResult, Error>
{
  return gtopt::load_scene_cuts_from_directory(planning_lp(),
                                               directory,
                                               m_options_.scale_alpha,
                                               m_label_maker_,
                                               &m_scene_phase_states_);
}

auto SDDPMethod::load_boundary_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_boundary_cuts_csv(planning_lp(),
                                filepath,
                                m_options_,
                                m_label_maker_,
                                m_scene_phase_states_);
}

auto SDDPMethod::load_named_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_named_cuts_csv(planning_lp(),
                             filepath,
                             m_options_,
                             m_label_maker_,
                             m_scene_phase_states_);
}

auto SDDPMethod::save_state(const std::string& filepath)
    -> std::expected<void, Error>
{
  return save_state_csv(
      planning_lp(), filepath, IterationIndex {m_current_iteration_.load()});
}

auto SDDPMethod::load_state(const std::string& filepath)
    -> std::expected<void, Error>
{
  return load_state_csv(planning_lp(), filepath);
}

// ── Monitoring API ───────────────────────────────────────────────────────────
// Implementation moved to sddp_monitor.cpp (write_solver_status free fn).
// maybe_write_api_status below builds the snapshot and delegates.

// ─── Private helper method implementations ───────────────────────────────────

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

  // Clamp min_iterations: max_iterations always wins
  m_options_.min_iterations =
      std::min(m_options_.min_iterations, m_options_.max_iterations);

  const auto init_start = std::chrono::steady_clock::now();

  const auto& sim = planning_lp().simulation();
  const auto num_scenes = sim.scene_count();
  const auto num_phases = sim.phase_count();

  SPDLOG_INFO("SDDP: {} scene(s), {} phase(s)", num_scenes, num_phases);

  m_scene_phase_states_.resize(num_scenes);
  m_cut_store_.resize_scenes(num_scenes);
  m_infeasibility_counter_.resize(num_scenes);
  m_max_kappa_.resize(num_scenes);
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

  // Auto-scale alpha: when scale_alpha == 0, compute as the maximum
  // state variable var_scale across all (scene, phase) cells.  This
  // keeps the alpha column's LP coefficient O(1) relative to the
  // largest state variable it is paired with in Benders cut rows.
  if (m_options_.scale_alpha <= 0.0) {
    double max_var_scale = 1.0;
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
        for (const auto& [key, svar] :
             sim.state_variables(scene_index, phase_index))
        {
          max_var_scale = std::max(max_var_scale, svar.var_scale());
        }
      }
    }
    m_options_.scale_alpha = max_var_scale;
    SPDLOG_INFO("SDDP: auto scale_alpha = {:.2e} (max state var_scale)",
                m_options_.scale_alpha);
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
                scene_uid(scene_index),
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

  if (m_options_.recovery_mode >= RecoveryMode::cuts) {
    if (!m_options_.cuts_input_file.empty()) {
      auto result = load_cuts(m_options_.cuts_input_file);
      if (result.has_value()) {
        // load_cuts() already updated m_iteration_offset_ via next() so
        // the first new iteration is strictly past result->max_iteration.
        cuts_count = result->count;
        cuts_source = m_options_.cuts_input_file;
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
  // Missing file = cold-start (silent); parse error = WARN.
  if (!m_options_.boundary_cuts_file.empty()
      && std::filesystem::exists(m_options_.boundary_cuts_file))
  {
    auto result = load_boundary_cuts(m_options_.boundary_cuts_file);
    if (result.has_value()) {
      boundary_count = result->count;
      SPDLOG_DEBUG("SDDP: loaded {} boundary cuts from {}",
                   result->count,
                   m_options_.boundary_cuts_file);
    } else {
      SPDLOG_WARN("SDDP: could not load boundary cuts: {}",
                  result.error().message);
    }
  }

  // ── Load named cuts (all phases, named state variables) ────────────────
  // Named cuts are also part of the problem specification — always load,
  // but do NOT affect the iteration offset.
  if (!m_options_.named_cuts_file.empty()
      && std::filesystem::exists(m_options_.named_cuts_file))
  {
    auto result = load_named_cuts(m_options_.named_cuts_file);
    if (result.has_value()) {
      named_count = result->count;
      SPDLOG_DEBUG("SDDP: loaded {} named cuts from {}",
                   result->count,
                   m_options_.named_cuts_file);
    } else {
      SPDLOG_WARN("SDDP: could not load named cuts: {}",
                  result.error().message);
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
  m_current_iteration_.store(0);
  m_current_gap_.store(1.0);
  m_current_lb_.store(0.0);
  m_current_ub_.store(0.0);
  m_converged_.store(false);
  m_current_pass_.store(0);
  m_scenes_done_.store(0);
}

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
                  scene_uid(scene_index),
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

  if (out.scenes_solved == 0) {
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = "SDDP: all scenes infeasible in forward pass",
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
        if (sc.phase_uid != phase_uid(src_phase_index)) {
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

  // Primary convergence: either gap indicator beats convergence_tol is
  // enough — gap (|UB-LB|/|LB|) OR gap_change (relative LB drift over
  // the look-back window).  We only need ONE gap signal to converge.
  // The min_iterations guard prevents declaring convergence before the
  // solver has accumulated enough cuts (default 2).
  const bool gap_ok = ir.gap < m_options_.convergence_tol;
  const bool gap_change_ok = ir.gap_change < m_options_.convergence_tol
      && ir.gap_change < 1.0;  // 1.0 = sentinel / first iteration
  const bool past_min_iter =
      (iteration_index
       >= m_iteration_offset_ + IterationIndex {m_options_.min_iterations - 1});
  ir.converged = (gap_ok || gap_change_ok) && past_min_iter;

  m_current_iteration_.store(iteration_index);
  m_current_gap_.store(ir.gap);
  m_current_lb_.store(ir.lower_bound);
  m_current_ub_.store(ir.upper_bound);
  m_converged_.store(ir.converged);

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

  const SolverStatusSnapshot snapshot {
      .iteration = m_current_iteration_.load(),
      .gap = m_current_gap_.load(),
      .lower_bound = m_current_lb_.load(),
      .upper_bound = m_current_ub_.load(),
      .converged = m_converged_.load(),
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
                                       m_current_iteration_.load());
}

// ── solve() — now in sddp_iteration.cpp ─────────────────────────────────────

// ─── SDDPPlanningMethod ─────────────────────────────────────────────────────

SDDPPlanningMethod::SDDPPlanningMethod(SDDPOptions opts) noexcept
    : m_sddp_opts_(std::move(opts))
{
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
      SPDLOG_INFO("SDDP: converged (gap={:.6f})", last.gap);
    } else {
      SPDLOG_WARN(
          "SDDP: max_iterations reached without convergence "
          "(gap={:.6f}), returning best solution",
          last.gap);
    }
    return 0;
  }

  return std::unexpected(Error {
      .code = ErrorCode::SolverError,
      .message = "SDDP produced no iteration results",
  });
}

}  // namespace gtopt
