#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>

namespace gtopt
{

class BlockLP
{
public:
  BlockLP() = default;

  explicit BlockLP(Block pblock)
      : block(std::move(pblock))
  {
  }

  // constexpr static auto is_active() { return true; }
  [[nodiscard]] constexpr auto uid() const { return BlockUid(block.uid); }
  [[nodiscard]] constexpr auto duration() const { return block.duration; }

private:
  Block block;
};

}  // namespace gtopt
