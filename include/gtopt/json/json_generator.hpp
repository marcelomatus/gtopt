#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/generator.hpp>
#include <gtopt/json/json_emission_capture.hpp>
#include <gtopt/json/json_emission_source.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::GeneratorAttrs;

template<>
struct json_data_contract<GeneratorAttrs>
{
  using type = json_member_list<
      json_variant<"bus", SingleId>,
      json_variant_null<"pmin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"pmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"lossfactor",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"gcost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"pmin_fcost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"fuel", OptSingleId, jvtl_SingleId>,
      json_variant_null<"fuel_per_block",
                        OptTBUidFieldSched,
                        jvtl_TBUidFieldSched>,
      json_variant_null<"heat_rate",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_array_null<"pmax_segments", Array<Real>, json_number_no_name<Real>>,
      json_array_null<"heat_rate_segments",
                      Array<Real>,
                      json_number_no_name<Real>>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expcap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expmod", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_bool_null<"integer_expmod", OptBool>,
      json_number_null<"uini", OptReal>>;

  constexpr static auto to_json_data(GeneratorAttrs const& attrs)
  {
    return std::forward_as_tuple(attrs.bus,
                                 attrs.pmin,
                                 attrs.pmax,
                                 attrs.lossfactor,
                                 attrs.gcost,
                                 attrs.pmin_fcost,
                                 attrs.fuel,
                                 attrs.fuel_per_block,
                                 attrs.heat_rate,
                                 attrs.pmax_segments,
                                 attrs.heat_rate_segments,
                                 attrs.capacity,
                                 attrs.expcap,
                                 attrs.expmod,
                                 attrs.capmax,
                                 attrs.annual_capcost,
                                 attrs.annual_derating,
                                 attrs.integer_expmod,
                                 attrs.uini);
  }
};

using gtopt::Generator;

template<>
struct json_data_contract<Generator>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_string_null<"description", OptName>,
      json_variant<"bus", SingleId>,
      json_variant_null<"pmin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"pmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"lossfactor",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"gcost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"pmin_fcost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"fuel", OptSingleId, jvtl_SingleId>,
      json_variant_null<"fuel_per_block",
                        OptTBUidFieldSched,
                        jvtl_TBUidFieldSched>,
      json_variant_null<"heat_rate",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_array_null<"pmax_segments", Array<Real>, json_number_no_name<Real>>,
      json_array_null<"heat_rate_segments",
                      Array<Real>,
                      json_number_no_name<Real>>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expcap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expmod", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_bool_null<"integer_expmod", OptBool>,
      json_number_null<"uini", OptReal>,
      json_variant_null<"emission_rate",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_array_null<"emissions", Array<EmissionSource>, EmissionSource>,
      json_array_null<"emission_captures",
                      Array<EmissionCapture>,
                      EmissionCapture>>;

  constexpr static auto to_json_data(Generator const& generator)
  {
    return std::forward_as_tuple(generator.uid,
                                 generator.name,
                                 generator.active,
                                 generator.type,
                                 generator.description,
                                 generator.bus,
                                 generator.pmin,
                                 generator.pmax,
                                 generator.lossfactor,
                                 generator.gcost,
                                 generator.pmin_fcost,
                                 generator.fuel,
                                 generator.fuel_per_block,
                                 generator.heat_rate,
                                 generator.pmax_segments,
                                 generator.heat_rate_segments,
                                 generator.capacity,
                                 generator.expcap,
                                 generator.expmod,
                                 generator.capmax,
                                 generator.annual_capcost,
                                 generator.annual_derating,
                                 generator.integer_expmod,
                                 generator.uini,
                                 generator.emission_rate,
                                 generator.emissions,
                                 generator.emission_captures);
  }
};

}  // namespace daw::json
