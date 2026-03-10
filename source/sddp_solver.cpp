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
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>

#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/work_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Utilities ──────────────────────────────────────────────────────────────

CutSharingMode parse_cut_sharing_mode(std::string_view name)
{
  if (name == "expected") {
    return CutSharingMode::Expected;
  }
  if (name == "max") {
    return CutSharingMode::Max;
  }
  return CutSharingMode::None;
}

// ─── Free-function building blocks ──────────────────────────────────────────

void propagate_trial_values(std::span<StateVarLink> links,
                            std::span<const double> source_solution,
                            LinearInterface& target_li) noexcept
{
  for (auto& link : links) {
    link.trial_value = source_solution[link.source_col];
    target_li.set_col_low(link.dependent_col, link.trial_value);
    target_li.set_col_upp(link.dependent_col, link.trial_value);
  }
}

auto build_benders_cut(ColIndex alpha_col,
                       std::span<const StateVarLink> links,
                       std::span<const double> reduced_costs,
                       double objective_value,
                       std::string_view name) -> SparseRow
{
  auto row = SparseRow {
      .name = std::string(name),
      .lowb = objective_value,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = 1.0;

  for (const auto& link : links) {
    const auto rc = reduced_costs[link.dependent_col];
    row[link.source_col] = -rc;
    row.lowb -= rc * link.trial_value;
  }

  return row;
}

bool relax_fixed_state_variable(LinearInterface& li,
                                const StateVarLink& link,
                                [[maybe_unused]] PhaseIndex phase,
                                double penalty)
{
  const auto dep = link.dependent_col;
  const auto lo = li.get_col_low()[dep];
  const auto hi = li.get_col_upp()[dep];

  if (std::abs(lo - hi) >= 1e-10) {
    return false;
  }

  // Relax to the physical bounds captured from the source column
  li.set_col_low(dep, link.source_low);
  li.set_col_upp(dep, link.source_upp);

  // Penalised slack variables: up (overshoot) and dn (undershoot)
  const auto sup = li.add_col({}, 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sup, penalty);

  const auto sdn = li.add_col({}, 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sdn, penalty);

  // dep + sup − sdn = trial_value
  auto elastic = SparseRow {
      .name = {},
      .lowb = link.trial_value,
      .uppb = link.trial_value,
  };
  elastic[dep] = 1.0;
  elastic[sup] = 1.0;
  elastic[sdn] = -1.0;

  li.add_row(elastic);

  SPDLOG_TRACE(
      "SDDP elastic: phase {} col {} relaxed to [{:.2f}, {:.2f}] "
      "(source bounds from phase {})",
      static_cast<Index>(phase),
      static_cast<Index>(dep),
      link.source_low,
      link.source_upp,
      static_cast<Index>(link.source_phase));
  return true;
}

auto average_benders_cut(const std::vector<SparseRow>& cuts,
                         std::string_view name) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() == 1) {
    auto result = cuts.front();
    result.name = std::string(name);
    return result;
  }

  const auto n = static_cast<double>(cuts.size());

  // Collect all column indices that appear in any cut
  flat_map<ColIndex, double> avg_coeffs;
  double avg_rhs = 0.0;

  for (const auto& cut : cuts) {
    avg_rhs += cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      avg_coeffs[col] += coeff;
    }
  }

  auto result = SparseRow {
      .name = std::string(name),
      .lowb = avg_rhs / n,
      .uppb = LinearProblem::DblMax,
  };

  for (const auto& [col, total_coeff] : avg_coeffs) {
    result[col] = total_coeff / n;
  }

  return result;
}

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

