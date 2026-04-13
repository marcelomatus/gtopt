/**
 * @file      inertia_zone_lp.hpp
 * @brief     LP formulation for inertia zones
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the InertiaZoneLP class, which builds LP variables
 * and constraints for system inertia zone requirements.  Mirrors the
 * ReserveZoneLP pattern with a single requirement direction.
 */

#pragma once

#include <gtopt/index_holder.hpp>
#include <gtopt/inertia_zone.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class InertiaZoneLP : public ObjectLP<InertiaZone>
{
public:
  static constexpr LPClassName ClassName {"InertiaZone"};
  static constexpr std::string_view RequirementName {"requirement"};
  /// PAMPL-visible alias for the requirement columns.
  static constexpr std::string_view ReqName {"req"};

  using Base = ObjectLP<InertiaZone>;

  explicit InertiaZoneLP(const InertiaZone& inertia_zone,
                         const InputContext& ic);

  [[nodiscard]] constexpr auto&& inertia_zone(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& requirement_rows() const { return rr.requirement_rows; }
  [[nodiscard]] auto&& requirement_cols() const { return rr.requirement_cols; }

  /// Look up the requirement column for (scenario, stage, block).
  [[nodiscard]] std::optional<ColIndex> lookup_requirement_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = rr.requirement_cols.find({scenario.uid(), stage.uid()});
    if (mit == rr.requirement_cols.end()) {
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
  } rr;
};

}  // namespace gtopt
