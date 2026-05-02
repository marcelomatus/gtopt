/**
 * @file      reserve_zone_lp.hpp
 * @brief     LP formulation for reserve zones
 * @date      Mon Apr 21 22:23:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ReserveZoneLP class, which builds LP variables
 * and constraints for operating reserve zone requirements.
 */

#pragma once

#include <gtopt/index_holder.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class ReserveZoneLP : public ObjectLP<ReserveZone>
{
public:
  static constexpr std::string_view UrequirementName {"urequirement"};
  static constexpr std::string_view DrequirementName {"drequirement"};
  /// PAMPL-visible aliases for the up/down requirement columns.
  /// Exposed under canonical short names so user constraints can write
  /// `reserve_zone("Z").up` instead of the verbose `urequirement`.
  static constexpr std::string_view UpName {"up"};
  static constexpr std::string_view DnName {"dn"};

  using Base = ObjectLP<ReserveZone>;

  explicit ReserveZoneLP(const ReserveZone& preserve_zone,
                         const InputContext& ic);

  [[nodiscard]] constexpr auto&& reserve_zone(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& urequirement_rows() const { return ur.requirement_rows; }
  [[nodiscard]] auto&& urequirement_cols() const { return ur.requirement_cols; }
  [[nodiscard]] auto&& drequirement_rows() const { return dr.requirement_rows; }
  [[nodiscard]] auto&& drequirement_cols() const { return dr.requirement_cols; }

  /// Look up the up-requirement column for (scenario, stage, block).
  /// Returns std::nullopt when no up-requirement variable exists for
  /// this period (e.g. not configured or block not active).
  [[nodiscard]] std::optional<ColIndex> lookup_urequirement_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = ur.requirement_cols.find({scenario.uid(), stage.uid()});
    if (mit == ur.requirement_cols.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Look up the down-requirement column for (scenario, stage, block).
  /// Returns std::nullopt when no down-requirement variable exists.
  [[nodiscard]] std::optional<ColIndex> lookup_drequirement_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = dr.requirement_cols.find({scenario.uid(), stage.uid()});
    if (mit == dr.requirement_cols.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

private:
  struct Requirement
  {
    Requirement(const InputContext& ic,
                std::string_view cname,
                const Id& id,
                auto&& rreq,
                auto&& rcost);

    OptTBRealSched req;
    OptTRealSched cost;
    STBIndexHolder<ColIndex> requirement_cols;
    STBIndexHolder<RowIndex> requirement_rows;
  } ur, dr;
};

// Pin the data-struct constant value so an accidental rename of the
// `ReserveZone::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"ReserveZone"`).
static_assert(ReserveZoneLP::Element::class_name == LPClassName {"ReserveZone"},
              "ReserveZone::class_name must remain \"ReserveZone\"");

}  // namespace gtopt
