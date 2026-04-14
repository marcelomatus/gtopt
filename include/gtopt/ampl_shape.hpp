/**
 * @file      ampl_shape.hpp
 * @brief     Dimensional shape tag for PAMPL-visible variables and parameters
 * @date      2026-04-10
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Every entry in the AMPL registry (variable or parameter) carries an
 * `AmplShape` tag declaring its dimensional dependency.  Five values
 * cover everything that exists in gtopt today:
 *
 *   - `Scalar`: a single value with no dependence on (scenario, stage,
 *     block).  Used for option constants such as
 *     `options.annual_discount_rate`.
 *   - `T`:  varies with stage only.  Used for stage-level scheduled
 *     parameters such as `generator.gcost` or `line.reactance`.
 *   - `TB`: varies with (stage, block).  Used for block-level scheduled
 *     parameters such as `generator.pmax`, `line.tmax_ab`.
 *   - `ST`: varies with (scenario, stage).  Used for scenario-stage
 *     scalar variables such as `generator.capainst`, `reservoir.eini`.
 *   - `STB`: varies with (scenario, stage, block).  Used for the dense
 *     LP variables: `generator.generation`, `line.flowp`, `bus.theta`.
 *
 * The unused subsets (`S`, `SB`, `B`, `∅` interpreted as anything other
 * than scalar) do not correspond to any entity that exists in the
 * formulation and are intentionally not representable.
 *
 * The bit layout (block in bit 1, scenario in bit 2, stage in bit 0
 * since stage participation is the most common) is purely an internal
 * convenience for the `shape_has_*` predicates and is not part of the
 * stable API; consumers should always go through the predicates rather
 * than testing bits directly.
 */

#pragma once

#include <cstdint>

namespace gtopt
{

/// Dimensional dependency tag for AMPL-visible entities.
enum class AmplShape : std::uint8_t
{
  Scalar = 0b000,  ///< Single value (e.g. options.annual_discount_rate).
  T = 0b001,  ///< Stage-only (e.g. gcost, reactance).
  TB = 0b011,  ///< Stage × block (e.g. pmax, tmax_ab).
  ST = 0b101,  ///< Scenario × stage (e.g. capainst, eini).
  STB = 0b111,  ///< Scenario × stage × block (e.g. generation, flowp).
};

[[nodiscard]] constexpr bool shape_has_stage(AmplShape s) noexcept
{
  return (static_cast<std::uint8_t>(s) & 0b001U) != 0U;
}

[[nodiscard]] constexpr bool shape_has_block(AmplShape s) noexcept
{
  return (static_cast<std::uint8_t>(s) & 0b010U) != 0U;
}

[[nodiscard]] constexpr bool shape_has_scenario(AmplShape s) noexcept
{
  return (static_cast<std::uint8_t>(s) & 0b100U) != 0U;
}

/// Number of indices required to subscript an entity of this shape.
[[nodiscard]] constexpr std::uint8_t shape_arity(AmplShape s) noexcept
{
  return static_cast<std::uint8_t>(shape_has_scenario(s))
      + static_cast<std::uint8_t>(shape_has_stage(s))
      + static_cast<std::uint8_t>(shape_has_block(s));
}

}  // namespace gtopt
