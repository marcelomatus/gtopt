/**
 * @file      sddp_solver.cpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) solver implementation
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the forward/backward iterative decomposition with multi-scene
 * support, iterative feasibility backpropagation, and optimality cut sharing.
 * See sddp_solver.hpp for the algorithm description and the free-function
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
#include <gtopt/sddp_monitor.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/sddp_solver.hpp>
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
  return cut_sharing_mode_from_name(name).value_or(CutSharingMode::none);
}

ElasticFilterMode parse_elastic_filter_mode(std::string_view name)
{
  return elastic_filter_mode_from_name(name).value_or(
      ElasticFilterMode::single_cut);
}

// ─── Free utility functions ──────────────────────────────────────────────────

std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible) noexcept
{
  const auto num_scenes = static_cast<int>(scene_feasible.size());
  std::vector<double> weights(static_cast<std::size_t>(num_scenes), 0.0);
  double total = 0.0;

  for (int si = 0; si < num_scenes; ++si) {
    if (scene_feasible[static_cast<std::size_t>(si)] == 0U) {
      continue;  // Infeasible → weight stays 0
    }
    if (std::cmp_less(si, scenes.size())) {
      for (const auto& sc : scenes[static_cast<std::size_t>(si)].scenarios()) {
        weights[static_cast<std::size_t>(si)] += sc.probability_factor();
      }
    }
    if (weights[static_cast<std::size_t>(si)] <= 0.0) {
      weights[static_cast<std::size_t>(si)] = 1.0;  // fallback equal weight
    }
    total += weights[static_cast<std::size_t>(si)];
  }

  if (total > 0.0) {
    for (auto& w : weights) {
      w /= total;
    }
  } else {
    // All infeasible or zero probability → equal weight among feasible
    int feasible_count = 0;
    for (int si = 0; si < num_scenes; ++si) {
      if (scene_feasible[static_cast<std::size_t>(si)] != 0U) {
        ++feasible_count;
      }
    }
    if (feasible_count > 0) {
      const double eq_w = 1.0 / static_cast<double>(feasible_count);
      for (int si = 0; si < num_scenes; ++si) {
        if (scene_feasible[static_cast<std::size_t>(si)] != 0U) {
          weights[static_cast<std::size_t>(si)] = eq_w;
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

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

SDDPSolver::SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts) noexcept
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

void SDDPSolver::clear_stored_cuts() noexcept
{
  {
    const std::scoped_lock lk(m_cuts_mutex_);
    m_stored_cuts_.clear();
  }
  for (auto& sc : m_scene_cuts_) {
    sc.clear();
  }
}

void SDDPSolver::forget_first_cuts(int count)
{
  if (count <= 0) {
    return;
  }

  const auto phase_map = build_phase_uid_map(planning_lp());
  const auto scene_map = build_scene_uid_map(planning_lp());

  using ScenePhaseKey = std::pair<SceneIndex, PhaseIndex>;

  // Helper: resolve a stored cut's (scene, phase) key.
  auto resolve_key = [&](const StoredCut& cut) -> std::optional<ScenePhaseKey>
  {
    auto pit = phase_map.find(cut.phase);
    auto sit = scene_map.find(cut.scene);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return std::nullopt;
    }
    return ScenePhaseKey {sit->second, pit->second};
  };

  // Collect LP rows to delete per (scene, phase) and cut names to forget.
  std::map<ScenePhaseKey, std::vector<int>> rows_to_delete;
  std::set<std::string> names_to_forget;

  int n = 0;
  {
    const std::scoped_lock lk(m_cuts_mutex_);
    n = std::min(count, static_cast<int>(m_stored_cuts_.size()));
    for (const auto& cut : m_stored_cuts_ | std::views::take(n)) {
      if (!cut.name.empty()) {
        names_to_forget.insert(cut.name);
      }
      if (auto key = resolve_key(cut)) {
        rows_to_delete[*key].push_back(static_cast<int>(cut.row));
      }
    }
    m_stored_cuts_.erase(m_stored_cuts_.begin(), m_stored_cuts_.begin() + n);
  }

  // Delete LP rows and track deletion count per (scene, phase).
  std::map<ScenePhaseKey, int> deleted_count;
  int total_deleted = 0;
  for (auto& [key, rows] : rows_to_delete) {
    auto& li = planning_lp().system(key.first, key.second).linear_interface();
    std::ranges::sort(rows);
    li.delete_rows(rows);
    deleted_count[key] = static_cast<int>(rows.size());
    total_deleted += static_cast<int>(rows.size());
  }

  // Helper: shift a cut's row index by the number of deleted rows
  // in its (scene, phase).
  auto shift_row = [&](StoredCut& cut)
  {
    if (auto key = resolve_key(cut)) {
      if (auto it = deleted_count.find(*key); it != deleted_count.end()) {
        cut.row -= RowIndex {it->second};
      }
    }
  };

  // Update row indices in remaining m_stored_cuts_.
  {
    const std::scoped_lock lk(m_cuts_mutex_);
    std::ranges::for_each(m_stored_cuts_, shift_row);
  }

  // Remove forgotten cuts from m_scene_cuts_ and update row indices.
  for (auto&& [si, cuts] : enumerate<SceneIndex>(m_scene_cuts_)) {
    std::erase_if(
        cuts,
        [&](const StoredCut& c)
        { return !c.name.empty() && names_to_forget.contains(c.name); });
    std::ranges::for_each(cuts, shift_row);
  }

  SPDLOG_INFO(
      "SDDP: forgot {} inherited cuts ({} LP rows deleted)", n, total_deleted);
}

void SDDPSolver::update_stored_cut_duals()
{
  const auto phase_map = build_phase_uid_map(planning_lp());
  const auto scene_map = build_scene_uid_map(planning_lp());

  auto update_dual = [&](StoredCut& cut)
  {
    auto pit = phase_map.find(cut.phase);
    auto sit = scene_map.find(cut.scene);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return;
    }
    const auto& li =
        planning_lp().system(sit->second, pit->second).linear_interface();
    const auto row_idx = static_cast<std::size_t>(cut.row);
    const auto duals = li.get_row_dual();
    if (row_idx < duals.size()) {
      cut.dual = duals[row_idx];
    }
  };

  {
    const std::scoped_lock lk(m_cuts_mutex_);
    std::ranges::for_each(m_stored_cuts_, update_dual);
  }
  for (auto& sc : m_scene_cuts_) {
    std::ranges::for_each(sc, update_dual);
  }
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPSolver::initialize_alpha_variables(SceneIndex scene)
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

    state.alpha_col = li.add_col(sddp_label("sddp", "alpha", scene, pi),
                                 m_options_.alpha_min,
                                 m_options_.alpha_max);
    li.set_obj_coeff(state.alpha_col, 1.0);
  }

  // Last phase: no future cost
  phase_states[PhaseIndex {phases.size() - 1}].alpha_col =
      ColIndex {unknown_index};
}

void SDDPSolver::collect_state_variable_links(SceneIndex scene)
{
  const auto& sim = planning_lp().simulation();
  const auto& phases = sim.phases();

  auto& phase_states = m_scene_phase_states_[scene];

  for (auto&& [phase, _ph] : enumerate<PhaseIndex>(phases)) {
    auto& state = phase_states[phase];

    // Read column bounds from the source phase LP
    const auto& src_li = planning_lp().system(scene, phase).linear_interface();
    const auto col_lo = src_li.get_col_low();
    const auto col_hi = src_li.get_col_upp();

    const auto next_phase = phase + PhaseIndex {1};

    for (const auto& [key, svar] : sim.state_variables(scene, phase)) {
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

std::optional<SDDPSolver::ElasticResult> SDDPSolver::elastic_solve(
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
  elastic_opts.warm_start = m_options_.warm_start;
  const auto& cur_state = m_scene_phase_states_[scene][phase];

  auto result = m_benders_cut_.elastic_filter_solve(li,
                                                    prev_state.outgoing_links,
                                                    m_options_.elastic_penalty,
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

bool SDDPSolver::check_sentinel_stop() const
{
  if (m_options_.sentinel_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.sentinel_file);
}

bool SDDPSolver::check_api_stop_request() const
{
  if (m_options_.api_stop_request_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.api_stop_request_file);
}

bool SDDPSolver::should_stop() const
{
  if (m_in_simulation_) {
    return false;  // simulation pass always runs to completion
  }
  return m_stop_requested_.load() || check_sentinel_stop()
      || check_api_stop_request();
}

// ── Coefficient updates ─────────────────────────────────────────────────────

void SDDPSolver::update_coefficients_for_phase(SceneIndex scene,
                                               PhaseIndex phase,
                                               IterationIndex iteration)
{
  auto& sys = planning_lp().system(scene, phase);

  const auto updated =
      update_lp_coefficients(sys, planning_lp().options(), iteration, phase);

  if (updated > 0) {
    SPDLOG_TRACE(
        "SDDP: updated {} LP coefficients for scene {} phase {} (iter {})",
        updated,
        scene,
        phase,
        iteration);
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

void SDDPSolver::store_cut(SceneIndex scene,
                           PhaseIndex src_phase,
                           const SparseRow& cut,
                           CutType type,
                           RowIndex row)
{
  StoredCut stored {
      .type = type,
      .phase = phase_uid(src_phase),
      .scene = scene_uid(scene),
      .name = cut.name,
      .rhs = cut.lowb,
      .row = row,
  };
  for (const auto& [col, coeff] : cut.cmap) {
    stored.coefficients.emplace_back(static_cast<int>(col), coeff);
  }
  // Per-scene storage: no lock needed (each scene writes its own vector)
  if (m_options_.single_cut_storage) {
    m_scene_cuts_[scene].push_back(std::move(stored));
  } else {
    m_scene_cuts_[scene].push_back(stored);
    const std::scoped_lock lock(m_cuts_mutex_);
    m_stored_cuts_.push_back(std::move(stored));
  }
}

// ── Helper: resolve an LP via the work pool (avoids naked direct calls) ─────

auto SDDPSolver::resolve_via_pool(
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

auto SDDPSolver::resolve_clone_via_pool(
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

auto SDDPSolver::backward_pass_single_phase(SceneIndex scene,
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

  auto cut = build_benders_cut(
      src_state.alpha_col,
      src_state.outgoing_links,
      target_state.forward_col_cost,
      target_state.forward_full_obj,
      sddp_label("sddp", "scut", scene, phase, iteration, cut_offset));

  const auto cut_row = src_li.add_row(cut);
  store_cut(scene, prev_phase, cut, CutType::Optimality, cut_row);
  ++cuts_added;

  SPDLOG_TRACE("SDDP backward: scene {} cut for phase {} rhs={:.4f}",
               scene_uid(scene),
               phase_uid(prev_phase),
               cut.lowb);

  // Re-solve source and handle iterative feasibility backpropagation.
  // Feasibility cuts are never shared between scenes — they stay local.
  if (phase > PhaseIndex {0}) {
    auto r = src_li.resolve(opts);
    if (!r.has_value() || !src_li.is_optimal()) {
      SPDLOG_WARN(
          "SDDP backward: iter {} scene {} phase {} non-optimal after cut "
          "(status {}), starting feasibility backpropagation",
          iteration,
          scene_uid(scene),
          phase_uid(prev_phase),
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

auto SDDPSolver::backward_pass(SceneIndex scene,
                               const SolverOptions& opts,
                               IterationIndex iteration)
    -> std::expected<int, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  int total_cuts = 0;

  SPDLOG_DEBUG("SDDP backward: scene {} iter {} starting ({} phases)",
               scene_uid(scene),
               iteration,
               num_phases);

  // Iterate backward from last phase to phase 1
  for (const auto phase :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message =
              std::format("SDDP backward: cancelled at scene {} phase {}",
                          scene_uid(scene),
                          phase_uid(phase)),
      });
    }

    auto step_result =
        backward_pass_single_phase(scene, phase, total_cuts, opts, iteration);
    if (!step_result.has_value()) {
      return std::unexpected(std::move(step_result.error()));
    }
    total_cuts += *step_result;
  }

  SPDLOG_DEBUG("SDDP backward: scene {} iter {} done, {} cuts added",
               scene_uid(scene),
               iteration,
               total_cuts);
  return total_cuts;
}

// ── Cut sharing (delegated to sddp_cut_sharing.hpp free function) ───────────

void SDDPSolver::share_cuts_for_phase(
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

void SDDPSolver::prune_inactive_cuts()
{
  const auto max_cuts = m_options_.max_cuts_per_phase;
  if (max_cuts <= 0) {
    return;
  }

  const auto threshold = m_options_.prune_dual_threshold;
  int total_pruned = 0;

  // Track deleted rows per (scene, phase) so we can update stored cuts.
  using ScenePhaseKey = std::pair<SceneIndex, PhaseIndex>;
  std::map<ScenePhaseKey, std::vector<Index>> all_deleted;

  for (auto&& [scene, scene_states] :
       enumerate<SceneIndex>(m_scene_phase_states_))
  {
    for (auto&& [phase, psi] : enumerate<PhaseIndex>(scene_states)) {
      auto& li = planning_lp().system(scene, phase).linear_interface();

      const auto total_rows = static_cast<Index>(li.get_numrows());
      const auto base = static_cast<Index>(psi.base_nrows);
      const auto num_cut_rows = total_rows - base;
      if (num_cut_rows <= max_cuts) {
        continue;
      }

      const auto duals = li.get_row_dual();

      // Build (row_index, |dual|) for cut rows, sorted by |dual| ascending
      struct CutInfo
      {
        int row;
        double abs_dual;
      };
      std::vector<CutInfo> cut_infos;
      cut_infos.reserve(total_rows - base);
      for (auto r = base; r < total_rows; ++r) {
        cut_infos.push_back(CutInfo {
            .row = r,
            .abs_dual = std::abs(duals[r]),
        });
      }
      std::ranges::sort(cut_infos, {}, &CutInfo::abs_dual);

      const auto to_remove = num_cut_rows - max_cuts;
      std::vector<Index> rows_to_delete;
      rows_to_delete.reserve(to_remove);
      for (const auto& ci : cut_infos | std::views::take(to_remove)) {
        if (ci.abs_dual < threshold) {
          rows_to_delete.push_back(ci.row);
        }
      }

      if (rows_to_delete.empty()) {
        continue;
      }

      std::ranges::sort(rows_to_delete);

      SPDLOG_DEBUG(
          "SDDP: pruning {} inactive cuts from scene {} phase {} "
          "({} cut rows, max {})",
          rows_to_delete.size(),
          scene_uid(scene),
          phase_uid(phase),
          num_cut_rows,
          max_cuts);

      li.delete_rows(rows_to_delete);
      total_pruned += static_cast<int>(rows_to_delete.size());
      all_deleted[{scene, phase}] = std::move(rows_to_delete);
    }
  }

  if (total_pruned == 0) {
    return;
  }

  // Build UID → index lookups for stored cut matching.
  const auto phase_map = build_phase_uid_map(planning_lp());
  const auto scene_map = build_scene_uid_map(planning_lp());

  // Helper: resolve a stored cut to its (scene, phase) key.
  auto resolve_key = [&](const StoredCut& cut) -> std::optional<ScenePhaseKey>
  {
    auto pit = phase_map.find(cut.phase);
    auto sit = scene_map.find(cut.scene);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return std::nullopt;
    }
    return ScenePhaseKey {sit->second, pit->second};
  };

  // Helper: find the deleted rows vector for a stored cut.
  auto find_deleted = [&](const StoredCut& cut) -> const std::vector<Index>*
  {
    if (auto key = resolve_key(cut)) {
      if (auto it = all_deleted.find(*key); it != all_deleted.end()) {
        return &it->second;
      }
    }
    return nullptr;
  };

  // Helper: check if a row was deleted.
  auto was_deleted = [](const std::vector<Index>& deleted, RowIndex row) -> bool
  { return std::ranges::binary_search(deleted, static_cast<Index>(row)); };

  // Helper: compute row shift (number of deleted rows below this one).
  auto compute_shift = [](const std::vector<Index>& deleted,
                          RowIndex row) -> RowIndex
  {
    return RowIndex {static_cast<Index>(
        std::ranges::lower_bound(deleted, static_cast<Index>(row) + 1)
        - deleted.begin())};
  };

  // Helper: erase pruned cuts and shift remaining row indices.
  auto update_cuts = [&](std::vector<StoredCut>& cuts)
  {
    std::erase_if(cuts,
                  [&](const StoredCut& cut)
                  {
                    const auto* del = find_deleted(cut);
                    return del != nullptr && was_deleted(*del, cut.row);
                  });
    for (auto& cut : cuts) {
      if (const auto* del = find_deleted(cut)) {
        cut.row -= compute_shift(*del, cut.row);
      }
    }
  };

  {
    const std::scoped_lock lk(m_cuts_mutex_);
    update_cuts(m_stored_cuts_);
  }
  for (auto& sc : m_scene_cuts_) {
    update_cuts(sc);
  }

  SPDLOG_INFO("SDDP: pruned {} inactive cuts across all LPs", total_pruned);
}

// ── Cut capping ─────────────────────────────────────────────────────────────

void SDDPSolver::cap_stored_cuts()
{
  const auto max_cuts = m_options_.max_stored_cuts;
  if (max_cuts <= 0) {
    return;
  }
  const auto limit = static_cast<std::size_t>(max_cuts);

  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  int total_dropped = 0;

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    auto& cuts = m_scene_cuts_[scene];
    if (cuts.size() > limit) {
      const auto excess = cuts.size() - limit;
      cuts.erase(cuts.begin(),
                 cuts.begin() + static_cast<std::ptrdiff_t>(excess));
      total_dropped += static_cast<int>(excess);
    }
  }

  // Also cap the shared storage when not using single_cut_storage
  if (!m_options_.single_cut_storage) {
    const std::scoped_lock lock(m_cuts_mutex_);
    const auto total_limit = limit * static_cast<std::size_t>(num_scenes);
    if (m_stored_cuts_.size() > total_limit) {
      const auto excess = m_stored_cuts_.size() - total_limit;
      m_stored_cuts_.erase(
          m_stored_cuts_.begin(),
          m_stored_cuts_.begin() + static_cast<std::ptrdiff_t>(excess));
    }
  }

  if (total_dropped > 0) {
    SPDLOG_INFO("SDDP: capped stored cuts, dropped {} oldest entries",
                total_dropped);
  }
}

std::vector<StoredCut> SDDPSolver::build_combined_cuts() const
{
  std::vector<StoredCut> combined;
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    const auto& cuts = m_scene_cuts_[scene];
    combined.insert(combined.end(), cuts.begin(), cuts.end());
  }
  return combined;
}

// ── Clone pool (now in sddp_clone_pool.hpp/cpp) ─────────────────────────────

// ── Cut persistence (delegated to sddp_cut_io.hpp free functions) ───────────

auto SDDPSolver::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  if (m_options_.single_cut_storage) {
    const auto combined = build_combined_cuts();
    return save_cuts_csv(combined, planning_lp(), filepath);
  }
  return save_cuts_csv(m_stored_cuts_, planning_lp(), filepath);
}

auto SDDPSolver::save_scene_cuts(SceneIndex scene,
                                 const std::string& directory) const
    -> std::expected<void, Error>
{
  return save_scene_cuts_csv(
      m_scene_cuts_[scene], scene, scene_uid(scene), planning_lp(), directory);
}

auto SDDPSolver::save_all_scene_cuts(const std::string& directory) const
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

auto SDDPSolver::load_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_cuts_csv(planning_lp(), filepath, m_label_maker_);
}

auto SDDPSolver::load_scene_cuts_from_directory(const std::string& directory)
    -> std::expected<CutLoadResult, Error>
{
  return gtopt::load_scene_cuts_from_directory(
      planning_lp(), directory, m_label_maker_);
}

auto SDDPSolver::load_boundary_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_boundary_cuts_csv(planning_lp(),
                                filepath,
                                m_options_,
                                m_label_maker_,
                                m_scene_phase_states_);
}

auto SDDPSolver::load_named_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_named_cuts_csv(planning_lp(),
                             filepath,
                             m_options_,
                             m_label_maker_,
                             m_scene_phase_states_);
}

// ── Monitoring API ───────────────────────────────────────────────────────────
// Implementation moved to sddp_monitor.cpp (write_sddp_api_status free fn).
// maybe_write_api_status below builds the snapshot and delegates.

// ─── Private helper method implementations ───────────────────────────────────

auto SDDPSolver::validate_inputs() const -> std::optional<Error>
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

auto SDDPSolver::initialize_solver() -> std::expected<void, Error>
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
  m_scene_cuts_.resize(num_scenes);
  m_infeasibility_counter_.resize(num_scenes);
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    m_infeasibility_counter_[scene].resize(num_phases, 0);
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
  m_iteration_offset_ = IterationIndex {};

  if (!m_options_.cuts_input_file.empty()) {
    auto result = load_cuts(m_options_.cuts_input_file);
    if (result.has_value()) {
      m_iteration_offset_ =
          std::max(m_iteration_offset_, IterationIndex {result->max_iteration});
      SPDLOG_INFO("SDDP hot-start: loaded {} cuts (max_iter={})",
                  result->count,
                  result->max_iteration);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load cuts: {}",
                  result.error().message);
    }
  } else if (m_options_.hot_start_mode != HotStartMode::none
             && !m_options_.cuts_output_file.empty())
  {
    const auto cut_dir =
        std::filesystem::path(m_options_.cuts_output_file).parent_path();
    if (!cut_dir.empty() && std::filesystem::exists(cut_dir)) {
      auto result = load_scene_cuts_from_directory(cut_dir.string());
      if (result.has_value() && result->count > 0) {
        m_iteration_offset_ = std::max(m_iteration_offset_,
                                       IterationIndex {result->max_iteration});
        SPDLOG_INFO("SDDP hot-start: loaded {} cuts from {} (max_iter={})",
                    result->count,
                    cut_dir.string(),
                    result->max_iteration);
      }
    }
  }

  // ── Load boundary cuts (future-cost function for last phase) ──────────────
  if (!m_options_.boundary_cuts_file.empty()) {
    auto result = load_boundary_cuts(m_options_.boundary_cuts_file);
    if (result.has_value()) {
      m_iteration_offset_ =
          std::max(m_iteration_offset_, IterationIndex {result->max_iteration});
      SPDLOG_INFO("SDDP: loaded {} boundary cuts from {} (max_iter={})",
                  result->count,
                  m_options_.boundary_cuts_file,
                  result->max_iteration);
    } else {
      SPDLOG_WARN("SDDP: could not load boundary cuts: {}",
                  result.error().message);
    }
  }

  // ── Load named hot-start cuts (all phases, named state variables) ─────────
  if (!m_options_.named_cuts_file.empty()) {
    auto result = load_named_cuts(m_options_.named_cuts_file);
    if (result.has_value()) {
      m_iteration_offset_ =
          std::max(m_iteration_offset_, IterationIndex {result->max_iteration});
      SPDLOG_INFO("SDDP: loaded {} named hot-start cuts from {} (max_iter={})",
                  result->count,
                  m_options_.named_cuts_file,
                  result->max_iteration);
    } else {
      SPDLOG_WARN("SDDP: could not load named hot-start cuts: {}",
                  result.error().message);
    }
  }

  if (m_iteration_offset_ > 0) {
    SPDLOG_INFO("SDDP: iteration offset set to {} from hot-start cuts",
                m_iteration_offset_);
  }

  m_initialized_ = true;

  SPDLOG_INFO("SDDP: updating initial LP coefficients for all phases");
  {
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(num_scenes));
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      threads.emplace_back(
          [this, si, num_phases]()
          {
            for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
              update_coefficients_for_phase(si, pi, m_iteration_offset_);
            }
          });
    }
  }
  SPDLOG_INFO(
      "SDDP: initial coefficient update done ({} scene(s) × {} phase(s))",
      num_scenes,
      num_phases);

  const auto init_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - init_start)
                          .count();
  SPDLOG_INFO("SDDP: initialization complete ({:.2f}s)", init_s);
  return {};
}

void SDDPSolver::reset_live_state() noexcept
{
  m_current_iteration_.store(0);
  m_current_gap_.store(1.0);
  m_current_lb_.store(0.0);
  m_current_ub_.store(0.0);
  m_converged_.store(false);
  m_current_pass_.store(0);
  m_scenes_done_.store(0);
}

auto SDDPSolver::run_forward_pass_all_scenes(IterationIndex iter,
                                             SDDPWorkPool& pool,
                                             const SolverOptions& opts)
    -> std::expected<ForwardPassOutcome, Error>
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  m_current_pass_.store(1);
  m_scenes_done_.store(0);

  const auto fwd_start = std::chrono::steady_clock::now();
  std::vector<std::future<std::expected<double, Error>>> futures;
  futures.reserve(num_scenes);

  // Forward-pass scene tasks use High priority; lower iteration = higher key.
  const auto fwd_req = make_forward_lp_task_req(iter, PhaseIndex {0});

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    auto fut = pool.submit([this, scene, iter, &opts]
                           { return forward_pass(scene, iter, opts); },
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
      SPDLOG_WARN(
          "SDDP forward: scene {} failed: {}", scene, fwd.error().message);
      out.has_feasibility_issue = true;
      out.scene_feasible[si_sz] = 0;
      m_scenes_done_.fetch_add(1);
      continue;
    }
    out.scene_upper_bounds[si_sz] = *fwd;
    ++out.scenes_solved;
    m_scenes_done_.fetch_add(1);
    const auto si = static_cast<Index>(scene);
    if ((si + 1) % 4 == 0 || si + 1 == num_scenes) {
      SPDLOG_DEBUG("SDDP forward: {}/{} scenes completed", si + 1, num_scenes);
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

auto SDDPSolver::run_backward_pass_all_scenes(
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
    bwd_opts.warm_start = true;
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
      SPDLOG_WARN("SDDP backward: failed: {}", bwd.error().message);
      out.has_feasibility_issue = true;
      m_scenes_done_.fetch_add(1);
      continue;
    }
    out.total_cuts += *bwd;
    m_scenes_done_.fetch_add(1);
    if (bwd_done % 4 == 0 || bwd_done == bwd_total) {
      SPDLOG_DEBUG(
          "SDDP backward: {}/{} scenes completed", bwd_done, bwd_total);
    }
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - bwd_start)
                      .count();

  m_current_pass_.store(0);
  return out;
}

// ── Phase-synchronized backward pass (for cut sharing) ──────────────────────

auto SDDPSolver::run_backward_pass_synchronized(
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
    const auto cuts_before_step = m_stored_cuts_.size();

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
      const std::scoped_lock lock(m_cuts_mutex_);
      for (std::size_t ci = cuts_before_step; ci < m_stored_cuts_.size(); ++ci)
      {
        const auto& sc = m_stored_cuts_[ci];
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

void SDDPSolver::compute_iteration_bounds(
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

void SDDPSolver::apply_cut_sharing_for_iteration(std::size_t cuts_before,
                                                 IterationIndex iteration)
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());

  if (m_options_.cut_sharing == CutSharingMode::none || num_scenes <= 1) {
    return;
  }

  // Helper: reconstruct a SparseRow from a StoredCut
  auto to_sparse_row = [](const StoredCut& sc) -> SparseRow
  {
    auto row = SparseRow {
        .name = sc.name,
        .lowb = sc.rhs,
        .uppb = LinearProblem::DblMax,
    };
    for (const auto& [col, coeff] : sc.coefficients) {
      row[ColIndex {col}] = coeff;
    }
    return row;
  };

  // Build scene UID → SceneIndex lookup
  flat_map<SceneUid, SceneIndex> scene_uid_map;
  const auto& scenes = planning_lp().simulation().scenes();
  for (auto&& [si, sc_lp] : enumerate<SceneIndex>(scenes)) {
    scene_uid_map[sc_lp.uid()] = si;
  }

  for (const auto pi : iota_range<PhaseIndex>(0, num_phases - 1)) {
    StrongIndexVector<SceneIndex, std::vector<SparseRow>> per_scene_cuts;
    per_scene_cuts.resize(num_scenes);

    const auto pi_uid = phase_uid(pi);

    if (m_options_.single_cut_storage) {
      // Iterate per-scene vectors directly
      for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
        const auto& cuts = m_scene_cuts_[si];
        const auto offset = m_scene_cuts_before_[static_cast<std::size_t>(si)];
        for (std::size_t ci = offset; ci < cuts.size(); ++ci) {
          const auto& sc = cuts[ci];
          if (sc.type != CutType::Optimality || sc.phase != pi_uid) {
            continue;
          }
          per_scene_cuts[si].push_back(to_sparse_row(sc));
        }
      }
    } else {
      // Use shared m_stored_cuts_ with offset
      for (std::size_t ci = cuts_before; ci < m_stored_cuts_.size(); ++ci) {
        const auto& sc = m_stored_cuts_[ci];
        if (sc.type != CutType::Optimality || sc.phase != pi_uid) {
          continue;
        }
        auto sit = scene_uid_map.find(sc.scene);
        if (sit != scene_uid_map.end()) {
          per_scene_cuts[sit->second].push_back(to_sparse_row(sc));
        }
      }
    }

    share_cuts_for_phase(pi, per_scene_cuts, iteration);
  }
}

void SDDPSolver::finalize_iteration_result(SDDPIterationResult& ir,
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

void SDDPSolver::maybe_write_api_status(
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
  const SDDPStatusSnapshot snapshot {
      .iteration = m_current_iteration_.load(),
      .gap = m_current_gap_.load(),
      .lower_bound = m_current_lb_.load(),
      .upper_bound = m_current_ub_.load(),
      .converged = m_converged_.load(),
      .max_iterations = m_options_.max_iterations,
      .min_iterations = m_options_.min_iterations,
      .current_pass = m_current_pass_.load(),
      .scenes_done = m_scenes_done_.load(),
  };
  write_sddp_api_status(status_file, results, elapsed, snapshot, monitor);
}

void SDDPSolver::save_cuts_for_iteration(
    IterationIndex iter, std::span<const uint8_t> scene_feasible)
{
  if (m_options_.cuts_output_file.empty()) {
    return;
  }

  const auto cut_dir =
      std::filesystem::path(m_options_.cuts_output_file).parent_path();

  // Save to a versioned file: sddp_cuts_<iter>.csv
  if (!cut_dir.empty()) {
    const auto versioned_file =
        (cut_dir / std::format(sddp_file::versioned_cuts_fmt, iter)).string();
    auto result = save_cuts(versioned_file);
    if (!result.has_value()) {
      SPDLOG_WARN("SDDP: could not save versioned cuts at iter {}: {}",
                  iter,
                  result.error().message);
    }
  }

  // Save per-scene cuts
  if (!cut_dir.empty()) {
    auto scene_result = save_all_scene_cuts(cut_dir.string());
    if (!scene_result.has_value()) {
      SPDLOG_WARN("SDDP: could not save per-scene cuts at iter {}: {}",
                  iter,
                  scene_result.error().message);
    }
  }

  // Rename cut files for infeasible scenes
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    if (scene_feasible[static_cast<std::size_t>(scene)] != 0U) {
      continue;
    }
    if (cut_dir.empty()) {
      continue;
    }
    const auto suid = scene_uid(scene);
    const auto scene_file =
        cut_dir / std::format(sddp_file::scene_cuts_fmt, suid);
    const auto error_file =
        cut_dir / std::format(sddp_file::error_scene_cuts_fmt, suid);
    std::error_code ec;
    if (std::filesystem::exists(scene_file, ec)) {
      std::filesystem::rename(scene_file, error_file, ec);
      if (!ec) {
        SPDLOG_TRACE("SDDP: renamed cut file for infeasible scene {} to {}",
                     scene,
                     error_file.string());
      }
    }
  }
}

// ── solve() — now in sddp_iteration.cpp ─────────────────────────────────────

// ─── SDDPPlanningSolver ─────────────────────────────────────────────────────

SDDPPlanningSolver::SDDPPlanningSolver(SDDPOptions opts) noexcept
    : m_sddp_opts_(std::move(opts))
{
}

auto SDDPPlanningSolver::solve(PlanningLP& planning_lp,
                               const SolverOptions& opts)
    -> std::expected<int, Error>
{
  const auto num_scenes = static_cast<int>(planning_lp.systems().size());
  const auto num_phases = num_scenes > 0
      ? static_cast<int>(planning_lp.systems().front().size())
      : 0;
  SPDLOG_INFO(
      "SDDPSolver: starting {} scene(s) × {} phase(s)", num_scenes, num_phases);

  // build_lp: LP already built in PlanningLP constructor — skip all
  // solving.
  if (m_sddp_opts_.build_lp) {
    SPDLOG_INFO("SDDP: build_lp mode — LP built, skipping solve");
    return 0;
  }

  SDDPSolver sddp(planning_lp, m_sddp_opts_);
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
        .iterations = training_iters,
        .converged = last.converged,
        .stationary_converged = last.stationary_converged,
    });
  }

  // Return 1 if converged, 0 otherwise
  if (!m_last_results_.empty() && m_last_results_.back().converged) {
    return 1;
  }

  return std::unexpected(Error {
      .code = ErrorCode::SolverError,
      .message = std::format(
          "SDDP did not converge after {} iterations (gap={:.6f})",
          m_last_results_.empty() ? 0 : m_last_results_.back().iteration,
          m_last_results_.empty() ? 1.0 : m_last_results_.back().gap),
  });
}

// ── Helper: build the ApertureSubmitFunc callback ───────────────────────────

auto SDDPSolver::make_aperture_submit_fn(IterationIndex /*iteration*/,
                                         PhaseIndex /*phase*/)
    -> ApertureSubmitFunc
{
  // Run aperture tasks synchronously — the caller is already running
  // in a pool thread (one per scene).  Submitting to the same pool
  // would block the thread waiting for sub-tasks, causing starvation
  // when all scene threads are occupied.
  return [](const std::function<ApertureCutResult()>& task)
             -> std::future<ApertureCutResult>
  {
    std::promise<ApertureCutResult> p;
    p.set_value(task());
    return p.get_future();
  };
}

