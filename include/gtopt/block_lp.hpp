/**
 * @file      block_lp.hpp
 * @brief     Header of
 * @date      Wed May 28 22:19:22 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>

namespace gtopt
{

class BlockLP
{
public:
  BlockLP() = default;

  explicit BlockLP(Block pblock, BlockIndex index = BlockIndex {unknown_index})
      : m_block_(std::move(pblock))
      , m_index_(index)
  {
  }

  [[nodiscard]] constexpr auto uid() const { return BlockUid(m_block_.uid); }
  [[nodiscard]] constexpr auto duration() const { return m_block_.duration; }
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

private:
  Block m_block_;
  BlockIndex m_index_ {unknown_index};
};

}  // namespace gtopt
