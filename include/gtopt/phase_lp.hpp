/**
 * @file      phase_lp.hpp
 * @brief     Header file for the PhaseLP class, which represents a linear
 * program phase
 * @date      Wed Apr  9 22:04:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for managing a phase in a linear
 * programming context, encapsulating a collection of StageLP objects and their
 * properties.
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
 * @brief Represents a phase in a linear programming planning problem
 *
 * PhaseLP encapsulates a sequence of StageLP objects that form a cohesive
 * phase in the planning process. It provides access to the contained
 * stages and calculates aggregate properties such as total duration.
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
   * @brief Constructs a PhaseLP from a Phase object and a collection of
   * stages
   *
   * @tparam Stages Type of the stages collection
   * @param pphase The Phase object containing phase metadata
   * @param pstages Collection of StageLP objects
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

  /**
   * @brief Get the total duration of the phase
   * @return Total duration as a double
   */
  [[nodiscard]] constexpr auto duration() const noexcept { return m_duration_; }

  /**
   * @brief Check if the phase is active
   * @return true if the phase is active, false otherwise
   */
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_phase_.active.value_or(true);
  }

  /**
   * @brief Get the unique identifier of the phase
   * @return Phase UID
   */
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return PhaseUid {m_phase_.uid};
  }

  /**
   * @brief Get a span of all stages in this phase
   * @return Span of StageLP objects
   */
  [[nodiscard]] constexpr auto& stages() const noexcept
  {
    return m_stage_span_;
  }

  [[nodiscard]] constexpr auto first_stage() const
  {
    return StageIndex {static_cast<Index>(m_phase_.first_stage)};
  }

  [[nodiscard]] constexpr auto count_stage() const
  {
    return static_cast<Index>(m_phase_.count_stage);
  }

  /// @return Index of this phase in the parent container
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

private:
  Phase m_phase_;  ///< The underlying Phase object
  StageSpan m_stage_span_;  ///< Span of StageLP objects in this phase
  double m_duration_ {0.0};  ///< Total duration of all stages in this phase
  PhaseIndex m_index_ {unknown_index};
};

}  // namespace gtopt