// ── Aperture per-phase implementation (shared by single-phase and full pass)

auto SDDPSolver::backward_pass_aperture_phase_impl(
    SceneIndex scene,
    PhaseIndex phase,
    int cut_offset,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    const SolverOptions& opts,
    IterationIndex iteration) -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene];
  int cuts_added = 0;

  const auto src_phase = phase - PhaseIndex {1};
  auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
  const auto& src_state = phase_states[src_phase];
  const auto& plp = planning_lp().simulation().phases()[phase];

  // Enable warm-start on aperture clone resolves when configured.
  auto aperture_solve_opts = opts;
  aperture_solve_opts.warm_start = m_options_.warm_start;

  // Forward-pass solution for the target phase — used as warm-start hint
  const auto& target_state = phase_states[phase];

  auto expected_cut =
      solve_apertures_for_phase(scene,
                                phase,
                                src_state,
                                base_scenario,
                                all_scenarios,
                                aperture_defs,
                                plp.apertures(),
                                cut_offset,
                                iteration,
                                planning_lp().system(scene, phase),
                                plp,
                                aperture_solve_opts,
                                m_label_maker_,
                                m_options_.log_directory,
                                scene_uid(scene),
                                phase_uid(phase),
                                make_aperture_submit_fn(iteration, phase),
                                m_options_.aperture_timeout,
                                m_options_.save_aperture_lp,
                                m_aperture_cache_,
                                target_state.forward_col_sol,
                                target_state.forward_row_dual,
                                get_pooled_clone_ptr(scene, phase));

  if (!expected_cut.has_value()) {
    // Fallback: build a regular Benders cut from the cached
    // forward-pass reduced costs and objective (same as backward_pass).
    const auto& target_state = phase_states[phase];
    auto fallback_cut = build_benders_cut(
        src_state.alpha_col,
        src_state.outgoing_links,
        target_state.forward_col_cost,
        target_state.forward_full_obj,
        sddp_label("sddp", "fcut", scene, phase, iteration, cut_offset));

    {
      const auto cut_row = src_li.add_row(fallback_cut);
      store_cut(scene, src_phase, fallback_cut, CutType::Optimality, cut_row);
    }
    ++cuts_added;

    SPDLOG_TRACE("SDDP aperture fallback: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 fallback_cut.lowb);

    if (src_phase > PhaseIndex {0}) {
      auto r = src_li.resolve(opts);
      if (!r.has_value() || !src_li.is_optimal()) {
        SPDLOG_WARN(
            "SDDP backward: iter {} scene {} phase {} non-optimal after "
            "fallback cut (status {}), skipping further backpropagation",
            iteration,
            scene_uid(scene),
            phase_uid(src_phase),
            src_li.get_status());
      }
    }

    return cuts_added;
  }

  {
    const auto cut_row = src_li.add_row(*expected_cut);
    store_cut(scene, src_phase, *expected_cut, CutType::Optimality, cut_row);
  }
  ++cuts_added;

  SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
               scene,
               src_phase,
               expected_cut->lowb);

  // Re-solve source phase after adding the cut to propagate feasibility.
  // Feasibility cuts are never shared between scenes.
  if (src_phase > PhaseIndex {0}) {
    auto r = src_li.resolve(opts);
    if (!r.has_value() || !src_li.is_optimal()) {
      SPDLOG_WARN(
          "SDDP backward: iter {} scene {} phase {} non-optimal after "
          "expected cut (status {}), skipping further backpropagation",
          iteration,
          scene_uid(scene),
          phase_uid(src_phase),
          src_li.get_status());
    }
  }

  return cuts_added;
}