SDDPSolver::SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts) noexcept
    : m_planning_lp_(planning_lp)
    , m_options_(std::move(opts))
    , m_label_maker_(planning_lp.options())
{
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPSolver::initialize_alpha_variables(SceneIndex scene)
{
  const auto& phases = planning_lp().simulation().phases();

  auto& phase_states = m_scene_phase_states_[scene];
  phase_states.resize(phases.size());

  // Add α (future-cost) variable to every phase except the last
  for (auto&& [pi, _phase] : enumerate<PhaseIndex>(phases)) {
    if (pi == PhaseIndex {static_cast<Index>(phases.size()) - 1}) {
      break;
    }
    auto& state = phase_states[pi];
    auto& li = planning_lp().system(scene, pi).linear_interface();

    state.alpha_col =
        li.add_col(sddp_label("sddp", "alpha", "sc", scene, "ph", pi),
                   m_options_.alpha_min,
                   m_options_.alpha_max);
    li.set_obj_coeff(state.alpha_col, 1.0);
  }

  // Last phase: no future cost
  phase_states[PhaseIndex {static_cast<Index>(phases.size()) - 1}].alpha_col =
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

    const auto next_phase = PhaseIndex {static_cast<Index>(phase) + 1};

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

std::optional<LinearInterface> SDDPSolver::elastic_solve(
    SceneIndex scene, PhaseIndex phase, const SolverOptions& opts)
{
  if (phase == PhaseIndex {0}) {
    return std::nullopt;
  }

  auto& li = planning_lp().system(scene, phase).linear_interface();
  const auto prev = PhaseIndex {static_cast<Index>(phase) - 1};
  const auto& prev_state = m_scene_phase_states_[scene][prev];

  // Clone the LP – modifications to the clone don't touch the original
  auto cloned = li.clone();

  bool modified = false;
  for (const auto& link : prev_state.outgoing_links) {
    modified |= relax_fixed_state_variable(
        cloned, link, phase, m_options_.elastic_penalty);
  }

  if (!modified) {
    return std::nullopt;
  }

  // Solve the clone with elastic slack variables
  auto result = cloned.resolve(opts);
  if (result.has_value() && cloned.is_optimal()) {
    SPDLOG_TRACE(
        "SDDP elastic: scene {} phase {} solved via clone "
        "(obj={:.4f})",
        scene,
        phase,
        cloned.get_obj_value());
    return std::move(cloned);
  }
  return std::nullopt;
}

bool SDDPSolver::check_sentinel_stop() const
{
  if (m_options_.sentinel_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.sentinel_file);
}

// ── Forward pass ────────────────────────────────────────────────────────────

auto SDDPSolver::forward_pass(SceneIndex scene, const SolverOptions& opts)
    -> std::expected<double, Error>
{
  const auto& phases = planning_lp().simulation().phases();
  auto& phase_states = m_scene_phase_states_[scene];
  double total_opex = 0.0;

  for (auto&& [phase, _ph] : enumerate<PhaseIndex>(phases)) {
    auto& li = planning_lp().system(scene, phase).linear_interface();
    auto& state = phase_states[phase];

    // Propagate state variables from previous phase
    if (phase != PhaseIndex {0}) {
      const auto prev = PhaseIndex {static_cast<Index>(phase) - 1};
      auto& prev_st = phase_states[prev];
      const auto& prev_sol =
          planning_lp().system(scene, prev).linear_interface().get_col_sol();
      propagate_trial_values(prev_st.outgoing_links, prev_sol, li);
    }

    // Solve this phase
    auto result = li.resolve(opts);

    // Pointer to the LP whose solution we use for cost/cut data.
    // Defaults to the original LP; switches to elastic clone if needed.
    LinearInterface* solved_li = &li;
    std::optional<LinearInterface> elastic_clone;

    if (!result.has_value() || !li.is_optimal()) {
      // Clone the LP, apply elastic filter, and solve the clone.
      // The original LP remains unmodified (PLP clone pattern).
      elastic_clone = elastic_solve(scene, phase, opts);
      if (elastic_clone.has_value()) {
        solved_li = &(*elastic_clone);
      } else {
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format(
                "SDDP forward pass failed at scene {} phase {} (status {})",
                scene,
                phase,
                li.get_status()),
        });
      }
    }

    // Cache solution data for the backward pass
    const auto obj = solved_li->get_obj_value();
    state.forward_full_obj = obj;

    const auto rc = solved_li->get_col_cost();
    state.forward_col_cost.assign(rc.begin(), rc.end());

    const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
        ? solved_li->get_col_sol()[state.alpha_col]
        : 0.0;
    state.forward_objective = obj - alpha_val;
    total_opex += state.forward_objective;

    SPDLOG_TRACE(
        "SDDP forward: scene {} phase {} obj={:.4f} alpha={:.4f} opex={:.4f}{}",
        scene,
        phase,
        obj,
        alpha_val,
        state.forward_objective,
        elastic_clone.has_value() ? " [elastic]" : "");
  }

  return total_opex;
}

