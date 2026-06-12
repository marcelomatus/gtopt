/**
 * @file      solver_enums.hpp
 * @brief     Named enum types for LP solver options
 * @date      2026-03-29
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by SolverOptions.  Extracted from
 * solver_options.hpp so that the struct definition stays focused on
 * the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- SolverLogMode ----------------------------------------------------------

/**
 * @brief Controls whether and how solver log files are written.
 *
 * - `nolog`:    No solver log files are written (default).
 * - `detailed`: Separate log files per scene/phase/aperture, using the
 *               naming pattern `<solver>_sc<N>_ph<N>[_ap<N>].log`.
 *               Each thread writes to its own file -- no locking required.
 */
enum class SolverLogMode : uint8_t
{
  nolog = 0,  ///< No solver log files (default)
  detailed = 1,  ///< Separate log files per scene/phase/aperture
};

inline constexpr auto log_mode_entries =
    std::to_array<EnumEntry<SolverLogMode>>({
        {.name = "nolog", .value = SolverLogMode::nolog},
        {.name = "detailed", .value = SolverLogMode::detailed},
    });

/// ADL customization point for NamedEnum concept
[[nodiscard]] constexpr auto enum_entries(SolverLogMode /*tag*/) noexcept
{
  return std::span {log_mode_entries};
}

// --- LPAlgo -----------------------------------------------------------------

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
[[nodiscard]] constexpr auto enum_entries(LPAlgo /*tag*/) noexcept
{
  return std::span {lp_algo_entries};
}

/// Return the next algorithm in the LP fallback cycle:
/// barrier → dual → primal → barrier.  `default_algo` and `last_algo`
/// both map to `dual` (the cycle starts as if the input were barrier).
///
/// Used by `LinearInterface::initial_solve` / `resolve` to walk the
/// algorithm space when the primary algorithm fails to reach optimal.
/// Lives in this header so it is `constexpr`-testable in isolation
/// without spinning up an LP backend.
[[nodiscard]] constexpr LPAlgo next_fallback_algo(LPAlgo current) noexcept
{
  switch (current) {
    case LPAlgo::barrier:
      return LPAlgo::dual;
    case LPAlgo::dual:
      return LPAlgo::primal;
    case LPAlgo::primal:
      return LPAlgo::barrier;
    case LPAlgo::default_algo:
    case LPAlgo::last_algo:
      return LPAlgo::dual;
  }
  return LPAlgo::dual;
}

// --- SolverScaling -----------------------------------------------------------

/**
 * @brief Solver-internal scaling strategy.
 *
 * Controls whether and how the LP solver applies matrix scaling before
 * solving.  Each backend maps these values to its native parameter:
 *
 * | SolverScaling | CPLEX CPX_PARAM_SCAIND | HiGHS scale_strategy | CLP |
 * |---------------|------------------------|----------------------|-----|
 * | none          | -1                     | 0 (off)              | 0   |
 * | automatic     |  0 (equilibration)     | 4 (default)          | 3   |
 * | aggressive    |  1 (aggressive)        | 1 (forced)           | 2   |
 */
enum class SolverScaling : uint8_t
{
  none = 0,  ///< Disable solver-internal scaling
  automatic = 1,  ///< Solver's default/auto strategy (recommended)
  aggressive = 2,  ///< Aggressive equilibration (for high-kappa LPs)
};

inline constexpr auto solver_scaling_entries =
    std::to_array<EnumEntry<SolverScaling>>({
        {.name = "none", .value = SolverScaling::none},
        {.name = "automatic", .value = SolverScaling::automatic},
        {.name = "aggressive", .value = SolverScaling::aggressive},
    });

/// ADL customization point for NamedEnum concept
[[nodiscard]] constexpr auto enum_entries(SolverScaling /*tag*/) noexcept
{
  return std::span {solver_scaling_entries};
}

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
template<>
struct formatter<gtopt::SolverScaling> : formatter<string_view>
{
  template<typename FormatContext>
  auto format(gtopt::SolverScaling scaling, FormatContext& ctx) const
  {
    return formatter<string_view>::format(gtopt::enum_name(scaling), ctx);
  }
};
}  // namespace std
