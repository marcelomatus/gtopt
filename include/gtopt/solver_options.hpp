/**
 * @file      solver_options.hpp
 * @brief     Linear programming solver configuration options
 * @date      Mon Mar 24 10:24:13 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines options specific to controlling linear programming
 * solvers, including algorithm selection, solver parameters, and tolerances.
 */

#pragma once

#include <optional>

#include <gtopt/solver_enums.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief Configuration options for linear programming solvers
 *
 * The SolverOptions structure contains parameters that control how linear
 * programming problems are solved, including algorithm selection, parallel
 * processing settings, numerical tolerances, and logging preferences.
 *
 * The three tolerance fields (@c optimal_eps, @c feasible_eps,
 * @c barrier_eps) are **optional**: when they are @c std::nullopt the solver
 * keeps its built-in default values instead of overriding them.
 */
struct SolverOptions
{
  /** @brief The solution algorithm to use (default_algo = use backend optimal)
   */
  LPAlgo algorithm {LPAlgo::default_algo};

  /** @brief Number of parallel threads to use (0 = use backend optimal) */
  int threads {0};

  /** @brief Whether to apply presolve optimizations (default: true) */
  bool presolve {true};

  /** @brief Optimality tolerance for solution (nullopt = use solver default)
   */
  std::optional<double> optimal_eps {};

  /** @brief Feasibility tolerance for constraints (nullopt = use solver
   * default) */
  std::optional<double> feasible_eps {};

  /** @brief Convergence tolerance for barrier algorithm (nullopt = use solver
   * default) */
  std::optional<double> barrier_eps {};

  /** @brief Verbosity level for solver output (0 = none) */
  int log_level {0};

  /** @brief Controls solver log file generation.
   *
   * - `nolog`:     No log files written (default).
   * - `detailed`:  Separate log file per scene/phase/aperture, named
   *                `<solver>_sc<N>_ph<N>[_ap<N>].log`.
   */
  std::optional<SolverLogMode> log_mode {};

  /** @brief Time limit for individual LP solves in seconds.
   *  0 = no limit (solver default).  When non-zero, the solver will abort
   *  the current solve if the wall-clock time exceeds this value.
   *  The caller should check `is_optimal()` after solve to detect timeouts.
   */
  std::optional<double> time_limit {};

  /** @brief Solver-internal scaling strategy.
   *
   *  Controls how the LP solver scales the constraint matrix before solving.
   *  When nullopt, the solver keeps its built-in default.
   *
   *  @see SolverScaling for the available strategies and backend mapping.
   */
  std::optional<SolverScaling> scaling {};

  /** @brief Controls barrier crossover (internal, not user-visible).
   *
   *  Crossover converts the interior-point solution into a basic feasible
   *  solution, producing exact dual values (row prices / reduced costs).
   *  Currently only disabled for the elastic filter clone solve
   *  (SDDPMethod::elastic_solve), which never needs duals.
   *
   *  All SDDP main passes (forward, backward, simulation) keep crossover
   *  enabled because the backward pass needs forward-pass duals for
   *  Benders cut generation.
   *
   *  Only meaningful when algorithm == barrier.  Simplex methods always
   *  produce duals by construction.
   *
   *  Backend mapping:
   *  - CPLEX: true → `BARCROSSALG=1` (primal), false → `BARCROSSALG=-1`
   *  - HiGHS: false → `run_crossover="off"`
   *  - MindOpt: false → `SolutionTarget=2` (interior-point only)
   *  - CLP: ignored (CLP barrier always does crossover)
   */
  bool crossover {true};

  /** @brief Maximum algorithm fallback attempts on non-optimal solve.
   *
   *  When a solve returns non-optimal, the solver cycles through
   *  alternative algorithms (barrier → dual → primal → barrier) up to
   *  this many times.  0 = no fallback (fail immediately).
   *  Default: 2 (try all three algorithms).
   */
  int max_fallbacks {2};

  /** @brief Enable basis-reuse optimizations for resolve on cloned LPs.
   *
   *  When true, set_solver_opts() overrides the algorithm to dual simplex
   *  and disables presolve — both critical for basis-reuse resolves.
   *  This is especially important when the original LP was solved with
   *  barrier: barrier solutions carry no simplex basis, so resolving
   *  a clone with barrier would restart from scratch, whereas dual simplex
   *  can pivot from the crossover basis in a few iterations.
   *
   *  On CLP, additional specialOptions bits are set to retain the
   *  factorization and work arrays from the cloned LP.
   */
  bool reuse_basis {false};

  /**
   * @brief Merge another SolverOptions into this one (first-value-wins for
   * optional fields).
   *
   * Only the optional tolerance / limit fields are merged; the non-optional
   * fields (algorithm, threads, presolve, log_level) are not changed by
   * merge — they keep their already-set values.
   */
  void merge(const SolverOptions& other)
  {
    merge_opt(optimal_eps, other.optimal_eps);
    merge_opt(feasible_eps, other.feasible_eps);
    merge_opt(barrier_eps, other.barrier_eps);
    merge_opt(time_limit, other.time_limit);
    merge_opt(log_mode, other.log_mode);
    merge_opt(scaling, other.scaling);
  }

  /**
   * @brief Overlay user-supplied options on top of backend defaults.
   *
   * Start from backend optimal defaults (this), then apply any user
   * option that is not at its sentinel/default value.  User settings
   * always win over backend defaults.
   *
   * Sentinels: algorithm=default_algo, threads=0, log_level=0,
   * max_fallbacks=2, presolve=true, reuse_basis=false.
   * Optional fields: has_value() means user specified.
   */
  void overlay(const SolverOptions& user)
  {
    if (user.algorithm != LPAlgo::default_algo) {
      algorithm = user.algorithm;
    }
    if (user.threads != 0) {
      threads = user.threads;
    }
    if (!user.presolve) {
      presolve = user.presolve;
    }
    if (user.reuse_basis) {
      reuse_basis = user.reuse_basis;
    }
    if (user.log_level != 0) {
      log_level = user.log_level;
    }
    if (user.max_fallbacks != 2) {
      max_fallbacks = user.max_fallbacks;
    }
    // optional fields: user wins if specified
    merge_opt(optimal_eps, user.optimal_eps);
    merge_opt(feasible_eps, user.feasible_eps);
    merge_opt(barrier_eps, user.barrier_eps);
    merge_opt(time_limit, user.time_limit);
    merge_opt(log_mode, user.log_mode);
    merge_opt(scaling, user.scaling);
  }
};

}  // namespace gtopt
