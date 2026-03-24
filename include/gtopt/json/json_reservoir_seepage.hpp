#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/reservoir_seepage.hpp>

namespace daw::json
{
using gtopt::ReservoirSeepage;
using gtopt::ReservoirSeepageSegment;

template<>
struct json_data_contract<ReservoirSeepageSegment>
{
  using type = json_member_list<json_number<"volume", Real>,
                                json_number<"slope", Real>,
                                json_number<"constant", Real>>;

  static constexpr auto to_json_data(ReservoirSeepageSegment const& seg)
  {
    return std::forward_as_tuple(seg.volume, seg.slope, seg.constant);
  }
};

template<>
struct json_data_contract<ReservoirSeepage>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"waterway", SingleId>,
      json_variant<"reservoir", SingleId>,
      json_variant_null<"slope", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"constant", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_array_null<"segments",
                      std::vector<ReservoirSeepageSegment>,
                      ReservoirSeepageSegment>>;

  static constexpr auto to_json_data(ReservoirSeepage const& seepage)
  {
    return std::forward_as_tuple(seepage.uid,
                                 seepage.name,
                                 seepage.active,
                                 seepage.waterway,
                                 seepage.reservoir,
                                 seepage.slope,
                                 seepage.constant,
                                 seepage.segments);
  }
};
}  // namespace daw::json
