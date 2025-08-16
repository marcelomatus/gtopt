#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/turbine.hpp>

namespace daw::json
{
using gtopt::Turbine;

template<>
struct json_data_contract<Turbine>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"waterway", SingleId>,
      json_variant<"generator", SingleId>,
      json_bool_null<"drain", OptBool>,
      json_variant_null<"conversion_rate",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(Turbine const& turbine)
  {
    return std::forward_as_tuple(turbine.uid,
                                 turbine.name,
                                 turbine.active,
                                 turbine.waterway,
                                 turbine.generator,
                                 turbine.drain,
                                 turbine.conversion_rate,
                                 turbine.capacity);
  }
};
}  // namespace daw::json
