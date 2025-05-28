/**
 * @file      reserve_provision_lp.hpp
 * @brief     Header of
 * @date      Mon Apr 21 22:23:43 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/generator_lp.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{

class ReserveProvisionLP : public ObjectLP<ReserveProvision>
{
public:
  constexpr static std::string_view ClassName = "ReserveProvision";

  using Base = ObjectLP<ReserveProvision>;

  explicit ReserveProvisionLP(const InputContext& ic,
                              ReserveProvision preserve_provision);

  [[nodiscard]] constexpr auto&& reserve_provision() { return object(); }
  [[nodiscard]] constexpr auto&& reserve_provision() const { return object(); }

  [[nodiscard]] auto generator() const
  {
    return GeneratorLPSId {reserve_provision().generator};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  struct Provision
  {
    Provision(const InputContext& ic,
              std::string_view cname,
              const Id& id,
              auto&& rmax,
              auto&& rcost,
              auto&& rcapf,
              auto&& rprof);

    OptTBRealSched max;
    OptTRealSched cost;
    OptTRealSched capacity_factor;
    OptTRealSched provision_factor;
    GSTBIndexHolder provision_cols;
    GSTBIndexHolder provision_rows;
    GSTBIndexHolder capacity_rows;
  } up, dp;

  ElementIndex<GeneratorLP> generator_index;
  std::vector<ElementIndex<ReserveZoneLP>> reserve_zone_indexes;
};

}  // namespace gtopt
