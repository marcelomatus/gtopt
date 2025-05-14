/**
 * @file      index_holder.hpp
 * @brief     Header of
 * @date      Sun Mar 23 22:02:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/strong_index_vector.hpp>

namespace gtopt
{
template<typename FirstIndex>
using IndexHolder0 = StrongIndexVector<FirstIndex, Index>;

// #define FESOP_USE_UNORDERED_MAP

template<typename key_type, typename value_type>
using index_map_t = gtopt::flat_map<key_type, value_type>;

template<typename key_type, typename value_type>
using tuple_map_t = gtopt::flat_map<key_type, value_type>;

template<typename FirstIndex>
using IndexHolder1 = index_map_t<FirstIndex, Index>;

template<typename FirstIndex, typename SecondIndex>
using IndexHolder2 = tuple_map_t<std::tuple<FirstIndex, SecondIndex>, Index>;

template<typename FirstIndex, typename SecondIndex, typename ThirdIndex>
using IndexHolder3 =
    tuple_map_t<std::tuple<FirstIndex, SecondIndex>, IndexHolder0<ThirdIndex>>;

using BIndexHolder = IndexHolder0<BlockIndex>;
using TIndexHolder = IndexHolder1<StageIndex>;
using STIndexHolder = IndexHolder2<ScenarioIndex, StageIndex>;
using STBIndexHolder = IndexHolder3<ScenarioIndex, StageIndex, BlockIndex>;

using GSTIndexHolder =
    tuple_map_t<std::tuple<ScenarioIndex, StageIndex>, Index>;

using GSTBIndexHolder =
    tuple_map_t<std::tuple<ScenarioIndex, StageIndex, BlockIndex>, Index>;

template<typename Map, typename BHolder>
constexpr auto emplace_bholder(const ScenarioIndex& scenario_index,
                               const StageIndex& stage_index,
                               Map& map,
                               BHolder&& holder,
                               bool empty_insert = false)
{
  // if empty_insert, holders that are empty will be inserted. Otherwise,
  // there will be skipped.

  // holder.shrink_to_fit();

  using Key = typename Map::key_type;
  return (empty_insert || !holder.empty())
      ? map.emplace(Key {scenario_index, stage_index},
                    std::forward<BHolder>(holder))
      : std::make_pair(map.end(), true);
}

template<typename Map, typename Value = typename Map::value_type>
constexpr auto emplace_value(const ScenarioIndex& scenario_index,
                             const StageIndex& stage_index,
                             Map& map,
                             Value&& value)
{
  using Key = typename Map::key_type;
  return map.emplace(Key {scenario_index, stage_index},
                     std::forward<Value>(value));
}

template<typename Map, typename Value = typename Map::value_type>
constexpr auto emplace_stage_value(const StageIndex& stage_index,
                                   Map& map,
                                   Value&& value)
{
  return map.emplace(stage_index, std::forward<Value>(value));
}

}  // namespace gtopt
