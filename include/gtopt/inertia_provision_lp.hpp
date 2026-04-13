/**
 * @file      inertia_provision_lp.hpp
 * @brief     LP formulation for inertia provisions
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the InertiaProvisionLP class, which builds LP
 * variables and constraints for generator inertia provision.
 *
 * For each block, creates r_inertia variable in [0, provision_max]:
 * - Coupling: p - r_inertia >= 0 (generator must produce at least r_inertia)
 * - Injects provision_factor * r_inertia into zone requirement row
 */

#pragma once

#include <gtopt/generator_lp.hpp>
#include <gtopt/inertia_provision.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{

class InertiaProvisionLP : public ObjectLP<InertiaProvision>
{
public:
  static constexpr LPClassName ClassName {"InertiaProvision"};
  static constexpr std::string_view ProvisionName {"provision"};
  static constexpr std::string_view CouplingName {"coupling"};
  /// PAMPL-visible alias for the provision columns.
  static constexpr std::string_view ProvName {"prov"};

  using Base = ObjectLP<InertiaProvision>;

  explicit InertiaProvisionLP(const InertiaProvision& inertia_provision,
                              const InputContext& ic);

  [[nodiscard]] constexpr auto&& inertia_provision(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {inertia_provision().generator};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Look up the provision column for (scenario, stage, block).
  [[nodiscard]] std::optional<ColIndex> lookup_provision_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = provision_cols_.find({scenario.uid(), stage.uid()});
    if (mit == provision_cols_.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

private:
  ElementIndex<GeneratorLP> generator_index_;
  std::vector<ElementIndex<InertiaZoneLP>> inertia_zone_indexes_;

  OptTBRealSched provision_max_;
  OptTRealSched provision_factor_;
  OptTRealSched cost_;

  STBIndexHolder<ColIndex> provision_cols_;
  STBIndexHolder<RowIndex> coupling_rows_;
};

}  // namespace gtopt