// ── Per-phase aperture backward pass step ───────────────────────────────────

auto SDDPSolver::backward_pass_with_apertures_single_phase(
    SceneIndex scene,
    PhaseIndex phase,
    int cut_offset,
    const SolverOptions& opts,
    IterationIndex iteration) -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();
  const auto& scene_lp = simulation.scenes()[scene];
  const auto& scene_scenarios = scene_lp.scenarios();

  if (scene_scenarios.empty()) {
    return backward_pass_single_phase(
        scene, phase, cut_offset, opts, iteration);
  }

  // Determine effective apertures based on options
  Array<Aperture> filtered;
  std::span<const Aperture> effective_defs;

  if (!m_options_.apertures.has_value()) {
    // nullopt: use simulation aperture_array as-is (per-phase filtering
    // happens inside build_effective_apertures via Phase::apertures)
    if (aperture_defs.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }
    effective_defs = aperture_defs;
  } else {
    // Non-empty UID list: filter aperture_defs to only matching UIDs
    const auto& requested = *m_options_.apertures;
    if (requested.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }

    if (!aperture_defs.empty()) {
      // Filter existing apertures by requested UIDs
      for (const auto& ap : aperture_defs) {
        if (std::ranges::find(requested, ap.uid) != requested.end()) {
          filtered.push_back(ap);
        }
      }
      for (const auto uid : requested) {
        const bool found = std::ranges::any_of(
            filtered, [uid](const auto& a) { return a.uid == uid; });
        if (!found) {
          SPDLOG_WARN("SDDP apertures: requested UID {} not found, skipping",
                      uid);
        }
      }
    } else {
      // No aperture_array: build synthetic from scenarios matching UIDs
      filtered = build_synthetic_apertures(all_scenarios,
                                           static_cast<int>(requested.size()));
    }

    if (filtered.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }
    effective_defs = filtered;
  }

  return backward_pass_aperture_phase_impl(scene,
                                           phase,
                                           cut_offset,
                                           scene_scenarios.front(),
                                           all_scenarios,
                                           effective_defs,
                                           opts,
                                           iteration);
}

