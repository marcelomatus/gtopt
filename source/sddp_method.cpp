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
#include <thread>
#include <utility>
#include <vector>

#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_clone_pool.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
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

CutSharingMode parse_cut_sharing_mode(std::string_view name)
{
  return enum_from_name<CutSharingMode>(name).value_or(CutSharingMode::none);
}

ElasticFilterMode parse_elastic_filter_mode(std::string_view name)
{
  return enum_from_name<ElasticFilterMode>(name).value_or(
      ElasticFilterMode::single_cut);
}

// ─── Free utility functions ──────────────────────────────────────────────────

std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible,
    ProbabilityRescaleMode rescale_mode) noexcept
{
  const auto num_scenes = scene_feasible.size();
  std::vector<double> weights(num_scenes, 0.0);
  double total = 0.0;

  for (const auto [si, feasible] : std::views::enumerate(scene_feasible)) {
    if (feasible == 0U) {
      continue;  // Infeasible → weight stays 0
    }
    if (std::cmp_less(si, scenes.size())) {
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
    for (const auto [si, feasible] : std::views::enumerate(scene_feasible)) {
      if (feasible != 0U) {
        ++feasible_count;
      }
    }
    if (feasible_count > 0) {
      const double eq_w = 1.0 / static_cast<double>(feasible_count);
      for (const auto [si, feasible] : std::views::enumerate(scene_feasible)) {
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

void SDDPMethod::update_max_kappa(SceneIndex scene,
                                  PhaseIndex phase,
                                  const LinearInterface& li,
                                  IterationIndex iteration)
{
  const double kappa = li.get_kappa();

  // Negative kappa means the backend doesn't support the query (e.g.
  // MindOpt returns -1).  Propagate -1 only when no real value was
  // recorded yet; otherwise skip so that real values are preserved.
  if (kappa >= 0.0) {
    m_max_kappa_[scene][phase] = std::max(m_max_kappa_[scene][phase], kappa);
  } else if (m_max_kappa_[scene][phase] < 0.0) {
    m_max_kappa_[scene][phase] = kappa;
  }

  const auto& sim = planning_lp().planning().simulation;
  const auto mode = sim.kappa_warning.value_or(KappaWarningMode::warn);
  if (mode == KappaWarningMode::none || kappa < 0.0) {
    return;
  }

  constexpr double default_kappa_threshold = 1e9;
  const double threshold =
      sim.kappa_threshold.value_or(default_kappa_threshold);
  if (kappa <= threshold) {
    return;
  }

  spdlog::warn("{}: high kappa {:.2e} (threshold {:.2e})",
               sddp_log("Kappa", iteration, scene_uid(scene), phase_uid(phase)),
               kappa,
               threshold);

  const bool should_save_lp =
      (mode == KappaWarningMode::save_lp || mode == KappaWarningMode::diagnose);
  if (should_save_lp && !m_options_.log_directory.empty()) {
    std::filesystem::create_directories(m_options_.log_directory);
    const auto stem = (std::filesystem::path(m_options_.log_directory)
                       / std::format("kappa_sc{}_ph{}_it{}",
                                     scene_uid(scene),
                                     phase_uid(phase),
                                     iteration))
                          .string();
    if (auto r = li.write_lp(stem)) {
      spdlog::warn("Saved high-kappa LP to {}.lp", stem);
    }
  }

  if (mode == KappaWarningMode::diagnose) {
    diagnose_kappa(scene, phase, li, iteration);
  }
}

void SDDPMethod::diagnose_kappa(SceneIndex scene,
                                PhaseIndex phase,
                                const LinearInterface& li,
                                IterationIndex iteration)
{
  const auto prefix =
      sddp_log("Kappa", iteration, scene_uid(scene), phase_uid(phase));

  // Collect cut rows for this (scene, phase) from the cut store
  const auto& scene_cuts = m_cut_store_.scene_cuts();
  const auto phase_uid_val = phase_uid(phase);

  std::vector<RowDiagnostics> cut_diags;

  // Check per-scene cuts
  if (static_cast<size_t>(scene) < scene_cuts.size()) {
    for (const auto& cut : scene_cuts[scene]) {
      if (cut.phase != phase_uid_val) {
        continue;
      }
      cut_diags.push_back(li.diagnose_row(cut.row));
    }
  }

  // Also check combined storage
  {
    const std::scoped_lock lock(m_cut_store_.cuts_mutex());
    for (const auto& cut : m_cut_store_.stored_cuts()) {
      if (cut.phase != phase_uid_val) {
        continue;
      }
      // Avoid duplicates: skip if this row was already diagnosed
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
    , m_label_maker_(planning_lp.options())
{
}

void SDDPMethod::clear_stored_cuts() noexcept
{
  m_cut_store_.clear();
}

void SDDPMethod::forget_first_cuts(int count)
{
  m_cut_store_.forget_first_cuts(count, planning_lp());
}

void SDDPMethod::update_stored_cut_duals()
{
  m_cut_store_.update_stored_cut_duals(planning_lp());
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPMethod::initialize_alpha_variables(SceneIndex scene)
{
  const auto& phases = planning_lp().simulation().phases();

  auto& phase_states = m_scene_phase_states_[scene];
  phase_states.resize(phases.size());

  // Add α (future-cost) variable to every phase except the last
  for (auto&& [pi, _phase] : enumerate<PhaseIndex>(phases)) {
    if (pi == PhaseIndex {phases.size() - 1}) {
      break;
    }
    auto& state = phase_states[pi];
    auto& li = planning_lp().system(scene, pi).linear_interface();

    const auto sa = m_options_.scale_alpha;
    state.alpha_col = li.add_col(sddp_label("sddp", "alpha", scene, pi),
                                 m_options_.alpha_min / sa,
                                 m_options_.alpha_max / sa);
    li.set_obj_coeff(state.alpha_col, sa);
    li.set_col_scale(state.alpha_col, sa);
  }

  // Last phase: no future cost
  phase_states[PhaseIndex {phases.size() - 1}].alpha_col =
      ColIndex {unknown_index};
}

void SDDPMethod::collect_state_variable_links(SceneIndex scene)
{
  const auto& sim = planning_lp().simulation();
  const auto& phases = sim.phases();
  const auto scale_obj = planning_lp().options().scale_objective();

  auto& phase_states = m_scene_phase_states_[scene];

  for (auto&& [phase, _ph] : enumerate<PhaseIndex>(phases)) {
    auto& state = phase_states[phase];

    // Read column bounds from the source phase LP
    const auto& src_li = planning_lp().system(scene, phase).linear_interface();
    const auto col_lo = src_li.get_col_low_raw();
    const auto col_hi = src_li.get_col_upp_raw();

    const auto next_phase = phase + PhaseIndex {1};

    for (const auto& [key, svar] : sim.state_variables(scene, phase)) {
      // Per-variable state cost from StateVariable (set at registration time
      // by ReservoirLP, BatteryLP, etc.).  Pre-divide by scale_objective so
      // it is consistent with the global penalty.
      const auto link_scost =
          (svar.scost() > 0.0) ? svar.scost() / scale_obj : 0.0;

      for (const auto& dep : svar.dependent_variables()) {
        if (dep.phase_index() != next_phase || dep.scene_index() != scene) {
          continue;
        }

        state.outgoing_links.push_back(StateVarLink {
            .source_col = svar.col(),
            .dependent_col = dep.col(),
            .source_phase = phase,
            .target_phase = dep.phase_index(),
            .source_low = col_lo[svar.col()],
            .source_upp = col_hi[svar.col()],
            .var_scale = svar.var_scale(),
            .scost = link_scost,
        });
      }
    }

    SPDLOG_TRACE("SDDP: scene {} phase {} has {} outgoing state-variable links",
                 scene,
                 phase,
                 state.outgoing_links.size());
  }
}

// ── Elastic filter via LP clone (PLP pattern) ───────────────────────────────

std::optional<SDDPMethod::ElasticResult> SDDPMethod::elastic_solve(
    SceneIndex scene, PhaseIndex phase, const SolverOptions& opts)
{
  if (phase == PhaseIndex {0}) {
    return std::nullopt;
  }

  const auto& li = planning_lp().system(scene, phase).linear_interface();
  const auto prev = phase - PhaseIndex {1};
  const auto& prev_state = m_scene_phase_states_[scene][prev];

  // Delegate to BendersCut member (uses work pool when set).
  // Enable warm-start on the clone resolve when configured.
  // Use the previous iteration's forward-pass solution (if any) as hint.
  auto elastic_opts = opts;
  elastic_opts.reuse_basis = m_options_.warm_start;
  elastic_opts.crossover = false;
  const auto& cur_state = m_scene_phase_states_[scene][phase];

  // Scale the elastic penalty by cost_factor so it is consistent with all
  // other LP objective coefficients that go through stage_ecost / cost_factor.
  // The per-variable physical-unit scaling (var_scale) is applied inside
  // relax_fixed_state_variable() using each link's var_scale field.
  const auto scale_obj = planning_lp().options().scale_objective();
  const auto scaled_penalty = m_options_.elastic_penalty / scale_obj;

  auto result = m_benders_cut_.elastic_filter_solve(li,
                                                    prev_state.outgoing_links,
                                                    scaled_penalty,
                                                    elastic_opts,
                                                    cur_state.forward_col_sol,
                                                    cur_state.forward_row_dual);

  if (result.has_value()) {
    SPDLOG_TRACE(
        "SDDP elastic: scene {} phase {} solved via clone "
        "(obj={:.4f})",
        scene,
        phase,
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

bool SDDPMethod::should_dispatch_update_lp(IterationIndex iteration) const
{
  // Three-way logic from the preallocated iteration vector:
  //  - update_lp == false  → explicitly skip
  //  - update_lp == true   → force dispatch (bypass skip count)
  //  - not specified        → default behaviour (respect global skip count)
  if (std::cmp_less(Index {iteration}, m_iterations_.size())) {
    const auto& iter_lp = m_iterations_[iteration];

    if (iter_lp.has_explicit_update_lp()) {
      return iter_lp.should_update_lp();
    }
    // Default: apply global skip count using relative iteration
    const auto skip = planning_lp().options().sddp_update_lp_skip();
    const auto rel =
        static_cast<int>(iteration) - static_cast<int>(m_iteration_offset_);
    if (skip > 0 && rel > 0 && (rel % (skip + 1)) != 0) {
      return false;
    }
  }
  return true;
}

int SDDPMethod::update_lp_for_phase(SceneIndex scene, PhaseIndex phase)
{
  auto& sys = planning_lp().system(scene, phase);

  // Set previous phase's SystemLP so that update_lp elements
  // (seepage, production factor, discharge limit) can look up the
  // previous phase's efin when computing reservoir volume via
  // physical_eini.  In warm_start mode, skip this — physical_eini
  // falls back to the warm solution or vini instead.
  const auto lookup = planning_lp().options().sddp_state_variable_lookup_mode();
  if (phase > PhaseIndex {0} && lookup == StateVariableLookupMode::cross_phase)
  {
    const auto prev = phase - PhaseIndex {1};
    sys.set_prev_phase_sys(&planning_lp().system(scene, prev));
  } else {
    sys.set_prev_phase_sys(nullptr);
  }

  return sys.update_lp();
}

void SDDPMethod::dispatch_update_lp(SceneIndex scene, IterationIndex iteration)
{
  if (!should_dispatch_update_lp(iteration)) {
    return;
  }

  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());

  for (const auto phase : iota_range<PhaseIndex>(0, num_phases)) {
    const auto updated = update_lp_for_phase(scene, phase);

    if (updated > 0) {
      SPDLOG_TRACE(
          "{}: updated {} LP elements",
          sddp_log("Update", iteration, scene_uid(scene), phase_uid(phase)),
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
    IterationIndex iteration, PhaseIndex phase) noexcept
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(
          iteration, SDDPPassDirection::forward, phase, SDDPTaskKind::lp),
      .name = {},
  };
}

BasicTaskRequirements<SDDPTaskKey> make_backward_lp_task_req(
    IterationIndex iteration, PhaseIndex phase) noexcept
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(
          iteration, SDDPPassDirection::backward, phase, SDDPTaskKind::lp),
      .name = {},
  };
}

}  // namespace

// ── forward_pass() — now in sddp_forward_pass.cpp ───────────────────────────

// ── Helper: store a cut for sharing and persistence (thread-safe) ───────────

void SDDPMethod::store_cut(SceneIndex scene,
                           PhaseIndex src_phase,
                           const SparseRow& cut,
                           CutType type,
                           RowIndex row)
{
  m_cut_store_.store_cut(scene,
                         src_phase,
                         cut,
                         type,
                         row,
                         m_options_.single_cut_storage,
                         scene_uid(scene),
                         phase_uid(src_phase));
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

// ── feasibility_backpropagate() — now in sddp_feasibility.cpp ───────────────

// ── Per-phase backward-pass step (optimality cut only; no feasibility sharing)

auto SDDPMethod::backward_pass_single_phase(SceneIndex scene,
                                            PhaseIndex phase,
                                            int cut_offset,
                                            const SolverOptions& opts,
                                            IterationIndex iteration)
    -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene];
  int cuts_added = 0;

  const auto prev_phase = phase - PhaseIndex {1};
  auto& src_li = planning_lp().system(scene, prev_phase).linear_interface();
  const auto& src_state = phase_states[prev_phase];

  // Use cached forward-pass solution for cut generation.
  const auto& target_state = phase_states[phase];

  const auto coeff_mode = m_options_.cut_coeff_mode;

  const auto sa = m_options_.scale_alpha;
  const auto ceps = m_options_.cut_coeff_eps;
  const auto cmax = m_options_.cut_coeff_max;
  // Use row duals when available; fall back to reduced costs when
  // forward_row_dual is empty (e.g. elastic solve cleared it).
  const bool use_row_duals = coeff_mode == CutCoeffMode::row_dual
      && !target_state.forward_row_dual.empty();
  auto cut = use_row_duals
      ? build_benders_cut_from_row_duals(
            src_state.alpha_col,
            src_state.outgoing_links,
            target_state.forward_row_dual,
            target_state.forward_full_obj,
            sddp_label("sddp", "scut", scene, phase, iteration, cut_offset),
            sa,
            ceps)
      : build_benders_cut(
            src_state.alpha_col,
            src_state.outgoing_links,
            target_state.forward_col_cost,
            target_state.forward_full_obj,
            sddp_label("sddp", "scut", scene, phase, iteration, cut_offset),
            sa,
            ceps);
  rescale_benders_cut(cut, src_state.alpha_col, cmax);
  filter_cut_coefficients(cut, src_state.alpha_col, ceps);

  const auto cut_row = src_li.add_row(cut);
  store_cut(scene, prev_phase, cut, CutType::Optimality, cut_row);
  ++cuts_added;
  m_phase_grid_.record(static_cast<int>(iteration),
                       static_cast<int>(scene_uid(scene)),
                       static_cast<int>(phase_uid(phase)),
                       GridCell::Backward);

  SPDLOG_TRACE(
      "{}: cut for phase {} rhs={:.4f}",
      sddp_log("Backward", iteration, scene_uid(scene), phase_uid(phase)),
      phase_uid(prev_phase),
      cut.lowb);

  // Re-solve source and handle iterative feasibility backpropagation.
  // Feasibility cuts are never shared between scenes — they stay local.
  if (phase > PhaseIndex {0}) {
    auto r = src_li.resolve(opts);
    // Track max kappa from backward resolve
    if (r.has_value() && src_li.is_optimal()) {
      update_max_kappa(scene, prev_phase, src_li, iteration);
    }
    if (!r.has_value() || !src_li.is_optimal()) {
      SPDLOG_WARN(
          "{}: non-optimal after cut (status {}), starting feasibility "
          "backpropagation",
          sddp_log(
              "Backward", iteration, scene_uid(scene), phase_uid(prev_phase)),
          src_li.get_status());
      auto bp_result = feasibility_backpropagate(
          scene, prev_phase, cut_offset + cuts_added, opts, iteration);
      if (!bp_result.has_value()) {
        return std::unexpected(std::move(bp_result.error()));
      }
      cuts_added += *bp_result;
    }
  }

  return cuts_added;
}

// ── Backward pass with iterative feasibility backpropagation ────────────────

auto SDDPMethod::backward_pass(SceneIndex scene,
                               const SolverOptions& opts,
                               IterationIndex iteration)
    -> std::expected<int, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  int total_cuts = 0;

  SPDLOG_DEBUG("{}: starting ({} phases)",
               sddp_log("Backward", iteration, scene_uid(scene)),
               num_phases);

  // Iterate backward from last phase to phase 1
  for (const auto phase :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "{}: cancelled",
              sddp_log(
                  "Backward", iteration, scene_uid(scene), phase_uid(phase))),
      });
    }

    auto step_result =
        backward_pass_single_phase(scene, phase, total_cuts, opts, iteration);
    if (!step_result.has_value()) {
      return std::unexpected(std::move(step_result.error()));
    }
    total_cuts += *step_result;
  }

  SPDLOG_DEBUG("{}: done, {} cuts added",
               sddp_log("Backward", iteration, scene_uid(scene)),
               total_cuts);
  return total_cuts;
}

// ── Cut sharing (delegated to sddp_cut_sharing.hpp free function) ───────────

void SDDPMethod::share_cuts_for_phase(
    PhaseIndex phase,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    IterationIndex iteration)
{
  gtopt::share_cuts_for_phase(phase,
                              scene_cuts,
                              m_options_.cut_sharing,
                              planning_lp(),
                              sddp_label("sddp", "share", phase, iteration));
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

// ── Clone pool (now in sddp_clone_pool.hpp/cpp) ─────────────────────────────

// ── Cut persistence (delegated to sddp_cut_io.hpp free functions) ───────────

auto SDDPMethod::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  if (m_options_.single_cut_storage) {
    const auto combined = m_cut_store_.build_combined_cuts(planning_lp());
    return save_cuts_csv(combined, planning_lp(), filepath);
  }
  return save_cuts_csv(m_cut_store_.stored_cuts(), planning_lp(), filepath);
}

auto SDDPMethod::save_scene_cuts(SceneIndex scene,
                                 const std::string& directory) const
    -> std::expected<void, Error>
{
  return save_scene_cuts_csv(m_cut_store_.scene_cuts()[scene],
                             scene,
                             scene_uid(scene),
                             planning_lp(),
                             directory);
}

auto SDDPMethod::save_all_scene_cuts(const std::string& directory) const
    -> std::expected<void, Error>
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    auto result = save_scene_cuts(scene, directory);
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

auto SDDPMethod::load_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_cuts_csv(
      planning_lp(), filepath, m_options_.scale_alpha, m_label_maker_);
}

auto SDDPMethod::load_scene_cuts_from_directory(const std::string& directory)
    -> std::expected<CutLoadResult, Error>
{
  return gtopt::load_scene_cuts_from_directory(
      planning_lp(), directory, m_options_.scale_alpha, m_label_maker_);
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

auto SDDPMethod::save_state(const std::string& filepath) const
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
  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());

  SPDLOG_INFO("SDDP: {} scene(s), {} phase(s)", num_scenes, num_phases);

  m_scene_phase_states_.resize(num_scenes);
  m_cut_store_.resize_scenes(num_scenes);
  m_infeasibility_counter_.resize(num_scenes);
  m_max_kappa_.resize(num_scenes);
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    m_infeasibility_counter_[scene].resize(num_phases, 0);
    m_max_kappa_[scene].resize(num_phases, -1.0);
  }

  // Auto-scale alpha: when scale_alpha == 0, compute as the maximum
  // state variable var_scale across all phases.  This ensures the
  // alpha LP variable is O(1) relative to the largest state variable.
  if (m_options_.scale_alpha <= 0.0) {
    double max_var_scale = 1.0;
    for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
      for (auto&& [phase, _ph] : enumerate<PhaseIndex>(sim.phases())) {
        for (const auto& [key, svar] : sim.state_variables(scene, phase)) {
          max_var_scale = std::max(max_var_scale, svar.var_scale());
        }
      }
    }
    m_options_.scale_alpha = max_var_scale;
    SPDLOG_INFO("SDDP: auto scale_alpha = {:.2e} (max state var_scale)",
                m_options_.scale_alpha);
  }

  SPDLOG_INFO("SDDP: adding alpha variables and collecting state links");
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    initialize_alpha_variables(scene);
    collect_state_variable_links(scene);
    SPDLOG_DEBUG("SDDP: scene {} initialized ({} state links)",
                 scene_uid(scene),
                 m_scene_phase_states_[scene].empty()
                     ? 0
                     : m_scene_phase_states_[scene][PhaseIndex {0}]
                           .outgoing_links.size());
  }

  // Save per-(scene, phase) base row counts before any cuts are loaded.
  // Rows below this threshold are structural constraints and are never
  // pruned; rows above it are Benders cuts (including hot-start cuts).
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    for (const auto phase : iota_range<PhaseIndex>(0, num_phases)) {
      auto& li = planning_lp().system(scene, phase).linear_interface();
      li.save_base_numrows();
      m_scene_phase_states_[scene][phase].base_nrows = li.base_numrows();
    }
  }

  // ── Initialize clone pool (skipped in simulation mode) ───────────────────
  if (m_options_.use_clone_pool && m_options_.max_iterations > 0) {
    m_clone_pool_.allocate(num_scenes, num_phases);
    SPDLOG_DEBUG("SDDP: clone pool allocated for {} scene×phase slots",
                 static_cast<std::size_t>(num_scenes)
                     * static_cast<std::size_t>(num_phases));
  }

  // ── Load hot-start cuts and track max iteration for offset ────────────────
  // Recovery cuts (explicit input, scene recovery) require
  // recovery_mode >= cuts.  Without --recover, the solver starts cold.
  m_iteration_offset_ = IterationIndex {};

  if (m_options_.recovery_mode >= RecoveryMode::cuts) {
    if (!m_options_.cuts_input_file.empty()) {
      auto result = load_cuts(m_options_.cuts_input_file);
      if (result.has_value()) {
        m_iteration_offset_ =
            std::max(m_iteration_offset_, result->max_iteration);
        SPDLOG_INFO("SDDP hot-start: loaded {} cuts (max_iter={})",
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
          m_iteration_offset_ = std::max(
              m_iteration_offset_, IterationIndex {result->max_iteration});
          SPDLOG_INFO("SDDP hot-start: loaded {} cuts from {} (max_iter={})",
                      result->count,
                      cut_dir.string(),
                      result->max_iteration);
        }
      }
    }
  }

  // ── Load boundary cuts (future-cost function for last phase) ──────────
  // Boundary cuts are part of the problem specification (analogous to
  // PLP's "planos de embalse"), NOT recovery state.  They do NOT
  // affect the iteration offset — the solver always starts from
  // iteration 0 (or from the recovery offset if recovery cuts exist).
  if (!m_options_.boundary_cuts_file.empty()) {
    auto result = load_boundary_cuts(m_options_.boundary_cuts_file);
    if (result.has_value()) {
      SPDLOG_INFO("SDDP: loaded {} boundary cuts from {}",
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
  if (!m_options_.named_cuts_file.empty()) {
    auto result = load_named_cuts(m_options_.named_cuts_file);
    if (result.has_value()) {
      SPDLOG_INFO("SDDP: loaded {} named cuts from {}",
                  result->count,
                  m_options_.named_cuts_file);
    } else {
      SPDLOG_WARN("SDDP: could not load named cuts: {}",
                  result.error().message);
    }
  }

  if (m_iteration_offset_ > 0) {
    SPDLOG_INFO("SDDP: iteration offset set to {} from hot-start cuts",
                m_iteration_offset_);
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
        SPDLOG_INFO("SDDP: loaded state variables from {}", state_file);
      } else {
        SPDLOG_WARN("SDDP: could not load state variables: {}",
                    result.error().message);
      }
    }
  }

  // ── Build preallocated iteration vector ───────────────────────────────────
  {
    const auto total_iterations =
        static_cast<Index>(m_iteration_offset_) + m_options_.max_iterations;
    m_iterations_.resize(total_iterations);
    for (auto i = Index {0}; i < total_iterations; ++i) {
      m_iterations_[IterationIndex {i}] = IterationLP {
          Iteration {
              .index = i,
          },
          IterationIndex {i},
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
                                             IterationIndex iter)
    -> std::expected<ForwardPassOutcome, Error>
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  m_current_pass_.store(1);
  m_scenes_done_.store(0);

  SPDLOG_INFO("{}: dispatching {} scene(s) to work pool",
              sddp_log("Forward", iter),
              num_scenes);

  const auto fwd_start = std::chrono::steady_clock::now();
  std::vector<std::future<std::expected<double, Error>>> futures;
  futures.reserve(num_scenes);

  // Forward-pass scene tasks use High priority; lower iteration = higher key.
  const auto fwd_req = make_forward_lp_task_req(iter, PhaseIndex {0});

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    auto fut = pool.submit([this, scene, iter, &opts]
                           { return forward_pass(scene, opts, iter); },
                           fwd_req);
    futures.push_back(std::move(fut.value()));
  }

  ForwardPassOutcome out;
  out.scene_upper_bounds.resize(num_scenes, 0.0);
  out.scene_feasible.resize(num_scenes, 1);

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    const auto si_sz = static_cast<std::size_t>(scene);
    auto fwd = futures[si_sz].get();
    if (!fwd.has_value()) {
      SPDLOG_WARN("{}: failed: {}",
                  sddp_log("Forward", iter, scene_uid(scene)),
                  fwd.error().message);
      out.has_feasibility_issue = true;
      out.scene_feasible[si_sz] = 0;
      m_scenes_done_.fetch_add(1);
      continue;
    }
    out.scene_upper_bounds[si_sz] = *fwd;
    ++out.scenes_solved;
    m_scenes_done_.fetch_add(1);
    if ((scene + 1) % 4 == 0 || scene + 1 == num_scenes) {
      SPDLOG_DEBUG("{}: {}/{} scenes completed",
                   sddp_log("Forward", iter),
                   scene + 1,
                   num_scenes);
    }
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - fwd_start)
                      .count();

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
    IterationIndex iter) -> BackwardPassOutcome
{
  // Enable warm-start for backward pass LP re-solves.  After adding a single
  // cut row, the previous basis is still near-optimal — dual simplex handles
  // this in very few pivots.  This is especially important when barrier is the
  // default algorithm, since barrier would ignore the basis entirely.
  auto bwd_opts = opts;
  if (m_options_.warm_start) {
    bwd_opts.reuse_basis = true;
  }

  m_current_pass_.store(2);
  m_scenes_done_.store(0);

  // When cut sharing is enabled, use the phase-synchronized backward pass:
  // all scenes complete a phase before cuts are shared and the next phase
  // is processed.  When sharing is disabled (None), scenes run their full
  // backward pass independently in parallel with no synchronization.
  if (m_options_.cut_sharing != CutSharingMode::none) {
    auto result =
        run_backward_pass_synchronized(scene_feasible, pool, bwd_opts, iter);
    m_current_pass_.store(0);
    return result;
  }

  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  SPDLOG_INFO(
      "{}: dispatching {} scene(s) to work pool "
      "(cut_sharing=none, apertures={})",
      sddp_log("Backward", iter),
      num_scenes,
      !m_options_.apertures || !m_options_.apertures->empty() ? "enabled"
                                                              : "disabled");

  const auto bwd_start = std::chrono::steady_clock::now();
  std::vector<std::future<std::expected<int, Error>>> futures;
  futures.reserve(num_scenes);

  // Backward-pass scene tasks use Medium priority; scenes with lower index
  // get slightly higher priority_key (phase 0 = lowest phase index).
  const auto bwd_req = make_backward_lp_task_req(iter, PhaseIndex {0});

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    if (scene_feasible[static_cast<std::size_t>(scene)] == 0U) {
      continue;
    }
    const bool use_ap = !m_options_.apertures || !m_options_.apertures->empty();
    auto fut = use_ap
        ? pool.submit(
              [this, scene, &bwd_opts, iter]
              { return backward_pass_with_apertures(scene, bwd_opts, iter); },
              bwd_req)
        : pool.submit([this, scene, &bwd_opts, iter]
                      { return backward_pass(scene, bwd_opts, iter); },
                      bwd_req);
    futures.push_back(std::move(fut.value()));
  }

  BackwardPassOutcome out;
  int bwd_done = 0;
  const auto bwd_total = static_cast<int>(futures.size());
  for (auto& fut : futures) {
    auto bwd = fut.get();
    ++bwd_done;
    if (!bwd.has_value()) {
      SPDLOG_WARN(
          "{}: failed: {}", sddp_log("Backward", iter), bwd.error().message);
      out.has_feasibility_issue = true;
      m_scenes_done_.fetch_add(1);
      continue;
    }
    out.total_cuts += *bwd;
    m_scenes_done_.fetch_add(1);
    if (bwd_done % 4 == 0 || bwd_done == bwd_total) {
      SPDLOG_DEBUG("{}: {}/{} scenes completed",
                   sddp_log("Backward", iter),
                   bwd_done,
                   bwd_total);
    }
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
    IterationIndex iter) -> BackwardPassOutcome
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());

  const auto bwd_start = std::chrono::steady_clock::now();
  BackwardPassOutcome out;

  // Per-scene cumulative cut count for unique cut labels across phase steps
  std::vector<int> per_scene_cut_count(static_cast<std::size_t>(num_scenes), 0);

  // Apertures enabled unless explicitly set to empty array
  const bool use_apertures =
      !m_options_.apertures || !m_options_.apertures->empty();

  // Process phases backward: all scenes complete one phase before
  // sharing cuts and moving to the previous phase.
  for (const auto phase :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    const auto cuts_before_step = m_cut_store_.stored_cuts().size();

    // Submit all feasible scenes for this phase step in parallel
    std::vector<std::pair<SceneIndex, std::future<std::expected<int, Error>>>>
        futures;
    futures.reserve(num_scenes);

    const auto bwd_req = make_backward_lp_task_req(iter, phase);

    for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
      if (scene_feasible[static_cast<std::size_t>(scene)] == 0U) {
        continue;
      }
      const int offset = per_scene_cut_count[static_cast<std::size_t>(scene)];

      auto fut = use_apertures
          ? pool.submit(
                [this, scene, phase, offset, &opts, iter]
                {
                  return backward_pass_with_apertures_single_phase(
                      scene, phase, offset, opts, iter);
                },
                bwd_req)
          : pool.submit(
                [this, scene, phase, offset, &opts, iter]
                {
                  return backward_pass_single_phase(
                      scene, phase, offset, opts, iter);
                },
                bwd_req);
      futures.emplace_back(scene, std::move(fut.value()));
    }

    // Wait for all scenes to complete this phase step
    for (auto& [scene, fut] : futures) {
      auto step_result = fut.get();
      if (!step_result.has_value()) {
        SPDLOG_WARN("SDDP backward synchronized: scene {} phase {} failed: {}",
                    scene,
                    phase,
                    step_result.error().message);
        out.has_feasibility_issue = true;
        m_scenes_done_.fetch_add(1);
        continue;
      }
      out.total_cuts += *step_result;
      per_scene_cut_count[static_cast<std::size_t>(scene)] += *step_result;
      m_scenes_done_.fetch_add(1);
    }

    // Share optimality cuts generated in this phase step across all scenes.
    // Feasibility cuts are stored but only optimality cuts are shared.
    const auto src_phase = phase - PhaseIndex {1};

    StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
    scene_cuts.resize(num_scenes);

    // Build scene UID → SceneIndex lookup for cut sharing
    flat_map<SceneUid, SceneIndex> scene_uid_map;
    const auto& scenes = planning_lp().simulation().scenes();
    for (auto&& [si, sc_lp] : enumerate<SceneIndex>(scenes)) {
      scene_uid_map[sc_lp.uid()] = si;
    }

    {
      const std::scoped_lock lock(m_cut_store_.cuts_mutex());
      for (std::size_t ci = cuts_before_step;
           ci < m_cut_store_.stored_cuts().size();
           ++ci)
      {
        const auto& sc = m_cut_store_.stored_cuts()[ci];
        // Only share optimality cuts; feasibility cuts stay local
        if (sc.type != CutType::Optimality) {
          continue;
        }
        if (sc.phase != phase_uid(src_phase)) {
          continue;
        }
        auto row = SparseRow {
            .name = sc.name,
            .lowb = sc.rhs,
            .uppb = LinearProblem::DblMax,
        };
        for (const auto& [col, coeff] : sc.coefficients) {
          row[ColIndex {col}] = coeff;
        }
        auto sit = scene_uid_map.find(sc.scene);
        if (sit != scene_uid_map.end()) {
          scene_cuts[sit->second].push_back(std::move(row));
        }
      }
    }

    share_cuts_for_phase(src_phase, scene_cuts, iter);

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
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  double weighted_upper = 0.0;
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    const auto si_sz = static_cast<std::size_t>(scene);
    weighted_upper += weights[si_sz] * ir.scene_upper_bounds[si_sz];
  }
  ir.upper_bound = weighted_upper;

  ir.scene_lower_bounds.resize(num_scenes, 0.0);
  double weighted_lower = 0.0;
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    const auto si_sz = static_cast<std::size_t>(scene);
    if (scene_feasible[si_sz] == 0U) {
      continue;
    }
    const double lb_si = planning_lp()
                             .system(scene, PhaseIndex {0})
                             .linear_interface()
                             .get_obj_value();
    ir.scene_lower_bounds[si_sz] = lb_si;
    weighted_lower += weights[si_sz] * lb_si;
  }
  ir.lower_bound = weighted_lower;
}

