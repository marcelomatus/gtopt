/**
 * @file      emission_lp.hpp
 * @brief     Parameter-carrier wrapper for the Emission data struct
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionLP` is the LP-side wrapper for an `Emission` pollutant.  In
 * Commit 1 it is **purely passive** — it resolves the price / cap /
 * cap_cost schedules at construction time and exposes `param_*`
 * accessors, but contributes no LP variables or rows.  The `AddToLP`
 * hooks (`add_to_lp`, `add_to_output`) are inline no-ops.
 *
 * Wiring of the cap row + cost coefficient on generator dispatch
 * columns is deferred to Commit 4 (after `Fuel.emission_factors[]`
 * lands in Commit 2 and the per-pollutant output paths land in
 * Commit 3).
 *
 * @see Emission for the data struct.
 * @see Fuel.emission_factors (planned) for the per-fuel mapping.
 */

#pragma once

#include <gtopt/emission.hpp>
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

class EmissionLP : public ObjectLP<Emission>
{
public:
  using Base = ObjectLP<Emission>;

  explicit EmissionLP(const Emission& emission, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission(this auto&& self) noexcept
  {
    return self.object();
  }

  // Intentionally no `add_to_lp` / `add_to_output` in Commit 1.
  // `Emission` is currently a passive parameter carrier — it does not
  // yet contribute LP rows or columns.  Promotion to LP-active bridge
  // element (owning the `Emission/production` column + `Emission/balance`
  // row + optional `Emission/cap` row) lands in Commit 3 of the
  // emissions ladder.  Until then the visitor in `system_lp.cpp` gates
  // on `AddToLP<T>` and skips this type.

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_price(StageUid s) const { return price_.at(s); }
  [[nodiscard]] auto param_cap(StageUid s) const { return cap_.at(s); }
  [[nodiscard]] auto param_cap_cost(StageUid s) const
  {
    return cap_cost_.at(s);
  }
  /// @}

private:
  OptTRealSched price_;
  OptTRealSched cap_;
  OptTRealSched cap_cost_;
};

/// SingleId-style reference into `Collection<EmissionLP>`.  Used by
/// downstream consumers (planned `Fuel.emission_factors[].emission`
/// and `Generator.emission_captures[].emission`) that resolve the
/// Emission at LP-build time via
/// `sc.element<EmissionLP>(EmissionLPSId{...})`.
using EmissionLPSId = ObjectSingleId<EmissionLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Emission::class_name` literal fails the build (LP row labels and
// output directory names depend on the exact string `"Emission"`).
static_assert(EmissionLP::Element::class_name == LPClassName {"Emission"},
              "Emission::class_name must remain \"Emission\"");

}  // namespace gtopt
