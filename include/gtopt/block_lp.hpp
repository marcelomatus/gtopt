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
 * @note Uses C++23 features including spaceship operator and explicit object parameters
 */

#pragma once

#include <gtopt/block.hpp>
#include <compare>
#include <string_view>

namespace gtopt
{

class BlockLP
{
public:
  constexpr static std::string_view ClassName = "BlockLP";

  BlockLP() = default;

  explicit constexpr BlockLP(
      Block pblock, BlockIndex index = BlockIndex{unknown_index}) noexcept
      : m_block_{std::move(pblock)}
      , m_index_{index}
  {}

  // Structured binding support
  template<std::size_t I>
  [[nodiscard]] constexpr auto get() const noexcept {
    if constexpr (I == 0) return uid();
    else if constexpr (I == 1) return duration();
    else if constexpr (I == 2) return index();
  }

  [[nodiscard]] constexpr auto uid() const noexcept -> BlockUid {
    return BlockUid{m_block_.uid};
  }

  [[nodiscard]] constexpr auto duration() const noexcept -> decltype(auto) {
    return (m_block_.duration);
  }

  [[nodiscard]] constexpr auto index() const noexcept -> BlockIndex {
    return m_index_;
  }

  [[nodiscard]] constexpr auto operator<=>(const BlockLP&) const noexcept = default;

private:
  Block m_block_;
  BlockIndex m_index_{unknown_index};
};

}  // namespace gtopt

// Structured binding support
namespace std
{
template<>
struct tuple_size<gtopt::BlockLP> : integral_constant<size_t, 3> {};

template<size_t I>
struct tuple_element<I, gtopt::BlockLP> {
  using type = decltype(declval<gtopt::BlockLP>().get<I>());
};
}
