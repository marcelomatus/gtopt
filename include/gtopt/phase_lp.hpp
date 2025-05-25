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
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/phase.hpp>
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
class PhaseLP
{
public:
  using StageSpan = std::span<const StageLP>;  ///< Span of StageLP objects
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
  template<class StageLPs = std::vector<StageLP>>
  explicit PhaseLP(Phase phase,
                   const StageLPs& all_stages = {},
                   PhaseIndex index = PhaseIndex {unknown_index})
      : m_phase_(std::move(phase))
      , m_stage_span_(std::span(all_stages)
                          .subspan(m_phase_.first_stage, m_phase_.count_stage))
      , m_duration_(ranges::fold_left(
            m_stage_span_ | ranges::views::transform(&StageLP::duration),
            0.0,
            std::plus<>()))
      , m_index_(index)
  {
  }

  /// @return Total duration of all stages in this phase (hours)
  [[nodiscard]] constexpr auto duration() const noexcept { return m_duration_; }

  /// @return Whether this phase is active in the planning
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_phase_.active.value_or(true);
  }

  /// @return Unique identifier for this phase
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return PhaseUid {m_phase_.uid};
  }

  /// @return Index of this phase in parent container
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

  /// @return Span of all StageLP objects in this phase
  [[nodiscard]] constexpr auto& stages() const noexcept
  {
    return m_stage_span_;
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

  /// @return Index of last stage in this phase (first_stage + count_stage - 1)
  [[nodiscard]] constexpr auto last_stage() const noexcept
  {
    return StageIndex {static_cast<Index>(m_phase_.first_stage 
                                        + m_phase_.count_stage - 1)};
  }

  /// @return Whether this phase contains the given stage index
  [[nodiscard]] constexpr auto contains(StageIndex stage_index) const noexcept
  {
    return stage_index >= first_stage() && stage_index <= last_stage();
  }

private:
  Phase m_phase_;  ///< The underlying Phase object
  StageSpan m_stage_span_;  ///< Span of StageLP objects in this phase
  double m_duration_ {0.0};  ///< Total duration of all stages in this phase
  PhaseIndex m_index_ {unknown_index};
};

}  // namespace gtopt
