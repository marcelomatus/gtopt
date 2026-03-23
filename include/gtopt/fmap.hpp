/**
 * @file      fmap.hpp
 * @brief     flat_map type alias and map utility helpers
 * @date      Sun Mar 23 16:26:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the flat_map alias and two map utility helpers:
 *
 *   map_reserve(map, n)
 *       Reserve capacity for @p n elements.  No-op for std::map.
 *
 *   map_insert_sorted_unique(map, first, last)
 *       Insert from a pre-sorted, deduplicated range using the most
 *       efficient API for each backend.
 *
 * Backend selection (can be overridden by defining one of the macros before
 * including this header):
 *
 *   GTOPT_USE_STD_FLAT_MAP    - std::flat_map  (default for GCC >= 15 and
 *                                               Clang >= 23, only when
 *                                               <flat_map> is available)
 *   GTOPT_USE_BOOST_FLAT_MAP  - boost::container::flat_map (default for all
 *                                               other compilers / older
 *                                               versions)
 *   GTOPT_USE_STD_MAP         - std::map  (not used by default; available
 *                                          as an explicit override)
 */

#pragma once

// ---------------------------------------------------------------------------
// Automatic backend selection (only when no explicit choice was made)
// ---------------------------------------------------------------------------
#if !defined(GTOPT_USE_STD_MAP) && !defined(GTOPT_USE_STD_FLAT_MAP) \
    && !defined(GTOPT_USE_BOOST_FLAT_MAP)

#  if (defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 15 \
       && __has_include(<flat_map>))
// GCC 15+: std::flat_map is available (guarded by __has_include)
#    define GTOPT_USE_STD_FLAT_MAP

#  elif (defined(__clang__) && __clang_major__ >= 23 && __has_include(<flat_map>))
// Clang 23+: std::flat_map is available (guarded by __has_include)
#    define GTOPT_USE_STD_FLAT_MAP

#  elif __has_include(<boost/container/flat_map.hpp>)
// GCC < 15, Clang < 23, and all other compilers:
// use boost::container::flat_map.  The map_reserve() helper already
// guards against reserve(0) so the old Clang debug-assertion issue
// does not apply.
#    define GTOPT_USE_BOOST_FLAT_MAP

#  else
// No flat_map available: fall back to std::map.
#    define GTOPT_USE_STD_MAP

#  endif
#endif

// ---------------------------------------------------------------------------
// Backend headers
// ---------------------------------------------------------------------------

#ifdef GTOPT_USE_STD_MAP
#  include <map>
#elifdef GTOPT_USE_STD_FLAT_MAP
#  include <flat_map>
#else  // GTOPT_USE_BOOST_FLAT_MAP
#  include <boost/container/flat_map.hpp>
#endif

// ---------------------------------------------------------------------------
// gtopt namespace — type alias and helpers
// ---------------------------------------------------------------------------

namespace gtopt
{

// ── Type alias ───────────────────────────────────────────────────────────────

#ifdef GTOPT_USE_STD_MAP
template<typename key_type, typename value_type>
using flat_map = std::map<key_type, value_type>;
#elifdef GTOPT_USE_STD_FLAT_MAP
template<typename key_type, typename value_type>
using flat_map = std::flat_map<key_type, value_type>;
#else  // GTOPT_USE_BOOST_FLAT_MAP
template<typename key_type, typename value_type>
using flat_map = boost::container::flat_map<key_type, value_type>;
#endif

// ── map_reserve ──────────────────────────────────────────────────────────────

/// @brief Reserve capacity in a flat_map for @p n elements.
///
/// For `std::flat_map`, extracts the underlying key/value containers,
/// reserves capacity in both, then re-inserts them via `replace()`.
/// For `boost::container::flat_map`, delegates to `map.reserve(n)`.
/// For `std::map`, this is intentionally a no-op (std::map does not
/// support reserve()).
///
/// Calling with @p n == 0 is a no-op for all backends to avoid
/// unnecessary allocations.
template<typename Map, typename Size>
void map_reserve([[maybe_unused]] Map& map, [[maybe_unused]] Size n)
{
#ifdef GTOPT_USE_STD_MAP
  // std::map does not support reserve() — intentional no-op.
#elifdef GTOPT_USE_STD_FLAT_MAP
  if (n == 0) {
    return;
  }
  auto containers = std::move(map).extract();
  containers.keys.reserve(static_cast<size_t>(n));
  containers.values.reserve(static_cast<size_t>(n));
  map.replace(std::move(containers.keys),  // NOLINT
              std::move(containers.values));
#else  // GTOPT_USE_BOOST_FLAT_MAP
  if (n == 0) {
    return;
  }
  map.reserve(n);
#endif
}

// ── map_insert_sorted_unique ─────────────────────────────────────────────────

/// @brief Insert from a pre-sorted, deduplicated range [first, last) using
/// the most efficient API for each backend.
///
/// The range **must** be sorted in ascending key order and must contain no
/// duplicate keys.  For `std::flat_map` and `boost::container::flat_map`,
/// violating this precondition is undefined behaviour.  For `std::map` it
/// is defined (first-seen key wins) but the deduplication is still
/// recommended for consistency.
///
/// Backend behaviour:
///  - `std::flat_map`  : `insert(std::sorted_unique, first, last)` — O(n)
///    bulk insert that skips per-element comparisons.
///  - `boost::flat_map`: `insert(ordered_unique_range, first, last)` — same
///    semantics using the Boost equivalent tag.
///  - `std::map`       : `insert(first, last)` — O(n log n) sequential
///    insert; duplicate keys take the first-seen value.
template<typename Map, typename Iterator>
void map_insert_sorted_unique(Map& map, Iterator first, Iterator last)
{
#ifdef GTOPT_USE_STD_FLAT_MAP
  map.insert(std::sorted_unique, first, last);
#elifdef GTOPT_USE_BOOST_FLAT_MAP
  map.insert(boost::container::ordered_unique_range, first, last);
#else  // GTOPT_USE_STD_MAP
  map.insert(first, last);
#endif
}

}  // namespace gtopt
