#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>

namespace daw::json
{
using gtopt::ReservoirDischargeLimit;
using gtopt::ReservoirDischargeLimitSegment;

template<>
struct json_data_contract<ReservoirDischargeLimitSegment>
{
  using type = json_member_list<json_number<"volume", Real>,
                                json_number<"slope", Real>,
                                json_number<"intercept", Real>>;

  static constexpr auto to_json_data(ReservoirDischargeLimitSegment const& seg)
  {
    return std::forward_as_tuple(seg.volume, seg.slope, seg.intercept);
  }
};

template<>
struct json_data_contract<ReservoirDischargeLimit>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"waterway", SingleId>,
      json_variant<"reservoir", SingleId>,
      json_array_null<"segments",
                      std::vector<ReservoirDischargeLimitSegment>,
                      ReservoirDischargeLimitSegment>>;

  static constexpr auto to_json_data(ReservoirDischargeLimit const& ddl)
  {
    return std::forward_as_tuple(ddl.uid,
                                 ddl.name,
                                 ddl.active,
                                 ddl.waterway,
                                 ddl.reservoir,
                                 ddl.segments);
  }
};
}  // namespace daw::json
