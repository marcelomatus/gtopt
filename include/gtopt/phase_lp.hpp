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
#include <numeric>
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
  using StageIndexes =
      std::vector<StageIndex>;  ///< Collection of stage indices
  using StageIndexSpan =
      std::span<const StageIndex>;  ///< Span of stage indices

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
  template<class Stages>
  explicit PhaseLP(Phase pphase, const Stages& pstages)
      : phase(std::move(pphase))
      , stage_span(
            std::span(pstages).subspan(phase.first_stage, phase.count_stage))
      , span_duration(std::transform_reduce(stage_span.begin(),
                                            stage_span.end(),
                                            0.0,
                                            std::plus<>(),
                                            [](const auto& b) noexcept
                                            { return b.duration(); }))
  {
  }

  /**
   * @brief Get the total duration of the phase
   * @return Total duration as a double
   */
  [[nodiscard]] constexpr auto duration() const noexcept
  {
    return span_duration;
  }

  /**
   * @brief Check if the phase is active
   * @return true if the phase is active, false otherwise
   */
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return phase.active.value_or(true);
  }

  /**
   * @brief Get the unique identifier of the phase
   * @return Phase UID
   */
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return PhaseUid {phase.uid};
  }

  /**
   * @brief Get a span of all stages in this phase
   * @return Span of StageLP objects
   */
  [[nodiscard]] constexpr auto& stages() const noexcept { return stage_span; }

  [[nodiscard]] constexpr auto first_stage() const { return phase.first_stage; }

  [[nodiscard]] constexpr auto count_stage() const { return phase.count_stage; }

private:
  Phase phase;  ///< The underlying Phase object
  StageSpan stage_span;  ///< Span of StageLP objects in this phase
  double span_duration {0.0};  ///< Total duration of all stages in this phase
};

}  // namespace gtopt
