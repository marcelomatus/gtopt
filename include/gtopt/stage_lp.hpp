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
#include <gtopt/phase.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

namespace details
{
constexpr auto create_block_array(const auto& block_array, const Stage& stage)
{
  auto&& blocks =
      std::span(block_array).subspan(stage.first_block, stage.count_block);

  return blocks | ranges::views::transform([](auto&& b) { return BlockLP {b}; })
      | ranges::to<std::vector>();
}
}  // namespace details

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
  template<typename Blocks = std::vector<Block> >
  explicit StageLP(Stage stage,
                   const Blocks& blocks = {},
                   double annual_discount_rate = 0.0,
                   StageIndex stage_index = StageIndex {unknown_index},
                   PhaseIndex phase_index = PhaseIndex {unknown_index})
      : m_stage_(std::move(stage))
      , m_blocks_(details::create_block_array(blocks, m_stage_))
      , m_timeinit_(ranges::fold_left(
            blocks | ranges::views::take(stage.first_block)
                | ranges::views::transform([](const Block& b)
                                           { return b.duration; }),
            0.0,
            std::plus()))
      , m_duration_(ranges::fold_left(
            m_blocks_ | ranges::views::transform(&BlockLP::duration),
            0.0,
            std::plus<>()))
      , m_discount_factor_(
            annual_discount_factor(annual_discount_rate, m_timeinit_))
      , m_index_(stage_index)
      , m_phase_index_(phase_index)
  {
  }

  [[nodiscard]] constexpr const auto& stage() const noexcept
  {
    return m_stage_;
  }

  [[nodiscard]] constexpr auto timeinit() const noexcept { return m_timeinit_; }

  /// @return Total duration of the stage in hours
  [[nodiscard]] constexpr auto duration() const noexcept { return m_duration_; }

  [[nodiscard]] constexpr auto total_duration() const noexcept
  {
    return ranges::fold_left(
        m_blocks_ | ranges::views::transform(&BlockLP::duration),
        0.0,
        std::plus<>());
  }

  /// @return Combined discount factor (annual and stage-specific)
  [[nodiscard]] static constexpr auto calculate_discount_factor(
      double annual_rate, double timeinit) noexcept
  {
    return annual_discount_factor(annual_rate, timeinit);
  }

  [[nodiscard]] constexpr auto discount_factor() const noexcept
  {
    return m_discount_factor_ * stage().discount_factor.value_or(1.0);
  }

  [[nodiscard]] constexpr auto annual_discount_rate() const noexcept
  {
    return m_discount_factor_;
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

  [[nodiscard]] constexpr auto phase_index() const noexcept
  {
    return m_phase_index_;
  }

  /// @return Span of blocks in this stage
  [[nodiscard]] constexpr const auto& blocks() const noexcept
  {
    return m_blocks_;
  }

  friend constexpr auto operator<=>(const StageLP&, const StageLP&) = default;

  [[nodiscard]] constexpr auto as_tuple() const noexcept
  {
    return std::tie(m_stage_, m_blocks_, m_timeinit_, m_duration_,
                   m_discount_factor_, m_index_, m_phase_index_);
  }

private:
  Stage m_stage_;  ///< Stage definition
  std::vector<BlockLP> m_blocks_;  ///< View of blocks in this m_stage_
  double m_timeinit_ {0.0};
  double m_duration_ {0.0};  ///< Total duration of the m_stage_
  double m_discount_factor_ {1.0};

  StageIndex m_index_ {unknown_index};
  PhaseIndex m_phase_index_ {unknown_index};
};

}  // namespace gtopt