// ── Backward pass with iterative feasibility backpropagation ────────────────

auto SDDPSolver::backward_pass(SceneIndex scene, const SolverOptions& opts)
    -> std::expected<int, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  auto& phase_states = m_scene_phase_states_[scene];
  int total_cuts = 0;

  for (Index pi = num_phases - 1; pi >= 1; --pi) {
    const auto phase = PhaseIndex {pi};
    const auto src_phase = PhaseIndex {pi - 1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = phase_states[src_phase];

    // Use cached forward-pass solution for cut generation.
    // This avoids dependence on the original LP's solve state and works
    // correctly regardless of whether the elastic filter was used.
    const auto& target_state = phase_states[phase];

    auto cut = build_benders_cut(
        src_state.alpha_col,
        src_state.outgoing_links,
        target_state.forward_col_cost,
        target_state.forward_full_obj,
        sddp_label("sddp", "cut", "sc", scene, "ph", pi, "n", total_cuts));

    // Store the cut for sharing and persistence (thread-safe)
    {
      StoredCut stored {
          .phase = static_cast<int>(src_phase),
          .scene = static_cast<int>(scene),
          .name = cut.name,
          .rhs = cut.lowb,
      };
      for (const auto& [col, coeff] : cut.cmap) {
        stored.coefficients.emplace_back(static_cast<int>(col), coeff);
      }
      const std::lock_guard lock(m_cuts_mutex_);
      m_stored_cuts_.push_back(std::move(stored));
    }

    src_li.add_row(cut);
    ++total_cuts;

    SPDLOG_TRACE("SDDP backward: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 cut.lowb);

    // Re-solve source and handle iterative feasibility backpropagation.
    // If adding the cut makes phase k infeasible, build a feasibility
    // cut for phase k-1, continuing all the way to phase 0 if necessary.
    if (pi > 1) {
      auto r = src_li.resolve(opts);
      if (!r.has_value() || !src_li.is_optimal()) {
        // Iterative feasibility backpropagation
        for (Index back_pi = pi - 1; back_pi >= 0; --back_pi) {
          const auto back_phase = PhaseIndex {back_pi};

          if (back_pi > 0) {
            SPDLOG_WARN(
                "SDDP backward: scene {} phase {} infeasible after "
                "cut, backpropagating to phase {}",
                scene,
                back_phase,
                back_pi - 1);
          }

          // Clone the LP, apply elastic filter, solve the clone.
          // The original LP is never modified by the elastic filter.
          auto elastic_clone = elastic_solve(scene, back_phase, opts);
          if (elastic_clone.has_value()) {
            if (back_pi > 0) {
              // Build a feasibility-like cut for the previous phase
              const auto prev_bp = PhaseIndex {back_pi - 1};
              auto& prev_li =
                  planning_lp().system(scene, prev_bp).linear_interface();
              const auto& prev_state = phase_states[prev_bp];

              auto feas_cut = build_benders_cut(prev_state.alpha_col,
                                                prev_state.outgoing_links,
                                                elastic_clone->get_col_cost(),
                                                elastic_clone->get_obj_value(),
                                                sddp_label("sddp",
                                                           "feas",
                                                           "sc",
                                                           scene,
                                                           "ph",
                                                           back_pi,
                                                           "n",
                                                           total_cuts));

              prev_li.add_row(feas_cut);
              ++total_cuts;

              // Re-solve the previous phase
              auto r3 = prev_li.resolve(opts);
              if (r3.has_value() && prev_li.is_optimal()) {
                break;  // Feasibility restored
              }
              // Continue backpropagating to back_pi - 1
            } else {
              break;  // Restored at phase 0
            }
          } else if (back_pi == 0) {
            // Phase 0 with no elastic filter available = scene infeasible
            return std::unexpected(Error {
                .code = ErrorCode::SolverError,
                .message = std::format(
                    "SDDP: scene {} is infeasible (backpropagated to "
                    "phase 0)",
                    scene),
            });
          }
        }
      }
    }
  }

  return total_cuts;
}

// ── Cut sharing ─────────────────────────────────────────────────────────────

void SDDPSolver::share_cuts_for_phase(
    PhaseIndex phase,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts)
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  if (num_scenes <= 1 || m_options_.cut_sharing == CutSharingMode::None) {
    return;
  }

  if (m_options_.cut_sharing == CutSharingMode::Expected) {
    // Collect all cuts from all scenes for this phase
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    // Compute average cut
    auto avg = average_benders_cut(
        all_cuts, sddp_label("sddp", "avg", "cut", "ph", phase));

    // Add the average cut to all scenes
    for (Index si = 0; si < num_scenes; ++si) {
      auto& li =
          planning_lp().system(SceneIndex {si}, phase).linear_interface();
      li.add_row(avg);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added average cut to phase {} "
        "({} source cuts from {} scenes)",
        phase,
        all_cuts.size(),
        num_scenes);

  } else if (m_options_.cut_sharing == CutSharingMode::Max) {
    // Add ALL cuts from ALL scenes to ALL scenes for this phase
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    for (Index si = 0; si < num_scenes; ++si) {
      auto& li =
          planning_lp().system(SceneIndex {si}, phase).linear_interface();
      for (const auto& cut : all_cuts) {
        li.add_row(cut);
      }
    }

    SPDLOG_TRACE("SDDP sharing: added {} cuts to phase {} for all {} scenes",
                 all_cuts.size(),
                 phase,
                 num_scenes);
  }
}

