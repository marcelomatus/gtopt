/**
 * @file      phase_lp.hpp
 * @brief     Linear programming phase representation
 * @date      Wed Apr  9 22:04:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the PhaseLP class which represents a time phase in linear programming
 * planning problems. A phase contains multiple stages and provides aggregate
 * calculations across them.
 */

#pragma once

#include <functional>

#include <gtopt/basic_types.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

/**
 * @class PhaseLP
 * @brief Linear programming phase containing multiple stages
 *
 * Represents a contiguous time period (phase) in the planning problem,
 * composed of multiple StageLP objects. Provides:
 * - Access to contained stages
 * - Phase duration calculation
 * - Phase indexing and identification
 * - Active/inactive status checks
 */

namespace detail
{

[[nodiscard]] constexpr auto create_stage_array(
    const Phase& phase,
    PhaseIndex phase_index,
    double annual_discount_rate,
    std::span<const Stage> stage_array,
    std::span<const Block> all_blocks)
{
  auto stages = stage_array.subspan(phase.first_stage, phase.count_stage);

  return std::ranges::to<std::vector>(enumerate_active<StageIndex>(stages)
                                      | std::ranges::views::transform(
                                          [&](auto&& is)
                                          {
                                            const auto& [stage_index, stage] =
                                                is;
                                            return StageLP {
                                                stage,
                                                all_blocks,
                                                annual_discount_rate,
                                                stage_index,
                                                phase_index,
                                            };
                                          }));
}
}  // namespace detail

class PhaseLP
{
public:
  /**
   * @brief Default constructor
   */
  PhaseLP() noexcept = default;

  /**
   * @brief Construct a PhaseLP from phase definition and stages
   *
   * @param phase        Phase definition containing first_stage/count_stage
   * @param options      Planning options (provides annual_discount_rate)
   * @param stages       Complete collection of all available stages
   * @param blocks       Complete collection of all available blocks
   * @param phase_index  Index of this phase in parent container
   *
   * @note The phase will contain stages[first_stage..first_stage+count_stage-1]
   */
  explicit PhaseLP(Phase phase,
                   const PlanningOptionsLP& options,
                   std::span<const Stage> stages = {},
                   std::span<const Block> blocks = {},
                   PhaseIndex phase_index = PhaseIndex {unknown_index})
      : m_phase_(std::move(phase))
      , m_index_(phase_index)
      , m_stages_(detail::create_stage_array(
            m_phase_, m_index_, options.annual_discount_rate(), stages, blocks))
      , m_duration_(std::ranges::fold_left(
            m_stages_ | std::ranges::views::transform(&StageLP::duration),
            0.0,
            std::plus<>()))
  {
  }

  explicit PhaseLP(Phase phase,
                   const PlanningOptionsLP& options,
                   const Simulation& simulation,
                   PhaseIndex phase_index = PhaseIndex {unknown_index})
      : m_phase_(std::move(phase))
      , m_index_(phase_index)
      , m_stages_(
            detail::create_stage_array(m_phase_,
                                       m_index_,
                                       simulation.annual_discount_rate.value_or(
                                           options.annual_discount_rate()),
                                       simulation.stage_array,
                                       simulation.block_array))
      , m_duration_(std::ranges::fold_left(
            m_stages_ | std::ranges::views::transform(&StageLP::duration),
            0.0,
            std::plus<>()))
  {
  }

  /// @return Total duration of all stages in this phase (hours)
  [[nodiscard]] constexpr auto duration() const noexcept { return m_duration_; }

  /// @return Whether this phase is active in the planning
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_phase_.is_active();
  }

  /// @return Whether this phase uses continuous LP relaxation
  [[nodiscard]] constexpr auto is_continuous() const noexcept
  {
    return m_phase_.is_continuous();
  }

  /// @return Unique identifier for this phase
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return make_uid<Phase>(m_phase_.uid);
  }

  /// @return Index of this phase in parent container
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

  /// @return Span of all StageLP objects in this phase
  [[nodiscard]] constexpr const auto& stages() const noexcept
  {
    return m_stages_;
  }

  /// @return Index of first stage in this phase
  [[nodiscard]] constexpr auto first_stage() const noexcept
  {
    return StageIndex {static_cast<Index>(m_phase_.first_stage)};
  }

  /// @return Number of stages in this phase
  [[nodiscard]] constexpr auto count_stage() const noexcept
  {
    return static_cast<Index>(m_phase_.count_stage);
  }

  /// @return Per-phase aperture UIDs (empty = use all global apertures)
  [[nodiscard]] constexpr const auto& apertures() const noexcept
  {
    return m_phase_.apertures;
  }

private:
  Phase m_phase_ {};  ///< The underlying Phase object
  PhaseIndex m_index_ {unknown_index};
  std::vector<StageLP> m_stages_;  ///< Span of StageLP objects in this phase
  double m_duration_ {0.0};  ///< Total duration of all stages in this phase
};

}  // namespace gtopt
