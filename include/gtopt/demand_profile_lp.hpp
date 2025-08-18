/**
 * @file      demand_profile_lp.hpp
 * @brief     Header of
 * @date      Tue Apr  1 23:54:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
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
  static constexpr LPClassName ClassName {"DemandProfile", "dpr"};

  explicit DemandProfileLP(DemandProfile pdemand_profile, InputContext& ic);

  [[nodiscard]] constexpr auto&& demand_profile(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.object());
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

protected:
  using ProfileObjectLP<DemandProfile, DemandLP>::ProfileObjectLP;
};

}  // namespace gtopt
