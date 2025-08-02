/**
 * @file      reserve_zone_lp.hpp
 * @brief     Header of
 * @date      Mon Apr 21 22:23:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
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
  constexpr static std::string_view ClassName = "ReserveZone";

  using Base = ObjectLP<ReserveZone>;

  explicit ReserveZoneLP(ReserveZone preserve_zone, const InputContext& ic);

  template<typename Self>
  [[nodiscard]] constexpr auto&& reserve_zone(this Self& self)
  {
    return std::forward<Self>(self).object();
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

}  // namespace gtopt
