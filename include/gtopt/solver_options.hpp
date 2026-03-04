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
#include <ranges>
#include <string_view>

namespace gtopt
{
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
 * @brief Name–value pair for an LPAlgo enumerator.
 *
 * With C++26 static reflection (P2996 `std::meta::enumerators_of`) this table
 * could be generated automatically from the @c LPAlgo enum definition.
 * Until broad compiler support arrives it is maintained here, next to the
 * enum itself, as the single source of truth.
 */
struct LPAlgoEntry
{
  std::string_view name;
  LPAlgo value;
};

/**
 * @brief Compile-time table mapping each LPAlgo enumerator to its name.
 *
 * Excludes the sentinel @c last_algo value.  With C++26 P2996 reflection
 * this would be derived from `std::meta::enumerators_of(^LPAlgo)` at
 * compile time without any manual maintenance.
 */
inline constexpr auto lp_algo_entries = std::to_array<LPAlgoEntry>({
    {"default", LPAlgo::default_algo},
    {"primal", LPAlgo::primal},
    {"dual", LPAlgo::dual},
    {"barrier", LPAlgo::barrier},
});

/**
 * @brief Look up an LPAlgo enumerator by name.
 *
 * @param name  Case-sensitive algorithm name (e.g. @c "barrier").
 * @return The matching @c LPAlgo value, or @c std::nullopt if not found.
 */
[[nodiscard]] constexpr std::optional<LPAlgo> lp_algo_from_name(
    std::string_view name) noexcept
{
  const auto it = std::ranges::find_if(
      lp_algo_entries, [name](const LPAlgoEntry& e) { return e.name == name; });
  if (it != lp_algo_entries.end()) {
    return it->value;
  }
  return std::nullopt;
}

/**
 * @brief Return the canonical name of an LPAlgo enumerator.
 *
 * @param algo  The algorithm value.
 * @return The name string, or @c "unknown" for out-of-range values.
 */
[[nodiscard]] constexpr std::string_view lp_algo_name(LPAlgo algo) noexcept
{
  const auto it = std::ranges::find_if(lp_algo_entries,
                                       [algo](const LPAlgoEntry& e)
                                       { return e.value == algo; });
  return it != lp_algo_entries.end() ? it->name : "unknown";
}

/**
 * @brief Configuration options for linear programming solvers
 *
 * The SolverOptions structure contains parameters that control how linear
 * programming problems are solved, including algorithm selection, parallel
 * processing settings, numerical tolerances, and logging preferences.
 */
struct SolverOptions
{
  /** @brief The solution algorithm to use */
  LPAlgo algorithm {LPAlgo::barrier};

  /** @brief Number of parallel threads to use (0 = automatic) */
  int threads {0};

  /** @brief Whether to apply presolve optimizations (default: true) */
  bool presolve {true};

  /** @brief Optimality tolerance for solution */
  double optimal_eps {1e-6};

  /** @brief Feasibility tolerance for constraints */
  double feasible_eps {1e-6};

  /** @brief Convergence tolerance for barrier algorithm */
  double barrier_eps {1e-6};

  /** @brief Verbosity level for solver output (0 = none) */
  int log_level {0};
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
    return formatter<string_view>::format(gtopt::lp_algo_name(algo), ctx);
  }
};
}  // namespace std
