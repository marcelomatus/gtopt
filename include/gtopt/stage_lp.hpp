/**
 * @file      stage_lp.hpp
 * @brief     Header file defining the StageLP class for linear programming
 * stages
 * @date      Wed Mar 26 12:10:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for managing stages in linear programming
 * optimization problems
 */

#pragma once

#include <numeric>
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

/**
 * @brief A class representing a stage in a linear programming optimization
 * problem
 *
 * Encapsulates a collection of blocks with time duration, discounting, and
 * indexing
 */
class StageLP
{
public:
  using BlockSpan = std::span<const BlockLP>;
  using BlockIndexes = std::vector<BlockIndex>;
  using BlockIndexSpan = std::span<const BlockIndex>;

  StageLP() = default;

  /**
   * @brief Constructs a StageLP from a Stage and a collection of blocks
   *
   * @tparam Blocks Container type for BlockLP objects
   * @param pstage The stage definition
   * @param pblocks Collection of blocks
   * @param annual_discount_rate Annual discount rate for time value
   * calculations
   */
  template<class Blocks>
  explicit StageLP(Stage pstage,
                   const Blocks& pblocks,
                   double annual_discount_rate = 0.0)
      : stage(std::move(pstage))
      , block_span(
            std::span(pblocks).subspan(stage.first_block, stage.count_block))
      , span_duration(std::transform_reduce(block_span.begin(),
                                            block_span.end(),
                                            0.0,
                                            std::plus(),
                                            [](const auto& b)
                                            { return b.duration(); }))
      , annual_discount_factor(std::pow(1.0 / (1.0 + annual_discount_rate),
                                        span_duration / hours_per_year))
  {
  }

  /// @return Total duration of the stage in hours
  [[nodiscard]] constexpr auto duration() const noexcept
  {
    return span_duration;
  }

  /// @return Combined discount factor (annual and stage-specific)
  [[nodiscard]] constexpr auto discount_factor() const noexcept
  {
    return annual_discount_factor * stage.discount_factor.value_or(1.0);
  }

  /// @return Whether this stage is active in the optimization
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return stage.active.value_or(true);
  }

  /// @return Unique identifier for this stage
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return StageUid {stage.uid};
  }

  /// @return Span of blocks in this stage
  [[nodiscard]] constexpr const auto& blocks() const noexcept
  {
    return block_span;
  }

private:
  Stage stage;  ///< Stage definition
  BlockSpan block_span;  ///< View of blocks in this stage

  double span_duration {0.0};  ///< Total duration of all blocks
  double annual_discount_factor {
      1.0};  ///< Applied discount factor based on duration
};

}  // namespace gtopt
