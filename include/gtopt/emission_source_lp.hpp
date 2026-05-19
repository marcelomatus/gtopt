/**
 * @file      emission_source_lp.hpp
 * @brief     Parameter-carrier wrapper for the EmissionSource bridge
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Passive in Commit 2 — resolves the per-stage `rate` schedule at
 * construction and exposes it via `param_rate(stage_uid)`.  In Commit
 * 3 this type becomes LP-active and contributes a coefficient
 * `rate · gen · dur` to its zone's balance row.
 */

#pragma once

#include <gtopt/emission_source.hpp>
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

class EmissionSourceLP : public ObjectLP<EmissionSource>
{
public:
  using Base = ObjectLP<EmissionSource>;

  explicit EmissionSourceLP(const EmissionSource& src, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission_source(this auto&& self) noexcept
  {
    return self.object();
  }

  /// Inject `-rate · dur_b` into the zone's balance row at every
  /// generator dispatch column for this (scenario, stage, block).
  /// EmissionZoneLP::add_to_lp must run BEFORE this — by the
  /// `lp_element_types_t` ordering it does (EmissionZoneLP precedes
  /// EmissionSourceLP).  Same dependency InertiaProvisionLP has on
  /// InertiaZoneLP.
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  /// No per-element output stream — the per-source contribution is
  /// captured by `EmissionZone/production_sol` (aggregate) and can be
  /// reconstructed post-solve as `rate · gen_sol · dur` when needed.
  [[nodiscard]] bool add_to_output(OutputContext& /*out*/) const noexcept
  {
    return true;
  }

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_rate(StageUid s) const { return rate_.at(s); }
  /// @}

private:
  OptTRealSched rate_;
};

using EmissionSourceLPSId = ObjectSingleId<EmissionSourceLP>;

static_assert(EmissionSourceLP::Element::class_name
                  == LPClassName {"EmissionSource"},
              "EmissionSource::class_name must remain \"EmissionSource\"");

}  // namespace gtopt
