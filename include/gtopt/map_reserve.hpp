/**
 * @file      map_reserve.hpp
 * @brief     Generic capacity-reservation helper for associative containers
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines a single free function `gtopt::map_reserve(map, n)` that reserves
 * capacity for @p n elements in any standard or boost associative container,
 * dispatched via C++26 requires-expressions on the *actual* argument type
 * rather than on the flat_map backend macros in `fmap.hpp`.
 *
 * Behaviour by container:
 *   - `boost::container::flat_map`, `std::unordered_map`, any vector-backed
 *     container exposing `.reserve(size_t)`  → calls `.reserve(n)`.
 *   - `std::flat_map` (which has no `.reserve()` of its own) → extracts the
 *     underlying key/value containers, reserves on each, then `replace()`s.
 *   - `std::map`, `std::set`, etc. (no reserve, no extract) → no-op.
 *
 * Calling with @p n == 0 is a no-op for every container, mirroring the
 * behaviour of the legacy helper that previously lived in `fmap.hpp`.
 *
 * This header intentionally pulls in nothing beyond `<utility>` so that TUs
 * which only use `std::unordered_map` (and never touch `flat_map`) do not
 * have to transitively #include `<flat_map>` or boost::container.
 */

#pragma once

#include <utility>

namespace gtopt
{

/// @brief Reserve capacity for @p n elements in any associative container.
///
/// Dispatch is by `if constexpr (requires …)` on the actual argument type,
/// so the same call site behaves correctly across `std::unordered_map`,
/// `boost::container::flat_map`, `std::flat_map`, and `std::map` — no matter
/// which flat_map backend the build is configured to use.
template<typename Map, typename Size>
constexpr void map_reserve([[maybe_unused]] Map& map, [[maybe_unused]] Size n)
{
  if (n == 0) {
    return;
  }

  if constexpr (requires { map.reserve(n); }) {
    // boost::container::flat_map, std::unordered_map, std::unordered_set,
    // and any other container that exposes a member `.reserve(size_t)`.
    map.reserve(n);
  } else if constexpr (requires { std::move(map).extract().keys; }) {
    // std::flat_map: no member reserve(), but its `extract()`/`replace()`
    // pair lets us reserve on the underlying key and value containers.
    auto containers = std::move(map).extract();
    containers.keys.reserve(static_cast<std::size_t>(n));
    containers.values.reserve(static_cast<std::size_t>(n));
    map.replace(std::move(containers.keys),  // NOLINT
                std::move(containers.values));
  }
  // else: std::map / std::set / std::multimap — no reserve, intentional no-op.
}

}  // namespace gtopt
