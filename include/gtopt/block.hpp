/**
 * @file      block.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:12:08 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <iterator>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Block
{
  Uid uid {};
  OptName name {};

  Real duration {};

  static constexpr std::string_view class_name = "block";
};

using BlockUid = StrongUidType<struct Block>;
using BlockIndex = StrongIndexType<Block>;

}  // namespace gtopt

namespace std
{

template<>
struct incrementable_traits<gtopt::BlockIndex>
{
  using difference_type = int;
};

}  // namespace std