// ── Aperture backward pass ──────────────────────────────────────────────────

auto SDDPSolver::backward_pass_with_apertures(SceneIndex scene,
                                              const SolverOptions& opts,
                                              IterationIndex iteration)
    -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();

  // Determine the effective aperture definitions to use
  Array<Aperture> filtered;
  std::span<const Aperture> effective_defs;

  if (!m_options_.apertures.has_value()) {
    // nullopt: use simulation aperture_array (per-phase filtering via
    // Phase::apertures happens downstream in build_effective_apertures)
    if (aperture_defs.empty()) {
      return backward_pass(scene, opts, iteration);
    }
    effective_defs = aperture_defs;
  } else {
    const auto& requested = *m_options_.apertures;
    if (requested.empty()) {
      return backward_pass(scene, opts, iteration);
    }

    if (!aperture_defs.empty()) {
      // Filter existing apertures by requested UIDs
      for (const auto& ap : aperture_defs) {
        if (std::ranges::find(requested, ap.uid) != requested.end()) {
          filtered.push_back(ap);
        }
      }
      for (const auto uid : requested) {
        const bool found = std::ranges::any_of(
            filtered, [uid](const auto& a) { return a.uid == uid; });
        if (!found) {
          SPDLOG_WARN("SDDP apertures: requested UID {} not found, skipping",
                      uid);
        }
      }
    } else {
      // No aperture_array: build synthetic from scenarios
      const auto num_all = static_cast<int>(all_scenarios.size());
      filtered = build_synthetic_apertures(
          all_scenarios, std::min(static_cast<int>(requested.size()), num_all));
    }

    if (filtered.empty()) {
      return backward_pass(scene, opts, iteration);
    }
    effective_defs = filtered;
  }

  // ── Common aperture backward loop ─────────────────────────────────────
  const auto& phases = simulation.phases();
  const auto num_phases = static_cast<Index>(phases.size());
  auto& phase_states = m_scene_phase_states_[scene];
  int total_cuts = 0;

  const auto& scene_lp = simulation.scenes()[scene];
  const auto& scene_scenarios = scene_lp.scenarios();
  if (scene_scenarios.empty()) {
    return backward_pass(scene, opts, iteration);
  }
  const auto& base_scenario = scene_scenarios.front();
  // Collect phases where all apertures were infeasible for a summary
  std::vector<PhaseUid> infeasible_phases;

  for (const auto phase :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "SDDP backward (apertures): cancelled at scene {} phase {}",
              scene_uid(scene),
              phase_uid(phase)),
      });
    }

    const auto src_phase = phase - PhaseIndex {1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = phase_states[src_phase];
    const auto& plp = phases[phase];

    // Enable warm-start on aperture clone resolves when configured.
    auto ws_opts = opts;
    ws_opts.warm_start = m_options_.warm_start;

    // Forward-pass solution for the target phase — warm-start hint
    const auto& target_state = phase_states[phase];

    auto expected_cut =
        solve_apertures_for_phase(scene,
                                  phase,
                                  src_state,
                                  base_scenario,
                                  all_scenarios,
                                  effective_defs,
                                  plp.apertures(),
                                  total_cuts,
                                  iteration,
                                  planning_lp().system(scene, phase),
                                  plp,
                                  ws_opts,
                                  m_label_maker_,
                                  m_options_.log_directory,
                                  scene_uid(scene),
                                  phase_uid(phase),
                                  make_aperture_submit_fn(iteration, phase),
                                  0.0,
                                  m_options_.save_aperture_lp,
                                  m_aperture_cache_,
                                  target_state.forward_col_sol,
                                  target_state.forward_row_dual,
                                  get_pooled_clone_ptr(scene, phase));

    if (!expected_cut.has_value()) {
      infeasible_phases.push_back(phase_uid(phase));
      auto fallback_cut = build_benders_cut(
          src_state.alpha_col,
          src_state.outgoing_links,
          target_state.forward_col_cost,
          target_state.forward_full_obj,
          sddp_label("sddp", "fcut", scene, phase, iteration, total_cuts));

      {
        const auto cut_row = src_li.add_row(fallback_cut);
        store_cut(scene, src_phase, fallback_cut, CutType::Optimality, cut_row);
      }
      ++total_cuts;

      SPDLOG_TRACE(
          "SDDP aperture fallback: scene {} cut for phase {} "
          "rhs={:.4f}",
          scene,
          src_phase,
          fallback_cut.lowb);

      if (src_phase > PhaseIndex {0}) {
        auto r = src_li.resolve(opts);
        if (!r.has_value() || !src_li.is_optimal()) {
          SPDLOG_WARN(
              "SDDP backward: iter {} scene {} phase {} non-optimal "
              "after fallback cut (status {}), skipping further "
              "backpropagation",
              iteration,
              scene_uid(scene),
              phase_uid(src_phase),
              src_li.get_status());
        }
      }

      continue;
    }

    {
      const auto cut_row = src_li.add_row(*expected_cut);
      store_cut(scene, src_phase, *expected_cut, CutType::Optimality, cut_row);
    }
    ++total_cuts;

    SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 expected_cut->lowb);

    // Re-solve source phase after adding the cut to propagate
    // feasibility.
    if (src_phase > PhaseIndex {0}) {
      auto r = src_li.resolve(opts);
      if (!r.has_value() || !src_li.is_optimal()) {
        SPDLOG_WARN(
            "SDDP backward: iter {} scene {} phase {} non-optimal "
            "after expected cut (status {}), skipping further "
            "backpropagation",
            iteration,
            scene_uid(scene),
            phase_uid(src_phase),
            src_li.get_status());
      }
    }
  }

  // Log a single summary for all phases with infeasible apertures
  if (!infeasible_phases.empty()) {
    SPDLOG_WARN(
        "SDDP aperture: scene {} — all apertures infeasible at {} "
        "phase(s) [{}], used Benders fallback cuts",
        scene_uid(scene),
        infeasible_phases.size(),
        join_values(infeasible_phases));
  }

  return total_cuts;
}

}  // namespace gtopt
