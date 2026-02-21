/**
 * @file      fmap.hpp
 * @brief     Header of flat_map
 * @date      Sun Mar 23 16:26:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the flat_map alias and map_reserve helper.
 *
 * Backend selection (can be overridden by defining one of the macros before
 * including this header):
 *
 *   GTOPT_USE_STD_FLAT_MAP    – std::flat_map  (default for GCC >= 14 and
 *                                                Clang >= 23)
 *   GTOPT_USE_BOOST_FLAT_MAP  – boost::container::flat_map  (default for
 *                                                all other compilers)
 *   GTOPT_USE_STD_MAP         – std::map  (automatic for Clang < 23 to avoid
 *                                          the boost::flat_map reserve(0)
 *                                          debug assertion / SIGABRT)
 */

#pragma once

// ---------------------------------------------------------------------------
// Automatic backend selection (only when no explicit choice was made)
// ---------------------------------------------------------------------------
#if !defined(GTOPT_USE_STD_MAP) && !defined(GTOPT_USE_STD_FLAT_MAP) \
    && !defined(GTOPT_USE_BOOST_FLAT_MAP)

#  if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
// GCC 14+: std::flat_map is available
#    define GTOPT_USE_STD_FLAT_MAP

#  elif defined(__clang__) && __clang_major__ >= 23
// Clang 23+: std::flat_map is available
#    define GTOPT_USE_STD_FLAT_MAP

#  elif defined(__clang__)
// Clang < 23: std::flat_map is not available, and
// boost::container::flat_map triggers a debug assertion (m_ptr || !off) when
// reserve(0) is called, causing SIGABRT in debug builds.  Fall back to
// std::map which only requires operator< (available on all key types via
// strong::regular / operator<=>).
#    define GTOPT_USE_STD_MAP

#  else
// Other compilers (older GCC, MSVC, ...): use boost::container::flat_map
#    define GTOPT_USE_BOOST_FLAT_MAP

#  endif
#endif

// ---------------------------------------------------------------------------
// Backend implementations
// ---------------------------------------------------------------------------

#ifdef GTOPT_USE_STD_MAP
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

#elifdef GTOPT_USE_BOOST_FLAT_MAP
#  include <boost/container/flat_map.hpp>

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

#else  // GTOPT_USE_STD_FLAT_MAP

#  include <flat_map>

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
  auto containers = std::move(map).extract();
  containers.keys.reserve(static_cast<size_t>(n));
  containers.values.reserve(static_cast<size_t>(n));
  map.replace(std::move(containers.keys),  // NOLINT
              std::move(containers.values));
}

}  // namespace gtopt

#endif
