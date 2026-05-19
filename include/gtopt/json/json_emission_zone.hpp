/**
 * @file      json_emission_zone.hpp
 * @brief     JSON serialization for EmissionZone (+ legacy-singular fold)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Accepts two equivalent shapes for the pollutant list:
 *
 *   1. Canonical multi-pollutant form (PLEXOS / GHG-basket):
 *        "emissions": [
 *          {"emission": "co2", "weight": 1.0},
 *          {"emission": "ch4", "weight": 27.9}
 *        ]
 *
 *   2. Legacy single-pollutant shortcut:
 *        "emission": "co2"
 *      (auto-folded to a one-element `emissions` list with weight 1.0
 *      by `EmissionZoneConstructor` below — same migration idiom we
 *      used for `CapacityProfile`'s legacy keys.)
 */

#pragma once

#include <gtopt/emission_zone.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::EmissionZone;
using gtopt::EmissionZoneFactor;

// ── EmissionZoneFactor (one row of the `emissions[]` table) ─────────

template<>
struct json_data_contract<EmissionZoneFactor>
{
  using type = json_member_list<json_variant<"emission", SingleId>,
                                json_number_null<"weight", OptReal>>;

  constexpr static auto to_json_data(EmissionZoneFactor const& f)
  {
    return std::forward_as_tuple(f.emission, f.weight);
  }
};

// ── EmissionZone — supports both shapes via a custom constructor ────

/// Folds the legacy single `emission` key into a one-element
/// `emissions[]` list with `weight = 1.0`.  When `emissions` is
/// explicitly set in JSON the legacy key (if also present) is
/// IGNORED — the explicit list wins.
struct EmissionZoneConstructor
{
  [[nodiscard]] EmissionZone operator()(
      Uid uid,
      Name name,
      OptActive active,
      OptSingleId legacy_emission,
      gtopt::Array<EmissionZoneFactor> emissions,
      OptTRealFieldSched cap,
      OptTRealFieldSched cap_cost,
      OptTRealFieldSched price) const
  {
    EmissionZone z;
    z.uid = uid;
    z.name = std::move(name);
    z.active = std::move(active);
    if (!emissions.empty()) {
      z.emissions = std::move(emissions);
    } else if (legacy_emission.has_value()) {
      // Legacy `"emission": "co2"` → single-pollutant zone, weight 1.0.
      z.emissions = {{
          .emission = std::move(*legacy_emission),
          .weight = gtopt::OptReal {1.0},
      }};
    }
    z.cap = std::move(cap);
    z.cap_cost = std::move(cap_cost);
    z.price = std::move(price);
    return z;
  }
};

template<>
struct json_data_contract<EmissionZone>
{
  using constructor_t = EmissionZoneConstructor;

  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      // Legacy single-pollutant shortcut (folded at parse time).
      json_variant_null<"emission", OptSingleId, jvtl_SingleId>,
      // Canonical multi-pollutant list.
      json_array_null<"emissions",
                      gtopt::Array<EmissionZoneFactor>,
                      EmissionZoneFactor>,
      json_variant_null<"cap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"cap_cost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"price", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(EmissionZone const& z)
  {
    // Always emit the canonical `emissions[]`; never emit the legacy
    // singular `emission` key.
    return std::make_tuple(z.uid,
                           z.name,
                           z.active,
                           gtopt::OptSingleId {},
                           z.emissions,
                           z.cap,
                           z.cap_cost,
                           z.price);
  }
};

}  // namespace daw::json
