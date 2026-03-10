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
#include <format>
#include <fstream>
#include <mutex>
#include <span>

#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
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
                       const std::string& name) -> SparseRow
{
  auto row = SparseRow {.name = name};
  row[alpha_col] = 1.0;

  auto rhs = objective_value;
  for (const auto& link : links) {
    const auto rc = reduced_costs[link.dependent_col];
    row[link.source_col] = -rc;
    rhs -= rc * link.trial_value;
  }

  row.lowb = rhs;
  row.uppb = LinearProblem::DblMax;
  return row;
}

bool relax_fixed_state_variable(LinearInterface& li,
                                const StateVarLink& link,
                                PhaseIndex phase,
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

  const auto pi = static_cast<Index>(phase);
  const auto ci = static_cast<Index>(dep);

  // Penalised slack variables: up (overshoot) and dn (undershoot)
  const auto sup = li.add_col(
      std::format("elastic_up_ph{}_c{}", pi, ci), 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sup, penalty);

  const auto sdn = li.add_col(
      std::format("elastic_dn_ph{}_c{}", pi, ci), 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(sdn, penalty);

  // dep + sup − sdn = trial_value
  SparseRow elastic;
  elastic.name = std::format("elastic_ph{}_c{}", pi, ci);
  elastic[dep] = 1.0;
  elastic[sup] = 1.0;
  elastic[sdn] = -1.0;
  elastic.lowb = link.trial_value;
  elastic.uppb = link.trial_value;

  li.add_row(elastic);

  SPDLOG_DEBUG(
      "SDDP elastic: phase {} col {} relaxed to [{:.2f}, {:.2f}] "
      "(source bounds from phase {})",
      pi,
      ci,
      link.source_low,
      link.source_upp,
      static_cast<Index>(link.source_phase));
  return true;
}

auto average_benders_cut(const std::vector<SparseRow>& cuts,
                         const std::string& name) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() == 1) {
    auto result = cuts.front();
    result.name = name;
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

  SparseRow result;
  result.name = name;
  result.lowb = avg_rhs / n;
  result.uppb = LinearProblem::DblMax;

  for (const auto& [col, total_coeff] : avg_coeffs) {
    result[col] = total_coeff / n;
  }

  return result;
}

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

SDDPSolver::SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts) noexcept
    : m_planning_lp_(planning_lp)
    , m_options_(std::move(opts))
{
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPSolver::initialize_alpha_variables(SceneIndex scene)
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());

  auto& phase_states = m_scene_phase_states_[scene];
  phase_states.resize(num_phases);

  // Add α (future-cost) variable to every phase except the last
  for (Index pi = 0; pi < num_phases - 1; ++pi) {
    auto& state = phase_states[PhaseIndex {pi}];
    auto& li = planning_lp().system(scene, PhaseIndex {pi}).linear_interface();

    state.alpha_col = li.add_col(std::format("sddp_alpha_sc{}_ph{}", scene, pi),
                                 m_options_.alpha_min,
                                 m_options_.alpha_max);
    li.set_obj_coeff(state.alpha_col, 1.0);
  }

  // Last phase: no future cost
  phase_states[PhaseIndex {num_phases - 1}].alpha_col =
      ColIndex {unknown_index};
}

