/**
 * @file      capacity_profile_lp.hpp
 * @brief     Unified LP wrapper for `CapacityProfile` (kind-dispatched)
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `CapacityProfileLP` is the kind-dispatched LP wrapper around the unified
 * `CapacityProfile` element.  At `add_to_lp` time it inspects
 * `CapacityProfile::owner_kind` and routes through the matching
 * `CapacityProfileTraits<OwnerLP>` specialization to:
 *   - locate the owner LP via `sc.element<OwnerLP>(...)`,
 *   - read its per-block column indices,
 *   - call the shared `ProfileObjectLP::add_profile_to_lp` helper.
 *
 * LP row labels and CSV output use the per-kind
 * `output_class_name` ("GeneratorProfile" / "DemandProfile"), preserving
 * the existing CSV schema while the data-struct class is unified.
 *
 * The legacy `DemandProfileLP` / `GeneratorProfileLP` classes still
 * exist in parallel during this transition (Commit 1).  Removal happens
 * in Commit 3 once the unified collection has been adopted by `System`.
 */

#pragma once

#include <gtopt/capacity_profile.hpp>
#include <gtopt/profile_object_lp.hpp>

namespace gtopt
{

/// LP wrapper for the unified `CapacityProfile` element.
///
/// Inherits from `ProfileObjectLP<CapacityProfile, GeneratorLP>`; the second
/// template argument is unused by the base — the owner LP type is
/// resolved at runtime via `dispatch_profile_owner` on
/// `CapacityProfile::owner_kind`.
class CapacityProfileLP
    : public ProfileObjectLP<CapacityProfile, /*unused*/ CapacityProfile>
{
public:
  explicit CapacityProfileLP(const CapacityProfile& pprofile, InputContext& ic);

  [[nodiscard]] constexpr auto&& profile_element(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Update profile constraints for an aperture scenario.
  [[nodiscard]] bool update_aperture(
      LinearInterface& li,
      const ScenarioLP& base_scenario,
      const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
      const StageLP& stage) const;
};

// Pin the unified class-name literal so an accidental rename fails the
// build.
static_assert(CapacityProfileLP::Element::class_name
                  == LPClassName {"CapacityProfile"},
              "CapacityProfile::class_name must remain \"CapacityProfile\"");

}  // namespace gtopt
