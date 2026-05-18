/**
 * @file      emission_zone_lp.hpp
 * @brief     Parameter-carrier wrapper for the EmissionZone struct
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionZoneLP` is currently **passive** (Commit 2 of the emissions
 * ladder).  It resolves the per-stage schedules (`cap`, `cap_cost`,
 * `price`) at construction time and exposes them via `param_*`
 * accessors.  In Commit 3 it is promoted to LP-active and gains the
 * `add_to_lp` method that owns the `EmissionZone/production` column +
 * `EmissionZone/balance` row + (optional) `EmissionZone/cap` row +
 * (optional) price coefficient.
 */

#pragma once

#include <gtopt/emission_zone.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

// Forward declarations
class InputContext;
class LinearProblem;
class OutputContext;
class SystemContext;

class EmissionZoneLP : public ObjectLP<EmissionZone>
{
public:
  using Base = ObjectLP<EmissionZone>;

  explicit EmissionZoneLP(const EmissionZone& zone, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission_zone(this auto&& self) noexcept
  {
    return self.object();
  }

  // Intentionally no `add_to_lp` / `add_to_output` in Commit 2.
  // Passive parameter carrier — promoted to LP-active in Commit 3
  // (the bridge wiring that owns `production` col + `balance` row +
  // optional `cap` row).  The visitor in `system_lp.cpp` gates on
  // `AddToLP<T>` and skips this type.

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_cap(StageUid s) const { return cap_.at(s); }
  [[nodiscard]] auto param_cap_cost(StageUid s) const
  {
    return cap_cost_.at(s);
  }
  [[nodiscard]] auto param_price(StageUid s) const { return price_.at(s); }
  /// @}

private:
  OptTRealSched cap_;
  OptTRealSched cap_cost_;
  OptTRealSched price_;
};

/// SingleId-style reference into `Collection<EmissionZoneLP>`.  Used by
/// `EmissionSource.zone` and (in Commit 3) by the AMPL resolver to
/// look up zones by uid/name.
using EmissionZoneLPSId = ObjectSingleId<EmissionZoneLP>;

// Pin the class-name literal.
static_assert(EmissionZoneLP::Element::class_name
                  == LPClassName {"EmissionZone"},
              "EmissionZone::class_name must remain \"EmissionZone\"");

}  // namespace gtopt
