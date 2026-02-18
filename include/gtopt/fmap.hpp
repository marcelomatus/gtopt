/**
 * @file      fmap.hpp
 * @brief     Header of flat_map
 * @date      Sun Mar 23 16:26:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the flat_map
 */

#pragma once

#ifdef GTOPT_USE_UNORDERED_MAP
#  include <unordered_map>

#  include <boost/functional/hash.hpp>

namespace gtopt
{

struct tuple_hash
{
  template<typename Key>
  size_t operator()(const Key& key) const
  {
    return boost::hash_value(key);
  }
};

using hash_type = tuple_hash;
template<typename key_type, typename value_type>
using flat_map = std::unordered_map<key_type, value_type, hash_type>;

template<typename Map, typename Size>
void map_reserve(Map& map, Size n)
{
  map.reserve(n);
}

}  // namespace gtopt

#else

#  define GTOPT_USE_BOOST_FLAT_MAP

#  ifdef GTOPT_USE_BOOST_FLAT_MAP

#    include <boost/container/flat_map.hpp>

namespace gtopt
{

template<typename key_type, typename value_type>
using flat_map = boost::container::flat_map<key_type, value_type>;

template<typename Map, typename Size>
void map_reserve(Map& map, Size n)
{
  map.reserve(n);
}

}  // namespace gtopt

#  else

#    include <flat_map>
namespace gtopt
{

template<typename key_type, typename value_type>
using flat_map = std::flat_map<key_type, value_type>;

template<class Map, typename Size>
void map_reserve(Map& map, Size new_cap)
{
  auto containers = std::move(map).extract();
  containers.keys.reserve(static_cast<size_t>(new_cap));
  containers.values.reserve(static_cast<size_t>(new_cap));
  map.replace(std::move(containers));
}

}  // namespace gtopt

#  endif

#endif