// ── Cut persistence ─────────────────────────────────────────────────────────

auto SDDPSolver::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  try {
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for writing: {}", filepath),
      });
    }

    // CSV format: phase,scene,name,rhs[,col_idx:coeff ...]
    ofs << "phase,scene,name,rhs,coefficients\n";
    for (const auto& cut : m_stored_cuts_) {
      ofs << cut.phase << "," << cut.scene << "," << cut.name << "," << cut.rhs;
      for (const auto& [col, coeff] : cut.coefficients) {
        ofs << "," << col << ":" << coeff;
      }
      ofs << "\n";
    }

    SPDLOG_TRACE("SDDP: saved {} cuts to {}", m_stored_cuts_.size(), filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error saving cuts to {}: {}", filepath, e.what()),
    });
  }
}

auto SDDPSolver::load_cuts(const std::string& filepath)
    -> std::expected<int, Error>
{
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for reading: {}", filepath),
      });
    }

    std::string line;
    std::getline(ifs, line);  // Skip header

    int cuts_loaded = 0;
    const auto num_scenes =
        static_cast<Index>(planning_lp().simulation().scenes().size());

    while (std::getline(ifs, line)) {
      if (line.empty()) {
        continue;
      }

      // Parse CSV: phase,scene,name,rhs,col1:coeff1,...
      std::istringstream iss(line);
      std::string token;

      std::getline(iss, token, ',');
      const auto phase_idx = std::stoi(token);

      std::getline(iss, token, ',');
      // scene_idx is parsed but intentionally ignored: loaded cuts are
      // broadcast to all scenes as warm-start approximations.
      [[maybe_unused]] const auto scene_idx = std::stoi(token);

      std::getline(iss, token, ',');
      const auto cut_name = token;

      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      auto row = SparseRow {
          .name = std::format("loaded_{}", cut_name),
          .lowb = rhs,
          .uppb = LinearProblem::DblMax,
      };

      while (std::getline(iss, token, ',')) {
        const auto colon = token.find(':');
        if (colon != std::string::npos) {
          const auto col = std::stoi(token.substr(0, colon));
          const auto coeff = std::stod(token.substr(colon + 1));
          row[ColIndex {col}] = coeff;
        }
      }

      // Add the loaded cut to all scenes for this phase
      const auto phase = PhaseIndex {phase_idx};
      for (Index si = 0; si < num_scenes; ++si) {
        auto& li =
            planning_lp().system(SceneIndex {si}, phase).linear_interface();
        li.add_row(row);
      }
      ++cuts_loaded;
    }

    SPDLOG_TRACE("SDDP: loaded {} cuts from {}", cuts_loaded, filepath);
    return cuts_loaded;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error loading cuts from {}: {}", filepath, e.what()),
    });
  }
}

