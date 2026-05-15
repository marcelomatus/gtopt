/**
 * @file      index_holder.hpp
 * @brief     Multi-dimensional index map type aliases for LP lookups
 * @date      Sun Mar 23 22:02:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides IndexHolder type aliases that map strongly-typed
 * index tuples to LP column/row indices using flat_map containers.
 */

#pragma once

#include <gtopt/fmap.hpp>
#include <gtopt/multi_array_2d.hpp>
#include <gtopt/scenario_lp.hpp>
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

/// `STBIndexHolder<X>::at({s,t})` throws on missing key.  LP-element
/// classes that follow the conditional-insert pattern (only insert
/// the outer (s, t) key when the inner per-block map is non-empty —
/// see `LineLP::add_to_lp`, `JunctionLP::add_to_lp`,
/// `WaterwayLP::add_to_lp`) need a graceful fallback on the read
/// side: callers must see an empty inner map instead of a thrown
/// exception when the level / stage / scenario in question didn't
/// populate the holder.
///
/// Returns a reference to a per-instantiation static empty
/// `BIndexHolder<Inner>` when the `(scenario, stage)` outer key is
/// missing.  One static instance per `Inner` type.
///
/// Generic over any STB-style holder (`flat_map<tuple<ScenarioUid,
/// StageUid>, BIndexHolder<Inner>>`); used today by `LineLP`'s
/// flow/loss/capacity accessors and by `JunctionLP::drain_cols_at` /
/// `WaterwayLP::flow_cols_at`.  Co-located here with the
/// `STBIndexHolder` aliases so all the LP element classes can share
/// one definition.
template<typename Holder>
[[nodiscard]] constexpr const auto& find_or_empty_inner(
    const Holder& holder,
    const ScenarioLP& scenario,
    const StageLP& stage) noexcept
{
  using Inner = typename Holder::mapped_type;
  static const Inner empty {};
  const auto it = holder.find({scenario.uid(), stage.uid()});
  return (it != holder.end()) ? it->second : empty;
}

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

using block_factor_matrix_t = gtopt::MultiArray2D<std::vector<double>>;
using stage_factor_matrix_t = std::vector<double>;
using scenario_stage_factor_matrix_t = gtopt::MultiArray2D<double>;

}  // namespace gtopt
