#pragma once

#include <gtopt/demand_profile.hpp>
#include <gtopt/json/json_demand.hpp>

namespace daw::json
{
using gtopt::DemandProfile;

template<>
struct json_data_contract<DemandProfile>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"demand", DemandVar, jvtl_DemandVar>,
      json_variant<"profile", STBRealFieldSched, jvtl_STBRealFieldSched>,
      json_variant_null<"scost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(DemandProfile const& demand_profile)
  {
    return std::forward_as_tuple(demand_profile.uid,
                                 demand_profile.name,
                                 demand_profile.active,
                                 demand_profile.demand,
                                 demand_profile.profile,
                                 demand_profile.scost);
  }
};
}  // namespace daw::json
