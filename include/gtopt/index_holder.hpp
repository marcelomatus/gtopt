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
using IndexHolder0 = StrongIndexVector<FirstIndex, size_t>;

// #define FESOP_USE_UNORDERED_MAP

template<typename key_type, typename value_type>
using index_map_t = gtopt::flat_map<key_type, value_type>;

template<typename key_type, typename value_type>
using tuple_map_t = gtopt::flat_map<key_type, value_type>;

template<typename FirstIndex>
using IndexHolder1 = index_map_t<FirstIndex, size_t>;

template<typename FirstIndex, typename SecondIndex>
using IndexHolder2 = tuple_map_t<std::tuple<FirstIndex, SecondIndex>, size_t>;

template<typename FirstIndex, typename SecondIndex, typename ThirdIndex>
using IndexHolder3 =
    tuple_map_t<std::tuple<FirstIndex, SecondIndex>, IndexHolder0<ThirdIndex>>;

using BIndexHolder = IndexHolder0<BlockIndex>;
using TIndexHolder = IndexHolder1<StageIndex>;
using STIndexHolder = IndexHolder2<ScenarioIndex, StageIndex>;
using STBIndexHolder = IndexHolder3<ScenarioIndex, StageIndex, BlockIndex>;

using GSTIndexHolder =
    tuple_map_t<std::tuple<ScenarioIndex, StageIndex>, size_t>;

using GSTBIndexHolder =
    tuple_map_t<std::tuple<ScenarioIndex, StageIndex, BlockIndex>, size_t>;

}  // namespace gtopt
