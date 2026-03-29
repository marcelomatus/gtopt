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

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>

#include <gtopt/enum_option.hpp>

namespace gtopt
{
/**
 * @brief Controls whether and how solver log files are written.
 *
 * - `nolog`:    No solver log files are written (default).
 * - `detailed`: Separate log files per scene/phase/aperture, using the
 *               naming pattern `<solver>_sc<N>_ph<N>[_ap<N>].log`.
 *               Each thread writes to its own file — no locking required.
 */
enum class SolverLogMode : uint8_t
{
  nolog = 0,  ///< No solver log files (default)
  detailed = 1,  ///< Separate log files per scene/phase/aperture
};

inline constexpr auto solver_log_mode_entries =
    std::to_array<EnumEntry<SolverLogMode>>({
        {.name = "nolog", .value = SolverLogMode::nolog},
        {.name = "detailed", .value = SolverLogMode::detailed},
    });

/// ADL customization point for NamedEnum concept
constexpr auto enum_entries(SolverLogMode /*tag*/) noexcept
{
  return std::span {solver_log_mode_entries};
}

/**
 * @brief Enumeration of linear programming solution algorithms
 */
enum class LPAlgo : uint8_t
{
  /** @brief Use the solver's default algorithm */
  default_algo = 0,

  /** @brief Use the primal simplex algorithm */
  primal = 1,

  /** @brief Use the dual simplex algorithm */
  dual = 2,

  /** @brief Use the interior point (barrier) algorithm */
  barrier = 3,

  /** @brief Sentinel value for iteration/validation */
  last_algo = 4,
};

/**
 * @brief Compile-time table mapping each LPAlgo enumerator to its name.
 *
 * Excludes the sentinel @c last_algo value.
 */
inline constexpr auto lp_algo_entries = std::to_array<EnumEntry<LPAlgo>>({
    {.name = "default", .value = LPAlgo::default_algo},
    {.name = "primal", .value = LPAlgo::primal},
    {.name = "dual", .value = LPAlgo::dual},
    {.name = "barrier", .value = LPAlgo::barrier},
});

/// ADL customization point for NamedEnum concept
constexpr auto enum_entries(LPAlgo /*tag*/) noexcept
{
  return std::span {lp_algo_entries};
}

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
  /** @brief The solution algorithm to use */
  LPAlgo algorithm {LPAlgo::barrier};

  /** @brief Number of parallel threads to use (0 = solver default) */
  int threads {2};

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
  std::optional<SolverLogMode> solver_log_mode {};

  /** @brief Time limit for individual LP solves in seconds.
   *  0 = no limit (solver default).  When non-zero, the solver will abort
   *  the current solve if the wall-clock time exceeds this value.
   *  The caller should check `is_optimal()` after solve to detect timeouts.
   */
  std::optional<double> time_limit {};

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
  void merge(const SolverOptions& other) noexcept
  {
    if (!optimal_eps && other.optimal_eps) {
      optimal_eps = other.optimal_eps;
    }
    if (!feasible_eps && other.feasible_eps) {
      feasible_eps = other.feasible_eps;
    }
    if (!barrier_eps && other.barrier_eps) {
      barrier_eps = other.barrier_eps;
    }
    if (!time_limit && other.time_limit) {
      time_limit = other.time_limit;
    }
  }
};

}  // namespace gtopt

// Specialize std::formatter for LPAlgo using its canonical name
namespace std
{
template<>
struct formatter<gtopt::LPAlgo> : formatter<string_view>
{
  template<typename FormatContext>
  auto format(gtopt::LPAlgo algo, FormatContext& ctx) const
  {
    return formatter<string_view>::format(gtopt::enum_name(algo), ctx);
  }
};
template<>
struct formatter<gtopt::SolverLogMode> : formatter<string_view>
{
  template<typename FormatContext>
  auto format(gtopt::SolverLogMode mode, FormatContext& ctx) const
  {
    return formatter<string_view>::format(gtopt::enum_name(mode), ctx);
  }
};
}  // namespace std
