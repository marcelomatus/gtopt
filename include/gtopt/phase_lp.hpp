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
#include <gtopt/options_lp.hpp>
#include <gtopt/phase.hpp>
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
    const OptionsLP& options,
    std::span<const Stage> stage_array,
    std::span<const Block> all_blocks)
{
  auto stages = stage_array.subspan(phase.first_stage, phase.count_stage);

  return enumerate_active<StageIndex>(stages)
      | std::ranges::views::transform(
             [&](auto&& is)
             {
               const auto& [stage_index, stage] = is;
               return StageLP {
                   stage,
                   all_blocks,
                   options.annual_discount_rate(),
                   stage_index,
                   phase_index,
               };
             })
      | std::ranges::to<std::vector>();
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
   * @tparam StageLPs  Type of stages container (defaults to vector<StageLP>)
   * @param phase      Phase definition containing first_stage/count_stage
   * @param all_stages Complete collection of all available stages
   * @param index      Index of this phase in parent container
   *
   * @note The phase will contain stages[first_stage..first_stage+count_stage-1]
   */
  explicit PhaseLP(Phase phase,
                   const OptionsLP& options,
                   std::span<const Stage> stages = {},
                   std::span<const Block> blocks = {},
                   PhaseIndex phase_index = PhaseIndex {unknown_index})
      : m_phase_(std::move(phase))
      , m_index_(phase_index)
      , m_stages_(detail::create_stage_array(
            m_phase_, m_index_, options, stages, blocks))
      , m_duration_(std::ranges::fold_left(
            m_stages_ | std::ranges::views::transform(&StageLP::duration),
            0.0,
            std::plus<>()))
  {
  }

  explicit PhaseLP(Phase phase,
                   const OptionsLP& options,
                   const Simulation& simulation,
                   PhaseIndex phase_index = PhaseIndex {unknown_index})
      : PhaseLP(std::move(phase),
                options,
                simulation.stage_array,
                simulation.block_array,
                phase_index)
  {
  }

  /// @return Total duration of all stages in this phase (hours)
  [[nodiscard]] constexpr auto duration() const noexcept { return m_duration_; }

  /// @return Whether this phase is active in the planning
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_phase_.is_active();
  }

  /// @return Unique identifier for this phase
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return PhaseUid {m_phase_.uid};
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

private:
  Phase m_phase_ {};  ///< The underlying Phase object
  PhaseIndex m_index_ {unknown_index};
  std::vector<StageLP> m_stages_;  ///< Span of StageLP objects in this phase
  double m_duration_ {0.0};  ///< Total duration of all stages in this phase
};

}  // namespace gtopt
