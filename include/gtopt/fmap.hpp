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

// Clang < 23 does not ship std::flat_map, and boost::container::flat_map
// triggers a debug-assertion (m_ptr || !off) in its vector iterator when
// reserve(0) is called, causing SIGABRT in debug builds.  Automatically
// fall back to std::map (which requires only operator<, available on all key
// types via strong::regular / operator<=>) unless an explicit backend was
// chosen.  std::unordered_map is not used here because boost::hash_value is
// not defined for strong::type<> index types nor for StateVariable::Key.
#if defined(__clang__) && __clang_major__ < 23
#  if !defined(GTOPT_USE_UNORDERED_MAP) && !defined(GTOPT_USE_STD_MAP) \
      && !defined(GTOPT_USE_STD_FLAT_MAP)
#    define GTOPT_USE_STD_MAP
#  endif
#endif

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
  if (n == 0) {
    return;
  }
  map.reserve(n);
}

}  // namespace gtopt

#elifdef GTOPT_USE_STD_MAP
#  include <map>

namespace gtopt
{

template<typename key_type, typename value_type>
using flat_map = std::map<key_type, value_type>;

template<typename Map, typename Size>
void map_reserve(Map& /*map*/, Size /*n*/)
{
  // std::map does not support reserve(); this is intentionally a no-op.
}

}  // namespace gtopt

#else

// Default to boost::container::flat_map; define GTOPT_USE_STD_FLAT_MAP
// before including this header to use std::flat_map instead.
#  ifndef GTOPT_USE_STD_FLAT_MAP
#    define GTOPT_USE_BOOST_FLAT_MAP
#  endif

#  ifdef GTOPT_USE_BOOST_FLAT_MAP

#    include <boost/container/flat_map.hpp>

namespace gtopt
{

template<typename key_type, typename value_type>
using flat_map = boost::container::flat_map<key_type, value_type>;

template<typename Map, typename Size>
void map_reserve(Map& map, Size n)
{
  if (n == 0) {
    return;
  }
  map.reserve(n);
}

}  // namespace gtopt

#  else

#    include <flat_map>

namespace gtopt
{

template<typename key_type, typename value_type>
using flat_map = std::flat_map<key_type, value_type>;

template<typename Map, typename Size>
void map_reserve(Map& map, Size n)
{
  if (n == 0) {
    return;
  }
  map.reserve(n);
}

}  // namespace gtopt

#  endif

#endif

#if __has_include(<flat_map>)

#  include <flat_map>

namespace gtopt
{

template<typename key_type, typename value_type, typename Size>
void map_reserve(std::flat_map<key_type, value_type>& map, Size new_cap)
{
  if (new_cap == 0) {
    return;
  }
  auto containers = std::move(map).extract();
  containers.keys.reserve(static_cast<size_t>(new_cap));
  containers.values.reserve(static_cast<size_t>(new_cap));

  map.replace(std::move(containers.keys),  // NOLINT
              std::move(containers.values));
}

}  // namespace gtopt

#endif  //