// ── Main solve loop ─────────────────────────────────────────────────────────

auto SDDPSolver::solve(const SolverOptions& lp_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  const auto& sim = planning_lp().simulation();

  if (sim.scenes().empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "No scenes in simulation",
    });
  }
  if (sim.phases().size() < 2) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "SDDP requires at least 2 phases",
    });
  }

  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());

  // Bootstrap: solve all phases to establish baseline and state links
  if (auto r = planning_lp().resolve(); !r.has_value()) {
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::format("Initial PlanningLP solve failed: {}",
                               r.error().message),
    });
  }

  if (!m_initialized_) {
    m_scene_phase_states_.resize(num_scenes);

    for (Index si = 0; si < num_scenes; ++si) {
      const auto scene = SceneIndex {si};
      initialize_alpha_variables(scene);
      collect_state_variable_links(scene);
    }

    // Load saved cuts for hot-start if a file is provided
    if (!m_options_.cuts_input_file.empty()) {
      auto load_result = load_cuts(m_options_.cuts_input_file);
      if (load_result.has_value()) {
        SPDLOG_INFO("SDDP hot-start: loaded {} cuts", *load_result);
      } else {
        SPDLOG_WARN("SDDP hot-start: could not load cuts: {}",
                    load_result.error().message);
      }
    }

    m_initialized_ = true;
  }

  // Set up work pool for parallel scene processing
  WorkPoolConfig pool_config {};
  const double cpu_factor = 1.25;
  pool_config.max_threads = static_cast<int>(
      std::lround(cpu_factor * std::thread::hardware_concurrency()));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));

  AdaptiveWorkPool pool(pool_config);
  pool.start();

  std::vector<SDDPIterationResult> results;
  results.reserve(m_options_.max_iterations);

  for (int iter = 1; iter <= m_options_.max_iterations; ++iter) {
    // ── Check sentinel file for user-requested stop ──
    if (check_sentinel_stop()) {
      SPDLOG_INFO(
          "SDDP: sentinel file '{}' detected, stopping after {} "
          "iterations",
          m_options_.sentinel_file,
          iter - 1);
      break;
    }

    SDDPIterationResult ir {
        .iteration = iter,
    };

    // ── Forward pass for all scenes (parallel) ──
    std::vector<std::future<std::expected<double, Error>>> fwd_futures;
    fwd_futures.reserve(num_scenes);

    for (Index si = 0; si < num_scenes; ++si) {
      const auto scene = SceneIndex {si};
      auto fut = pool.submit([this, scene, &lp_opts]
                             { return forward_pass(scene, lp_opts); });
      fwd_futures.push_back(std::move(fut.value()));
    }

    double total_upper = 0.0;
    int scenes_solved = 0;
    for (Index si = 0; si < num_scenes; ++si) {
      auto fwd = fwd_futures[si].get();
      if (!fwd.has_value()) {
        // If a scene is infeasible, log warning and continue with others
        SPDLOG_WARN(
            "SDDP forward: scene {} failed: {}", si, fwd.error().message);
        ir.feasibility_issue = true;
        continue;
      }
      total_upper += *fwd;
      ++scenes_solved;
    }

    if (scenes_solved == 0) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = "SDDP: all scenes infeasible in forward pass",
      });
    }
    ir.upper_bound = total_upper / static_cast<double>(scenes_solved);

    // ── Lower bound = average of phase 0 objectives across scenes ──
    double total_lower = 0.0;
    for (Index si = 0; si < num_scenes; ++si) {
      total_lower += planning_lp()
                         .system(SceneIndex {si}, PhaseIndex {0})
                         .linear_interface()
                         .get_obj_value();
    }
    ir.lower_bound = total_lower / static_cast<double>(num_scenes);

    // ── Backward pass for all scenes (parallel) ──
    // Collect cuts per scene per phase for sharing
    using phase_cuts_t = StrongIndexVector<SceneIndex, std::vector<SparseRow>>;
    std::vector<phase_cuts_t> per_phase_scene_cuts(num_phases);
    for (auto& pc : per_phase_scene_cuts) {
      pc.resize(num_scenes);
    }

    std::vector<std::future<std::expected<int, Error>>> bwd_futures;
    bwd_futures.reserve(num_scenes);

    for (Index si = 0; si < num_scenes; ++si) {
      const auto scene = SceneIndex {si};
      auto fut = pool.submit([this, scene, &lp_opts]
                             { return backward_pass(scene, lp_opts); });
      bwd_futures.push_back(std::move(fut.value()));
    }

    int total_cuts = 0;
    for (Index si = 0; si < num_scenes; ++si) {
      auto bwd = bwd_futures[si].get();
      if (!bwd.has_value()) {
        // If a scene is infeasible in backward pass, keep solving others
        SPDLOG_WARN(
            "SDDP backward: scene {} failed: {}", si, bwd.error().message);
        ir.feasibility_issue = true;
        continue;
      }
      total_cuts += *bwd;
    }
    ir.cuts_added = total_cuts;

    // ── Cut sharing between scenes ──
    if (m_options_.cut_sharing != CutSharingMode::None && num_scenes > 1) {
      const auto cuts_before = m_stored_cuts_.size() - total_cuts;
      for (Index pi = 0; pi < num_phases - 1; ++pi) {
        StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
        scene_cuts.resize(num_scenes);

        for (size_t ci = cuts_before; ci < m_stored_cuts_.size(); ++ci) {
          const auto& sc = m_stored_cuts_[ci];
          if (sc.phase == pi) {
            // Reconstruct the SparseRow
            auto row = SparseRow {
                .name = sc.name,
                .lowb = sc.rhs,
                .uppb = LinearProblem::DblMax,
            };
            for (const auto& [col, coeff] : sc.coefficients) {
              row[ColIndex {col}] = coeff;
            }
            if (sc.scene >= 0 && sc.scene < num_scenes) {
              scene_cuts[SceneIndex {sc.scene}].push_back(std::move(row));
            }
          }
        }

        share_cuts_for_phase(PhaseIndex {pi}, scene_cuts);
      }
    }

    // Convergence check
    const auto denom = std::max(1.0, std::abs(ir.upper_bound));
    ir.gap = (ir.upper_bound - ir.lower_bound) / denom;
    ir.converged = (ir.gap < m_options_.convergence_tol);

    SPDLOG_TRACE(
        "SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} cuts={} scenes={}{}",
        iter,
        ir.lower_bound,
        ir.upper_bound,
        ir.gap,
        ir.cuts_added,
        num_scenes,
        ir.converged ? " [CONVERGED]" : "");

    // Log a brief INFO summary every iteration (non-trace)
    SPDLOG_INFO("SDDP iter {}: gap={:.6f}{}",
                iter,
                ir.gap,
                ir.converged ? " [CONVERGED]" : "");

    results.push_back(ir);

    // ── Save cuts incrementally after each iteration ──
    if (!m_options_.cuts_output_file.empty()) {
      auto save_result = save_cuts(m_options_.cuts_output_file);
      if (!save_result.has_value()) {
        SPDLOG_WARN("SDDP: could not save cuts at iter {}: {}",
                    iter,
                    save_result.error().message);
      }
    }

    if (ir.converged) {
      break;
    }
  }

  return results;
}

// ─── SDDPPlanningSolver ─────────────────────────────────────────────────────

SDDPPlanningSolver::SDDPPlanningSolver(SDDPOptions opts) noexcept
    : m_sddp_opts_(std::move(opts))
{
}

auto SDDPPlanningSolver::solve(PlanningLP& planning_lp,
                               const SolverOptions& opts)
    -> std::expected<int, Error>
{
  SDDPSolver sddp(planning_lp, m_sddp_opts_);
  auto results = sddp.solve(opts);

  if (!results.has_value()) {
    return std::unexpected(std::move(results.error()));
  }

  m_last_results_ = std::move(*results);

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

}  // namespace gtopt
