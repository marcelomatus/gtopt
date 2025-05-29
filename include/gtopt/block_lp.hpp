/**
 * @file      block_lp.hpp
 * @brief     Linear Programming representation of a Block for optimization
 * problems
 * @date      Wed May 28 22:19:22 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * The BlockLP class provides a linear programming (LP) compatible
 * representation of a Block, which is a fundamental building block for
 * optimization problems. It maintains the block's unique identifier, duration,
 * and index while providing constexpr and noexcept guarantees for efficient use
 * in optimization contexts.
 *
 */

#pragma once

#include <gtopt/block.hpp>

namespace gtopt
{

class BlockLP
{
public:
  BlockLP() = default;

  explicit constexpr BlockLP(
      Block pblock, BlockIndex index = BlockIndex {unknown_index}) noexcept
      : m_block_ {std::move(pblock)}
      , m_index_ {index}
  {
  }

  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return BlockUid {m_block_.uid};
  }
  [[nodiscard]] constexpr auto duration() const noexcept
  {
    return m_block_.duration;
  }
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

private:
  Block m_block_;
  BlockIndex m_index_ {unknown_index};
};

}  // namespace gtopt
