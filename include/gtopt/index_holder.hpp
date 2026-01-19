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

template<typename FirstIndex, typename Value = Index>
using IndexHolder0 = gtopt::flat_map<FirstIndex, Value>;

// #define FESOP_USE_UNORDERED_MAP

template<typename key_type, typename value_type>
using index_map_t = gtopt::flat_map<key_type, value_type>;

template<typename key_type, typename value_type>
using tuple_map_t = gtopt::flat_map<key_type, value_type>;

template<typename FirstIndex, typename Value = Index>
using IndexHolder1 = index_map_t<FirstIndex, Value>;

template<typename FirstIndex, typename SecondIndex, typename Value = Index>
using IndexHolder2 = tuple_map_t<std::tuple<FirstIndex, SecondIndex>, Value>;

template<typename FirstIndex,
         typename SecondIndex,
         typename ThirdIndex,
         typename Value = Index>
using IndexHolder3 = tuple_map_t<std::tuple<FirstIndex, SecondIndex>,
                                 IndexHolder0<ThirdIndex, Value>>;

template<typename Value = Index>
using BIndexHolder = IndexHolder0<BlockUid, Value>;
template<typename Value = Index>
using TIndexHolder = IndexHolder1<StageUid, Value>;
template<typename Value = Index>
using STIndexHolder = IndexHolder2<ScenarioUid, StageUid, Value>;
template<typename Value = Index>
using STBIndexHolder = IndexHolder3<ScenarioUid, StageUid, BlockUid, Value>;

template<typename Value = Index>
using GSTIndexHolder = tuple_map_t<std::tuple<ScenarioUid, StageUid>, Value>;

template<typename Value = Index>
using GSTBIndexHolder =
    tuple_map_t<std::tuple<ScenarioUid, StageUid, BlockUid>, Value>;

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

  using Key = Map::key_type;
  return (empty_insert || !holder.empty())
      ? map.emplace(Key {scenario.uid(), stage.uid()},
                    std::forward<BHolder>(holder))
      : std::pair {map.end(), true};
}

template<typename Map, typename Value = Map::value_type>
constexpr auto emplace_value(const ScenarioLP& scenario,
                             const StageLP& stage,
                             Map& map,
                             Value&& value)
{
  using Key = Map::key_type;
  return map.emplace(Key {scenario.uid(), stage.uid()},
                     std::forward<Value>(value));
}

using block_factor_matrix_t = boost::multi_array<std::vector<double>, 2>;
using stage_factor_matrix_t = std::vector<double>;
using scenario_stage_factor_matrix_t = boost::multi_array<double, 2>;

}  // namespace gtopt
