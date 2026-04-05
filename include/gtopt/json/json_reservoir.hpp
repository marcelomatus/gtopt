#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_reservoir_discharge_limit.hpp>
#include <gtopt/json/json_reservoir_production_factor.hpp>
#include <gtopt/json/json_reservoir_seepage.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/reservoir.hpp>

namespace daw::json
{
using gtopt::Reservoir;

template<>
struct json_data_contract<Reservoir>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"junction", SingleId>,
      json_number_null<"spillway_capacity", OptReal>,
      json_number_null<"spillway_cost", OptReal>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"emax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"ecost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"eini", OptReal>,
      json_number_null<"efin", OptReal>,
      json_number_null<"mean_production_factor", OptReal>,
      json_variant_null<"scost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"soft_emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"soft_emin_cost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_number_null<"fmin", OptReal>,
      json_number_null<"fmax", OptReal>,
      json_number_null<"energy_scale", OptReal>,
      json_string_null<"energy_scale_mode", OptName>,
      json_number_null<"flow_conversion_rate", OptReal>,
      json_bool_null<"use_state_variable", OptBool>,
      json_bool_null<"daily_cycle", OptBool>,
      json_array_null<"seepage", Array<ReservoirSeepage>, ReservoirSeepage>,
      json_array_null<"discharge_limit",
                      Array<ReservoirDischargeLimit>,
                      ReservoirDischargeLimit>,
      json_array_null<"production_factor",
                      Array<ReservoirProductionFactor>,
                      ReservoirProductionFactor>>;

  constexpr static auto to_json_data(Reservoir const& reservoir)
  {
    return std::forward_as_tuple(reservoir.uid,
                                 reservoir.name,
                                 reservoir.active,
                                 reservoir.junction,
                                 reservoir.spillway_capacity,
                                 reservoir.spillway_cost,
                                 reservoir.capacity,
                                 reservoir.annual_loss,
                                 reservoir.emin,
                                 reservoir.emax,
                                 reservoir.ecost,
                                 reservoir.eini,
                                 reservoir.efin,
                                 reservoir.mean_production_factor,
                                 reservoir.scost,
                                 reservoir.soft_emin,
                                 reservoir.soft_emin_cost,
                                 reservoir.fmin,
                                 reservoir.fmax,
                                 reservoir.energy_scale,
                                 reservoir.energy_scale_mode,
                                 reservoir.flow_conversion_rate,
                                 reservoir.use_state_variable,
                                 reservoir.daily_cycle,
                                 reservoir.seepage,
                                 reservoir.discharge_limit,
                                 reservoir.production_factor);
  }
};
}  // namespace daw::json
