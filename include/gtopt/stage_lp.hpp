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

#include <numeric>
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
  template<typename BlockLPs = std::vector<BlockLP> >
  explicit StageLP(Stage pstage,
                   const BlockLPs& pblocks = {},
                   double annual_discount_rate = 0.0,
                   StageIndex index = {})
      : m_stage_(std::move(pstage))
      , m_blocks_(std::span(pblocks).subspan(m_stage_.first_block,
                                             m_stage_.count_block))
      , m_timeinit_(
            std::transform_reduce(pblocks.begin(),
                                  pblocks.begin() + m_stage_.first_block,
                                  0.0,
                                  std::plus(),
                                  [](const auto& b) { return b.duration(); }))
      , m_duration_(std::transform_reduce(m_blocks_.begin(),
                                          m_blocks_.end(),
                                          0.0,
                                          std::plus(),
                                          [](const auto& b)
                                          { return b.duration(); }))
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

  StageIndex m_index_;
};

}  // namespace gtopt
