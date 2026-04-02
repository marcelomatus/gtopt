/**
 * @file      json_volume_right.hpp
 * @brief     JSON serialization for VolumeRight objects
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_right_bound_rule.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/volume_right.hpp>

namespace daw::json
{
using gtopt::MonthType;
using gtopt::VolumeRight;

/// Custom constructor: converts JSON reset_month string → MonthType enum.
struct VolumeRightConstructor
{
  [[nodiscard]] VolumeRight operator()(
      Uid uid,
      Name name,
      OptActive active,
      OptName purpose,
      OptSingleId reservoir,
      OptSingleId right_reservoir,
      OptInt direction,
      OptBool consumptive,
      OptTRealFieldSched emin,
      OptTRealFieldSched emax,
      OptTRealFieldSched ecost,
      OptReal eini,
      OptReal efin,
      OptTRealFieldSched soft_emin,
      OptTRealFieldSched soft_emin_cost,
      OptTRealFieldSched demand,
      OptTBRealFieldSched fmax,
      OptReal fail_cost,
      OptReal priority,
      OptReal flow_conversion_rate,
      OptReal energy_scale,
      OptName energy_scale_mode,
      OptBool use_state_variable,
      OptTRealFieldSched annual_loss,
      OptName reset_month_str,
      OptSingleId source_flow_right,
      std::optional<gtopt::RightBoundRule> bound_rule) const
  {
    VolumeRight vr;
    vr.uid = uid;
    vr.name = std::move(name);
    vr.active = std::move(active);
    vr.purpose = std::move(purpose);
    vr.reservoir = std::move(reservoir);
    vr.right_reservoir = std::move(right_reservoir);
    vr.direction = direction;
    vr.consumptive = consumptive;
    vr.emin = std::move(emin);
    vr.emax = std::move(emax);
    vr.ecost = std::move(ecost);
    vr.eini = eini;
    vr.efin = efin;
    vr.soft_emin = std::move(soft_emin);
    vr.soft_emin_cost = std::move(soft_emin_cost);
    vr.demand = std::move(demand);
    vr.fmax = std::move(fmax);
    vr.fail_cost = fail_cost;
    vr.priority = priority;
    vr.flow_conversion_rate = flow_conversion_rate;
    vr.energy_scale = energy_scale;
    vr.energy_scale_mode = std::move(energy_scale_mode);
    vr.use_state_variable = use_state_variable;
    vr.annual_loss = std::move(annual_loss);
    if (reset_month_str) {
      vr.reset_month = gtopt::enum_from_name<MonthType>(*reset_month_str);
    }
    vr.source_flow_right = std::move(source_flow_right);
    vr.bound_rule = std::move(bound_rule);
    return vr;
  }
};

template<>
struct json_data_contract<VolumeRight>
{
  using constructor_t = VolumeRightConstructor;

  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"purpose", OptName>,
      json_variant_null<"reservoir", OptSingleId, jvtl_SingleId>,
      json_variant_null<"right_reservoir", OptSingleId, jvtl_SingleId>,
      json_number_null<"direction", OptInt>,
      json_bool_null<"consumptive", OptBool>,
      json_variant_null<"emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"emax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"ecost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"eini", OptReal>,
      json_number_null<"efin", OptReal>,
      json_variant_null<"soft_emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"soft_emin_cost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"demand", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"fmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_number_null<"fail_cost", OptReal>,
      json_number_null<"priority", OptReal>,
      json_number_null<"flow_conversion_rate", OptReal>,
      json_number_null<"energy_scale", OptReal>,
      json_string_null<"energy_scale_mode", OptName>,
      json_bool_null<"use_state_variable", OptBool>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_string_null<"reset_month", OptName>,
      json_variant_null<"source_flow_right", OptSingleId, jvtl_SingleId>,
      json_class_null<"bound_rule", std::optional<RightBoundRule>>>;

  constexpr static auto to_json_data(VolumeRight const& vr)
  {
    return std::make_tuple(vr.uid,
                           vr.name,
                           vr.active,
                           vr.purpose,
                           vr.reservoir,
                           vr.right_reservoir,
                           vr.direction,
                           vr.consumptive,
                           vr.emin,
                           vr.emax,
                           vr.ecost,
                           vr.eini,
                           vr.efin,
                           vr.soft_emin,
                           vr.soft_emin_cost,
                           vr.demand,
                           vr.fmax,
                           vr.fail_cost,
                           vr.priority,
                           vr.flow_conversion_rate,
                           vr.energy_scale,
                           vr.energy_scale_mode,
                           vr.use_state_variable,
                           vr.annual_loss,
                           detail::enum_to_opt_name(vr.reset_month),
                           vr.source_flow_right,
                           vr.bound_rule);
  }
};
}  // namespace daw::json
