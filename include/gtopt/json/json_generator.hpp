#pragma once

#include <gtopt/generator.hpp>
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
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"gcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
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

  constexpr static auto to_json_data(GeneratorAttrs const& attrs)
  {
    return std::forward_as_tuple(attrs.bus,
                                 attrs.pmin,
                                 attrs.pmax,
                                 attrs.lossfactor,
                                 attrs.gcost,
                                 attrs.capacity,
                                 attrs.expcap,
                                 attrs.expmod,
                                 attrs.capmax,
                                 attrs.annual_capcost,
                                 attrs.annual_derating);
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
      json_variant<"bus", SingleId>,
      json_variant_null<"pmin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"pmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"gcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
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

  constexpr static auto to_json_data(Generator const& generator)
  {
    return std::forward_as_tuple(generator.uid,
                                 generator.name,
                                 generator.active,
                                 generator.bus,
                                 generator.pmin,
                                 generator.pmax,
                                 generator.lossfactor,
                                 generator.gcost,
                                 generator.capacity,
                                 generator.expcap,
                                 generator.expmod,
                                 generator.capmax,
                                 generator.annual_capcost,
                                 generator.annual_derating);
  }
};

}  // namespace daw::json
