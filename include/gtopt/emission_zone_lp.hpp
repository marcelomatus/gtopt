/**
 * @file      emission_zone_lp.hpp
 * @brief     LP-active wrapper for the EmissionZone bridge entity
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionZoneLP` owns the per-(scenario, stage, block)
 * `EmissionZone/production` column (the bridge variable, in tCO₂ /
 * block) plus the `EmissionZone/balance` row that pins
 *
 *   production_{z,b} − Σ_{s ∈ sources(z)} rate_s · gen_s,b · dur_b  =  0
 *
 * Optionally, when `EmissionZone.cap` is set, a per-stage cap row
 *
 *   Σ_b production_{z,b}  ≤  cap_z,s
 *
 * is built (hard if `cap_cost` unset; soft with a slack column
 * penalised at `cap_cost` otherwise).
 *
 * When `EmissionZone.price` is set, the production column carries an
 * objective coefficient `price · duration` so each tonne emitted in
 * block `b` costs `price · dur_b`.
 *
 * `EmissionSourceLP::add_to_lp` runs after this and injects the
 * generator-side coefficient `-rate · dur_b` on its generator's
 * generation column into the corresponding balance row — analogous
 * to `InertiaProvisionLP` injecting its provision factor into
 * `InertiaZoneLP::requirement_rows()`.
 *
 * ## Allowance-pool coupling (cap-and-trade with banking)
 *
 * When `EmissionZone.allowance_pool` is set, each per-block
 * `production` column is injected (coefficient `+1`, tCO₂) as a
 * drawdown into the referenced `AllowancePoolLP`'s energy-balance
 * rows, and the per-stage `cap` row is SKIPPED.  The pool's banked
 * SoC (`emin` / `emax` / `efin` / `efin_cost`) then mediates a
 * multi-stage cap with banking, replacing the fixed per-stage cap.
 * `AllowancePoolLP` is visited before `EmissionZoneLP` (see
 * `system_lp.hpp` `collections_t` ordering) so its energy rows exist
 * when this injection runs.
 */

#pragma once

#include <gtopt/emission_zone.hpp>
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

class EmissionZoneLP : public ObjectLP<EmissionZone>
{
public:
  using Base = ObjectLP<EmissionZone>;

  /// Column / row name constants (snake_case for output filenames).
  static constexpr std::string_view ProductionName {"production"};
  static constexpr std::string_view BalanceName {"balance"};
  static constexpr std::string_view CapName {"cap"};
  static constexpr std::string_view CapSlackName {"cap_slack"};

  explicit EmissionZoneLP(const EmissionZone& zone, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission_zone(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_cap(StageUid s) const { return cap_.at(s); }
  [[nodiscard]] auto param_cap_cost(StageUid s) const
  {
    return cap_cost_.at(s);
  }
  [[nodiscard]] auto param_price(StageUid s) const { return price_.at(s); }
  /// @}

  /// @name LP-row / col accessors — consumed by EmissionSourceLP at
  /// add_to_lp time to inject the `-rate · dur` coefficient into the
  /// matching balance row.
  /// @{
  [[nodiscard]] constexpr const auto& production_cols() const noexcept
  {
    return production_cols_;
  }
  [[nodiscard]] constexpr const auto& balance_rows() const noexcept
  {
    return balance_rows_;
  }
  /// @}

private:
  // Schedule parameter holders
  OptTRealSched cap_;
  OptTRealSched cap_cost_;
  OptTRealSched price_;

  // Per-(scenario, stage, block) indices populated by add_to_lp.
  STBIndexHolder<ColIndex> production_cols_;
  STBIndexHolder<RowIndex> balance_rows_;
  // Per-(scenario, stage) cap row + optional slack column.
  STIndexHolder<RowIndex> cap_rows_;
  STIndexHolder<ColIndex> cap_slack_cols_;
};

using EmissionZoneLPSId = ObjectSingleId<EmissionZoneLP>;

static_assert(EmissionZoneLP::Element::class_name
                  == LPClassName {"EmissionZone"},
              "EmissionZone::class_name must remain \"EmissionZone\"");

}  // namespace gtopt
