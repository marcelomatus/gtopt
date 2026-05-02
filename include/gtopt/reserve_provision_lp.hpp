/**
 * @file      reserve_provision_lp.hpp
 * @brief     LP formulation for reserve provisions
 * @date      Mon Apr 21 22:23:43 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ReserveProvisionLP class, which builds LP variables
 * and constraints for generator reserve provision commitments.
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
  static constexpr std::string_view UprovisionName {"uprovision"};
  static constexpr std::string_view DprovisionName {"dprovision"};
  static constexpr std::string_view UcapacityName {"ucapacity"};
  static constexpr std::string_view DcapacityName {"dcapacity"};
  /// PAMPL-visible aliases for the up/down provision columns.
  /// Exposed under canonical short names so user constraints can write
  /// `reserve_provision("P").up` instead of the verbose `uprovision`.
  static constexpr std::string_view UpName {"up"};
  static constexpr std::string_view DnName {"dn"};

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

  /// Look up the up-provision column for (scenario, stage, block).
  /// Returns std::nullopt when the column has not been created yet
  /// (e.g. reserve is inactive or not configured for this period).
  [[nodiscard]] std::optional<ColIndex> lookup_up_provision_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = up.provision_cols.find({scenario.uid(), stage.uid()});
    if (mit == up.provision_cols.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Look up the down-provision column for (scenario, stage, block).
  /// Returns std::nullopt when the column has not been created yet.
  [[nodiscard]] std::optional<ColIndex> lookup_dn_provision_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = dp.provision_cols.find({scenario.uid(), stage.uid()});
    if (mit == dp.provision_cols.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Look up the up-provision row for (scenario, stage, block).
  [[nodiscard]] std::optional<RowIndex> lookup_up_provision_row(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = up.provision_rows.find({scenario.uid(), stage.uid()});
    if (mit == up.provision_rows.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Look up the down-provision row for (scenario, stage, block).
  [[nodiscard]] std::optional<RowIndex> lookup_dn_provision_row(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = dp.provision_rows.find({scenario.uid(), stage.uid()});
    if (mit == dp.provision_rows.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

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

// Pin the data-struct constant value so an accidental rename of the
// `ReserveProvision::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"ReserveProvision"`).
static_assert(ReserveProvisionLP::Element::class_name
                  == LPClassName {"ReserveProvision"},
              "ReserveProvision::class_name must remain \"ReserveProvision\"");

}  // namespace gtopt
