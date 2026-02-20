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
  static constexpr LPClassName ClassName {"ReserveProvision", "rpr"};

  using Base = ObjectLP<ReserveProvision>;

  explicit ReserveProvisionLP(const ReserveProvision& preserve_provision,
                              const InputContext& ic);

  [[nodiscard]] constexpr auto&& reserve_provision(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return self.object();
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {reserve_provision().generator};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

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
    STBIndexHolder<ColIndex> provision_cols;
    STBIndexHolder<RowIndex> provision_rows;
    STBIndexHolder<RowIndex> capacity_rows;
  };

private:
  Provision up;
  Provision dp;
  ElementIndex<GeneratorLP> generator_index;
  std::vector<ElementIndex<ReserveZoneLP>> reserve_zone_indexes;
};

}  // namespace gtopt
