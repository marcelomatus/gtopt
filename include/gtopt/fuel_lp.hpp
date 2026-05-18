/**
 * @file      fuel_lp.hpp
 * @brief     Parameter-carrier wrapper for the Fuel data struct
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `FuelLP` does NOT contribute any LP variables or rows on its own — it
 * is a passive resolver of the time-schedulable fuel price + emission
 * factors that `GeneratorLP` consumes via `system_context.element<FuelLP>`.
 * The two `AddToLP` hooks (`add_to_lp` / `add_to_output`) are no-ops.
 */

#pragma once

#include <gtopt/fuel.hpp>
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

class FuelLP : public ObjectLP<Fuel>
{
public:
  using Base = ObjectLP<Fuel>;

  explicit FuelLP(const Fuel& fuel, const InputContext& ic);

  [[nodiscard]] constexpr auto&& fuel(this auto&& self) noexcept
  {
    return self.object();
  }

  /// No-op: Fuel carries parameters only.
  [[nodiscard]] bool add_to_lp(const SystemContext& /*sc*/,
                               const ScenarioLP& /*scenario*/,
                               const StageLP& /*stage*/,
                               LinearProblem& /*lp*/) noexcept
  {
    return true;
  }

  /// No-op: Fuel emits no per-(scene, stage) parquet outputs.
  [[nodiscard]] bool add_to_output(OutputContext& /*out*/) const noexcept
  {
    return true;
  }

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_price(StageUid s) const { return price_.at(s); }
  [[nodiscard]] auto param_heat_content(StageUid s) const
  {
    return heat_content_.at(s);
  }
  [[nodiscard]] auto param_combustion_emission_factor(StageUid s) const
  {
    return combustion_ef_.at(s);
  }
  [[nodiscard]] auto param_upstream_emission_factor(StageUid s) const
  {
    return upstream_ef_.at(s);
  }
  /// @}

private:
  OptTRealSched price_;
  OptTRealSched heat_content_;
  OptTRealSched combustion_ef_;
  OptTRealSched upstream_ef_;
};

/// SingleId-style reference into `Collection<FuelLP>`.  Used by
/// downstream consumers (e.g. `Generator.fuel`, `Commitment.fuel`)
/// that resolve the Fuel at LP-build time via
/// `sc.element<FuelLP>(FuelLPSId{...})`.
using FuelLPSId = ObjectSingleId<FuelLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Fuel::class_name` literal fails the build (LP row labels and CSV
// outputs depend on the exact string `"Fuel"`).
static_assert(FuelLP::Element::class_name == LPClassName {"Fuel"},
              "Fuel::class_name must remain \"Fuel\"");

}  // namespace gtopt
