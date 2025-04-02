#pragma once

#include <gtopt/converter.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_generator.hpp>

namespace daw::json
{
using gtopt::Converter;

template<>
struct json_data_contract<Converter>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"battery", SingleId>,
      json_variant_null<"bus_generator", OptSingleId, jvtl_SingleId>,
      json_variant_null<"generator", OptGeneratorVar, jvtl_GeneratorVar>,
      json_variant_null<"bus_demand", OptSingleId, jvtl_SingleId>,
      json_variant_null<"demand", OptDemandVar, jvtl_DemandVar>,
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"conversion_rate",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,

      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expcap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expmod", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(Converter const& converter)
  {
    return std::forward_as_tuple(converter.uid,
                                 converter.name,
                                 converter.active,
                                 converter.battery,
                                 converter.bus_generator,
                                 converter.generator,
                                 converter.bus_demand,
                                 converter.demand,
                                 converter.lossfactor,
                                 converter.conversion_rate,
                                 converter.capacity,
                                 converter.expcap,
                                 converter.expmod,
                                 converter.capmax,
                                 converter.annual_capcost,
                                 converter.annual_derating);
  }
};
}  // namespace daw::json