void SDDPSolver::collect_state_variable_links(SceneIndex scene)
{
  const auto& sim = planning_lp().simulation();
  const auto num_phases = static_cast<Index>(sim.phases().size());

  auto& phase_states = m_scene_phase_states_[scene];

  for (Index pi = 0; pi < num_phases; ++pi) {
    const auto phase = PhaseIndex {pi};
    auto& state = phase_states[phase];

    // Read column bounds from the source phase LP
    const auto& src_li = planning_lp().system(scene, phase).linear_interface();
    const auto col_lo = src_li.get_col_low();
    const auto col_hi = src_li.get_col_upp();

    for (const auto& [key, svar] : sim.state_variables(scene, phase)) {
      for (const auto& dep : svar.dependent_variables()) {
        if (dep.phase_index() != PhaseIndex {pi + 1}
            || dep.scene_index() != scene)
        {
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

    SPDLOG_DEBUG("SDDP: scene {} phase {} has {} outgoing state-variable links",
                 scene,
                 pi,
                 state.outgoing_links.size());
  }
}

// ── Forward pass ────────────────────────────────────────────────────────────

auto SDDPSolver::forward_pass(SceneIndex scene, const SolverOptions& opts)
    -> std::expected<double, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  auto& phase_states = m_scene_phase_states_[scene];
  double total_opex = 0.0;

  for (Index pi = 0; pi < num_phases; ++pi) {
    const auto phase = PhaseIndex {pi};
    auto& li = planning_lp().system(scene, phase).linear_interface();
    auto& state = phase_states[phase];

    // Propagate state variables from previous phase
    if (pi > 0) {
      auto& prev = phase_states[PhaseIndex {pi - 1}];
      const auto& prev_sol = planning_lp()
                                 .system(scene, PhaseIndex {pi - 1})
                                 .linear_interface()
                                 .get_col_sol();
      propagate_trial_values(prev.outgoing_links, prev_sol, li);
    }

    // Solve this phase
    auto result = li.resolve(opts);
    if (!result.has_value() || !li.is_optimal()) {
      if (apply_elastic_filter(scene, phase, opts)) {
        result = li.resolve(opts);
      }
      if (!result.has_value() || !li.is_optimal()) {
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format(
                "SDDP forward pass failed at scene {} phase {} (status {})",
                scene,
                pi,
                li.get_status()),
        });
      }
    }

    // Operating cost = objective minus α contribution
    const auto obj = li.get_obj_value();
    const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
        ? li.get_col_sol()[state.alpha_col]
        : 0.0;
    state.forward_objective = obj - alpha_val;
    total_opex += state.forward_objective;

    SPDLOG_DEBUG(
        "SDDP forward: scene {} phase {} obj={:.4f} alpha={:.4f} opex={:.4f}",
        scene,
        pi,
        obj,
        alpha_val,
        state.forward_objective);
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
    auto& target_li =
        planning_lp().system(scene, PhaseIndex {pi}).linear_interface();

    const auto src_phase = PhaseIndex {pi - 1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = phase_states[src_phase];

    // Build and add Benders optimality cut
    auto cut = build_benders_cut(
        src_state.alpha_col,
        src_state.outgoing_links,
        target_li.get_col_cost(),
        target_li.get_obj_value(),
        std::format("sddp_cut_sc{}_ph{}_{}", scene, pi, total_cuts));

    // Store the cut for sharing and persistence
    StoredCut stored {
        .phase = static_cast<int>(src_phase),
        .scene = static_cast<int>(scene),
        .name = cut.name,
        .rhs = cut.lowb,
    };
    for (const auto& [col, coeff] : cut.cmap) {
      stored.coefficients.emplace_back(static_cast<int>(col), coeff);
    }
    m_stored_cuts_.push_back(std::move(stored));

    src_li.add_row(cut);
    ++total_cuts;

    SPDLOG_DEBUG("SDDP backward: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 pi - 1,
                 cut.lowb);

    // Re-solve source and handle iterative feasibility backpropagation.
    // If adding the cut makes phase k infeasible, build a feasibility
    // cut for phase k-1, continuing all the way to phase 0 if necessary.
    if (pi > 1) {
      auto r = src_li.resolve(opts);
      if (!r.has_value() || !src_li.is_optimal()) {
        // Iterative feasibility backpropagation:
        // If adding a cut makes phase k infeasible, build a feasibility
        // cut for phase k-1, continuing all the way to phase 0.
        for (Index back_pi = pi - 1; back_pi >= 0; --back_pi) {
          auto& back_li = planning_lp()
                              .system(scene, PhaseIndex {back_pi})
                              .linear_interface();

          if (back_pi > 0) {
            SPDLOG_WARN(
                "SDDP backward: scene {} phase {} infeasible after "
                "cut, backpropagating to phase {}",
                scene,
                back_pi,
                back_pi - 1);
          }

          // Apply elastic filter to make the infeasible phase solvable
          if (apply_elastic_filter(scene, PhaseIndex {back_pi}, opts)) {
            auto r2 = back_li.resolve(opts);
            if (r2.has_value() && back_li.is_optimal()) {
              if (back_pi > 0) {
                // Build a feasibility-like cut for the previous phase
                const auto prev_bp = PhaseIndex {back_pi - 1};
                auto& prev_li =
                    planning_lp().system(scene, prev_bp).linear_interface();
                const auto& prev_state = phase_states[prev_bp];

                auto feas_cut = build_benders_cut(
                    prev_state.alpha_col,
                    prev_state.outgoing_links,
                    back_li.get_col_cost(),
                    back_li.get_obj_value(),
                    std::format(
                        "sddp_feas_sc{}_ph{}_{}", scene, back_pi, total_cuts));

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

// ── Elastic filter ──────────────────────────────────────────────────────────

bool SDDPSolver::apply_elastic_filter(
    SceneIndex scene,
    PhaseIndex phase,
    [[maybe_unused]] const SolverOptions& opts)
{
  if (phase == PhaseIndex {0}) {
    return false;
  }

  auto& li = planning_lp().system(scene, phase).linear_interface();
  const auto prev = PhaseIndex {static_cast<Index>(phase) - 1};
  const auto& prev_state = m_scene_phase_states_[scene][prev];

  bool modified = false;
  for (const auto& link : prev_state.outgoing_links) {
    modified |=
        relax_fixed_state_variable(li, link, phase, m_options_.elastic_penalty);
  }
  return modified;
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
    for (Index si = 0; si < num_scenes; ++si) {
      const auto& cuts = scene_cuts[SceneIndex {si}];
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    // Compute average cut
    auto avg = average_benders_cut(
        all_cuts, std::format("sddp_avg_cut_ph{}", static_cast<Index>(phase)));

    // Add the average cut to all scenes
    for (Index si = 0; si < num_scenes; ++si) {
      auto& li =
          planning_lp().system(SceneIndex {si}, phase).linear_interface();
      li.add_row(avg);
    }

    SPDLOG_DEBUG(
        "SDDP sharing: added average cut to phase {} "
        "({} source cuts from {} scenes)",
        static_cast<Index>(phase),
        all_cuts.size(),
        num_scenes);

  } else if (m_options_.cut_sharing == CutSharingMode::Max) {
    // Add ALL cuts from ALL scenes to ALL scenes for this phase
    std::vector<SparseRow> all_cuts;
    for (Index si = 0; si < num_scenes; ++si) {
      const auto& cuts = scene_cuts[SceneIndex {si}];
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

    SPDLOG_DEBUG("SDDP sharing: added {} cuts to phase {} for all {} scenes",
                 all_cuts.size(),
                 static_cast<Index>(phase),
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
    // - phase: 0-based phase index this cut was added to
    // - scene: 0-based scene that generated the cut (-1 = shared/average)
    // - name:  human-readable cut identifier
    // - rhs:   right-hand side (lower bound of the cut row)
    // - remaining columns: col_idx:coeff pairs for non-zero coefficients
    ofs << "phase,scene,name,rhs,coefficients\n";
    for (const auto& cut : m_stored_cuts_) {
      ofs << cut.phase << "," << cut.scene << "," << cut.name << "," << cut.rhs;
      for (const auto& [col, coeff] : cut.coefficients) {
        ofs << "," << col << ":" << coeff;
      }
      ofs << "\n";
    }

    SPDLOG_INFO("SDDP: saved {} cuts to {}", m_stored_cuts_.size(), filepath);
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
      // broadcast to all scenes as warm-start approximations, analogous
      // to how PLP shares cuts across all its scenarios on restart.
      [[maybe_unused]] const auto scene_idx = std::stoi(token);

      std::getline(iss, token, ',');
      const auto cut_name = token;

      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      SparseRow row;
      row.name = std::format("loaded_{}", cut_name);
      row.lowb = rhs;
      row.uppb = LinearProblem::DblMax;

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

    SPDLOG_INFO("SDDP: loaded {} cuts from {}", cuts_loaded, filepath);
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
    for (Index si = 0; si < num_scenes; ++si) {
      auto fwd = fwd_futures[si].get();
      if (!fwd.has_value()) {
        return std::unexpected(std::move(fwd.error()));
      }
      total_upper += *fwd;
    }
    ir.upper_bound = total_upper / static_cast<double>(num_scenes);

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
        return std::unexpected(std::move(bwd.error()));
      }
      total_cuts += *bwd;
    }
    ir.cuts_added = total_cuts;

    // ── Cut sharing between scenes ──
    if (m_options_.cut_sharing != CutSharingMode::None && num_scenes > 1) {
      // Collect the cuts generated this iteration for sharing.
      // The cuts are already in m_stored_cuts_; find the ones from this
      // iteration by checking the most recent entries.
      const auto cuts_before = m_stored_cuts_.size() - total_cuts;
      for (Index pi = 0; pi < num_phases - 1; ++pi) {
        StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
        scene_cuts.resize(num_scenes);

        for (size_t ci = cuts_before; ci < m_stored_cuts_.size(); ++ci) {
          const auto& sc = m_stored_cuts_[ci];
          if (sc.phase == pi) {
            // Reconstruct the SparseRow
            SparseRow row;
            row.name = sc.name;
            row.lowb = sc.rhs;
            row.uppb = LinearProblem::DblMax;
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

    SPDLOG_INFO(
        "SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} cuts={} scenes={}{}",
        iter,
        ir.lower_bound,
        ir.upper_bound,
        ir.gap,
        ir.cuts_added,
        num_scenes,
        ir.converged ? " [CONVERGED]" : "");

    results.push_back(ir);

    if (ir.converged) {
      break;
    }
  }

  // Save cuts if output file is specified
  if (!m_options_.cuts_output_file.empty()) {
    auto save_result = save_cuts(m_options_.cuts_output_file);
    if (!save_result.has_value()) {
      SPDLOG_WARN("SDDP: could not save cuts: {}", save_result.error().message);
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
