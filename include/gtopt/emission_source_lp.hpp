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

  // Intentionally no `add_to_lp` / `add_to_output` in Commit 2.
  // Promoted to LP-active in Commit 3 — adds a coefficient to its
  // zone's balance row.

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
