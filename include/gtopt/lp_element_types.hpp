/**
 * @file      lp_element_types.hpp
 * @brief     Authoritative compile-time registry of all LP element types
 * @date      Mon Mar  9 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the canonical ordered list of all LP component types as a
 * `std::tuple` alias (`lp_element_types_t`), forward declarations for each
 * type, and compile-time index utilities (`lp_type_index_v<T>`).
 *
 * This header is intentionally lightweight — it contains only forward
 * declarations and type-level metaprogramming, so it can be included by
 * `system_context.hpp` without pulling in `system_lp.hpp`.
 */

#pragma once

#include <array>
#include <cstddef>
#include <ranges>
#include <tuple>
#include <type_traits>

namespace gtopt
{

// ── Forward declarations for all LP element types
// ─────────────────────────────
class BatteryLP;
class BusLP;
class CommitmentLP;
class ConverterLP;
class DemandLP;
class DemandProfileLP;
class ReservoirDischargeLimitLP;
class ReservoirSeepageLP;
class FlowLP;
class GeneratorLP;
class GeneratorProfileLP;
class FlowRightLP;
class VolumeRightLP;
class JunctionLP;
class LineLP;
class ReserveProvisionLP;
class ReserveZoneLP;
class ReservoirProductionFactorLP;
class ReservoirLP;
class TurbineLP;
class UserConstraintLP;
class WaterwayLP;

// ── Authoritative ordered type list
// ───────────────────────────────────────────
/**
 * @brief Ordered tuple of all LP element types.
 *
 * The position of each type in this tuple determines its index in the
 * `m_collection_ptrs_` array inside `SystemContext` (see `lp_type_index_v`).
 * The order matches `SystemLP::collections_t` for readability, but correctness
 * does not depend on that — the indices are always looked up via
 * `lp_type_index_v<T>`, never assumed to match positionally.
 *
 * `UserConstraintLP` is always the LAST type so that user constraints are
 * added to the LP after all other elements (user constraints reference
 * columns created by the earlier elements).
 */
using lp_element_types_t = std::tuple<BusLP,
                                      DemandLP,
                                      GeneratorLP,
                                      LineLP,
                                      GeneratorProfileLP,
                                      DemandProfileLP,
                                      BatteryLP,
                                      ConverterLP,
                                      ReserveZoneLP,
                                      ReserveProvisionLP,
                                      CommitmentLP,
                                      JunctionLP,
                                      WaterwayLP,
                                      FlowLP,
                                      ReservoirLP,
                                      ReservoirSeepageLP,
                                      ReservoirDischargeLimitLP,
                                      TurbineLP,
                                      ReservoirProductionFactorLP,
                                      FlowRightLP,
                                      VolumeRightLP,
                                      UserConstraintLP>;

/// Total number of LP element types.
inline constexpr std::size_t lp_type_count_v =
    std::tuple_size_v<lp_element_types_t>;

// ── Compile-time index of T in lp_element_types_t
// ─────────────────────────────
namespace detail
{

/// Returns the 0-based position of T in the pack Ts..., or sizeof...(Ts) if
/// not found.  Implemented as a consteval function (C++20) with a local
/// bool array to avoid recursive template instantiation depth.
template<typename T, typename... Ts>
[[nodiscard]] consteval std::size_t type_index_in() noexcept
{
  const std::array<bool, sizeof...(Ts)> same = {
      std::is_same_v<T, Ts>...,
  };
  for (auto [i, v] : std::views::enumerate(same)) {
    if (v) {
      return i;
    }
  }
  return sizeof...(Ts);  // not found
}

template<typename T, typename Tuple>
struct lp_type_index_impl;

template<typename T, typename... Ts>
struct lp_type_index_impl<T, std::tuple<Ts...>>
    : std::integral_constant<std::size_t, type_index_in<T, Ts...>()>
{
};

}  // namespace detail

/**
 * @brief Compile-time index of LP element type `T` in `lp_element_types_t`.
 *
 * Used by `SystemContext::get_element<T>` to locate the `Collection<T>`
 * pointer in the `m_collection_ptrs_` array without including
 * `system_lp.hpp`.
 *
 * Example: `lp_type_index_v<BatteryLP>` evaluates to 6 (0-based).
 */
template<typename T>
inline constexpr std::size_t lp_type_index_v =
    detail::lp_type_index_impl<T, lp_element_types_t>::value;

// ── Shared void-pointer array type
// ────────────────────────────────────────────
/**
 * @brief Array of `const void*` pointers, one slot per LP element type.
 *
 * Each slot stores a pointer to the corresponding `Collection<T>` inside a
 * `SystemLP`.  The slot for type `T` is at index `lp_type_index_v<T>`.
 * Populated in `SystemContext::SystemContext()` via `std::apply`.
 */
using lp_collection_ptrs_t = std::array<const void*, lp_type_count_v>;

}  // namespace gtopt
