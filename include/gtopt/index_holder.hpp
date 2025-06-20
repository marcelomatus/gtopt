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

#include <boost/multi_array.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/strong_index_vector.hpp>

namespace gtopt
{
template<typename FirstIndex>
using IndexHolder0 = gtopt::flat_map<FirstIndex, Index>;

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

using BIndexHolder = IndexHolder0<BlockUid>;
using TIndexHolder = IndexHolder1<StageUid>;
using STIndexHolder = IndexHolder2<ScenarioUid, StageUid>;
using STBIndexHolder = IndexHolder3<ScenarioUid, StageUid, BlockUid>;

using BIndexUHolder = IndexHolder0<BlockUid>;
using TIndexUHolder = IndexHolder1<StageUid>;
using STIndexUHolder = IndexHolder2<ScenarioUid, StageUid>;
using STBIndexUHolder = IndexHolder3<ScenarioUid, StageUid, BlockUid>;

using GSTIndexHolder = tuple_map_t<std::tuple<ScenarioUid, StageUid>, Index>;

using GSTBIndexHolder =
    tuple_map_t<std::tuple<ScenarioUid, StageUid, BlockUid>, Index>;

template<typename Map, typename BHolder>
constexpr auto emplace_bholder(const ScenarioLP& scenario,
                               const StageLP& stage,
                               Map& map,
                               BHolder&& holder,
                               bool empty_insert = false)
{
  // if empty_insert, holders that are empty will be inserted. Otherwise,
  // there will be skipped.

  // holder.shrink_to_fit();

  using Key = typename Map::key_type;
  return (empty_insert || !holder.empty())
      ? map.emplace(Key {scenario.uid(), stage.uid()},
                    std::forward<BHolder>(holder))
      : std::pair {map.end(), true};
}

template<typename Map, typename Value = typename Map::value_type>
constexpr auto emplace_value(const ScenarioLP& scenario,
                             const StageLP& stage,
                             Map& map,
                             Value&& value)
{
  using Key = typename Map::key_type;
  return map.emplace(Key {scenario.uid(), stage.uid()},
                     std::forward<Value>(value));
}

template<typename Map, typename Value = typename Map::value_type>
constexpr auto emplace_stage_value(const StageUid& stage_uid,
                                   Map& map,
                                   Value&& value)
{
  return map.emplace(stage_uid, std::forward<Value>(value));
}

using block_factor_matrix_t = boost::multi_array<std::vector<double>, 2>;
using stage_factor_matrix_t = std::vector<double>;
using scenario_stage_factor_matrix_t = boost::multi_array<double, 2>;

}  // namespace gtopt
