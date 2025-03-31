/**
 * @file      stage_lp.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:10:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <functional>
#include <numeric>
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

class StageLP
{
public:
  using BlockSpan = std::span<const BlockLP>;
  using BlockIndexes = std::vector<BlockIndex>;
  using BlockIndexSpan = std::span<const BlockIndex>;

  StageLP() = default;

  template<class Blocks>
  explicit StageLP(Stage pstage,
                   const Blocks& pblocks,
                   double annual_discount_rate = 0.0)
      : stage(std::move(pstage))
      , block_span(
            std::span(pblocks).subspan(stage.first_block, stage.count_block))
      , block_indexes(stage.count_block)
      , span_duration(std::transform_reduce(block_span.begin(),
                                            block_span.end(),
                                            0,
                                            std::plus(),
                                            [](auto&& b)
                                            { return b.duration(); }))
      , annual_discount_factor(std::pow(1.0 / (1.0 + annual_discount_rate),
                                        span_duration / avg_year_hours))
  {
    std::iota(  // NOLINT
        block_indexes.begin(),
        block_indexes.end(),
        BlockIndex {});
  }

  [[nodiscard]] constexpr auto duration() const { return span_duration; }
  [[nodiscard]] constexpr auto discount_factor() const
  {
    return annual_discount_factor * stage.discount_factor.value_or(1.0);
  }
  [[nodiscard]] constexpr auto is_active() const
  {
    return stage.active.value_or(true);
  }
  [[nodiscard]] constexpr auto uid() const { return StageUid {stage.uid}; }
  [[nodiscard]] constexpr auto&& blocks() const { return block_span; }
  [[nodiscard]] constexpr auto indexes() const
  {
    return BlockIndexSpan {block_indexes};
  }

private:
  Stage stage;
  BlockSpan block_span;
  BlockIndexes block_indexes;

  double span_duration {0};
  double annual_discount_factor {1};
};

}  // namespace gtopt
