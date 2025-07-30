#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/waterway.hpp>

namespace daw::json
{
using gtopt::Waterway;

template<>
struct json_data_contract<Waterway>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"junction_a", SingleId>,
      json_variant<"junction_b", SingleId>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"fmin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"fmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>>;

  constexpr static auto to_json_data(Waterway const& waterway)
  {
    return std::forward_as_tuple(waterway.uid,
                                 waterway.name,
                                 waterway.active,
                                 waterway.junction_a,
                                 waterway.junction_b,
                                 waterway.capacity,
                                 waterway.lossfactor,
                                 waterway.fmin,
                                 waterway.fmax);
  }
};
}  // namespace daw::json