void SDDPMethod::apply_cut_sharing_for_iteration(std::size_t cuts_before,
                                                 IterationIndex iteration)
{
  m_cut_store_.apply_cut_sharing_for_iteration(
      cuts_before, iteration, m_options_, planning_lp(), m_label_maker_);
}

void SDDPMethod::finalize_iteration_result(SDDPIterationResult& ir,
                                           IterationIndex iter)
{
  ir.gap = compute_convergence_gap(ir.upper_bound, ir.lower_bound);
  // Only declare convergence if both the gap tolerance is met AND
  // we have completed at least min_iterations (default 2).
  ir.converged = (ir.gap < m_options_.convergence_tol)
      && (iter >= m_iteration_offset_
              + IterationIndex {m_options_.min_iterations - 1});

  m_current_iteration_.store(iter);
  m_current_gap_.store(ir.gap);
  m_current_lb_.store(ir.lower_bound);
  m_current_ub_.store(ir.upper_bound);
  m_converged_.store(ir.converged);

  SPDLOG_TRACE(
      "SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} gap_change={:.6f} "
      "cuts={} infeas_cuts={} fwd={:.3f}s bwd={:.3f}s total={:.3f}s{}",
      iter,
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

  SPDLOG_INFO("SDDP iter {}: gap={:.6f} gap_change={:.6f} ({:.3f}s){}",
              iter,
              ir.gap,
              ir.gap_change,
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
    IterationIndex iter, std::span<const uint8_t> scene_feasible)
{
  m_cut_store_.save_cuts_for_iteration(iter,
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

  // lp_only: LP already built in PlanningLP constructor — skip all
  // solving.
  if (m_sddp_opts_.lp_only) {
    SPDLOG_INFO("SDDP: lp_only mode — LP built, skipping solve");
    return 0;
  }

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
    const int training_iters = static_cast<int>(m_last_results_.size()) > 1
        ? static_cast<int>(m_last_results_.size()) - 1
        : static_cast<int>(m_last_results_.size());
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
