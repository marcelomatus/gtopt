/**
 * @file      fuel_lp.hpp
 * @brief     LP wrapper for the Fuel data struct
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `FuelLP` resolves the time-schedulable fuel price + emission factors
 * that `GeneratorLP` consumes via `system_context.element<FuelLP>`.
 *
 * When `Fuel.max_offtake` is set the FuelLP also creates a per-(scenario,
 * stage) cap row enforcing
 *
 *   Σ_{g : Generator(g).fuel = this_fuel}
 *       (heat_rate_g(s, b) · generation_g(s, t, b) · duration_b)
 *     ≤  max_offtake(s)
 *
 * with an optional slack column priced at `max_offtake_cost` when the
 * cap is soft.  Mirrors PLEXOS's `FueMaxOffWeek_<fuel>` /
 * `FueMaxOffDay_<fuel>` Constraint pattern.
 */

#pragma once

#include <gtopt/fuel.hpp>
#include <gtopt/index_holder.hpp>
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

  /// LP row / col name constants (snake_case for output filenames).
  static constexpr std::string_view MaxOfftakeName {"max_offtake"};
  static constexpr std::string_view MaxOfftakeSlackName {"max_offtake_slack"};

  explicit FuelLP(const Fuel& fuel, const InputContext& ic);

  [[nodiscard]] constexpr auto&& fuel(this auto&& self) noexcept
  {
    return self.object();
  }

  /// LP-active hooks.  When `max_offtake` is unset on every stage these
  /// are effectively no-ops — FuelLP retains its passive-parameter
  /// behaviour for downstream consumers (`GeneratorLP`, `EmissionLP`).
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

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
  [[nodiscard]] auto param_max_offtake(StageUid s) const
  {
    return max_offtake_.at(s);
  }
  [[nodiscard]] auto param_max_offtake_cost(StageUid s) const
  {
    return max_offtake_cost_.at(s);
  }
  /// @}

private:
  OptTRealSched price_;
  OptTRealSched heat_content_;
  OptTRealSched combustion_ef_;
  OptTRealSched upstream_ef_;
  OptTRealSched max_offtake_;
  OptTRealSched max_offtake_cost_;

  /// Per-(scenario, stage) cap row + optional slack column.  Empty
  /// when `max_offtake` is unset for every (scenario, stage).
  STIndexHolder<RowIndex> max_offtake_rows_;
  STIndexHolder<ColIndex> max_offtake_slack_cols_;
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
