/**
 * @file      stage_lp.hpp
 * @brief     Header file defining the StageLP class for linear programming
 * stages
 * @date      Wed Mar 26 12:10:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for managing stages in linear programming
 * planning problems
 */

#pragma once

#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief A class representing a stage in a linear programming planning
 * problem
 *
 * Encapsulates a collection of blocks with time duration, discounting, and
 * indexing
 */
class StageLP
{
public:
  using BlockSpan = std::span<const BlockLP>;

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
  template<typename BlockLPs = std::vector<BlockLP> >
  explicit StageLP(Stage stage,
                   const BlockLPs& all_blocks = {},
                   double annual_discount_rate = 0.0,
                   StageIndex index = StageIndex {unknown_index})
      : m_stage_(std::move(stage))
      , m_blocks_(std::span(all_blocks)
                      .subspan(m_stage_.first_block, m_stage_.count_block))
      , m_timeinit_(ranges::fold_left(
            all_blocks | ranges::views::take(stage.first_block)
                | ranges::views::transform(&BlockLP::duration),
            0.0,
            std::plus()))
      , m_duration_(ranges::fold_left(
            m_blocks_ | ranges::views::transform(&BlockLP::duration),
            0.0,
            std::plus<>()))
      , m_discount_factor_(
            annual_discount_factor(annual_discount_rate, m_timeinit_))
      , m_index_(index)
  {
  }

  [[nodiscard]] constexpr const auto& stage() const noexcept
  {
    return m_stage_;
  }

  [[nodiscard]] constexpr auto timeinit() const noexcept { return m_timeinit_; }

  /// @return Total duration of the stage in hours
  [[nodiscard]] constexpr auto duration() const noexcept { return m_duration_; }

  /// @return Combined discount factor (annual and stage-specific)
  [[nodiscard]] constexpr auto discount_factor() const noexcept
  {
    return m_discount_factor_ * stage().discount_factor.value_or(1.0);
  }

  /// @return Whether this stage is active in the planning
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return stage().active.value_or(true);
  }

  /// @return Unique identifier for this stage
  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return StageUid {stage().uid};
  }

  /// @return Index of this stage in the parent container
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

  /// @return Span of blocks in this stage
  [[nodiscard]] constexpr const auto& blocks() const noexcept
  {
    return m_blocks_;
  }

private:
  Stage m_stage_;  ///< Stage definition
  BlockSpan m_blocks_;  ///< View of blocks in this m_stage_
  double m_timeinit_ {0.0};
  double m_duration_ {0.0};  ///< Total duration of the m_stage_
  double m_discount_factor_ {1.0};

  StageIndex m_index_ {unknown_index};
};

}  // namespace gtopt
