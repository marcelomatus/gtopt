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
