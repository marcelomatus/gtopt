/**
 * @file      demand_profile_lp.hpp
 * @brief     LP formulation for demand profiles
 * @date      Tue Apr  1 23:54:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the DemandProfileLP class, which builds LP variables
 * and constraints for time-varying demand profiles linked to demands.
 */

#pragma once

#include <gtopt/demand_lp.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/profile_object_lp.hpp>

namespace gtopt
{
class DemandProfileLP : public ProfileObjectLP<DemandProfile, DemandLP>
{
public:
  static constexpr std::string_view UnservedName {"unserved"};

  explicit DemandProfileLP(const DemandProfile& pdemand_profile,
                           InputContext& ic);

  [[nodiscard]] constexpr auto&& demand_profile(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto demand_sid() const noexcept
  {
    return DemandLPSId {demand_profile().demand};
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

// Pin the data-struct constant value so an accidental rename of the
// `DemandProfile::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"DemandProfile"`).
static_assert(DemandProfileLP::Element::class_name
                  == LPClassName {"DemandProfile"},
              "DemandProfile::class_name must remain \"DemandProfile\"");

}  // namespace gtopt
