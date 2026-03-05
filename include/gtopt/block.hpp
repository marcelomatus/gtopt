/**
 * @file      block.hpp
 * @brief     Time block definition for power system optimization
 * @date      Wed Mar 26 12:12:08 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A Block is the smallest indivisible time unit in the planning horizon.
 * Energy quantities are obtained by multiplying power by duration:
 * `energy [MWh] = power [MW] × duration [h]`.
 *
 * ### JSON Example
 * ```json
 * {"uid": 1, "duration": 1.0}
 * ```
 */

#pragma once

#include <iterator>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @struct Block
 * @brief Smallest time unit in the optimization horizon
 *
 * Blocks are ordered within a @ref Stage.  A stage references its first
 * block by index (`first_block`) and contains `count_block` consecutive
 * blocks.  The `duration` field converts MW dispatch values into MWh energy
 * quantities in the objective function.
 *
 * @see Stage for the grouping of blocks into planning periods
 */
struct Block
{
  Uid uid {unknown_uid};   ///< Unique identifier
  OptName name {};         ///< Optional human-readable label

  Real duration {0.0};     ///< Duration of this time block [h]

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
