#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/filtration.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Filtration;
using gtopt::FiltrationSegment;

template<>
struct json_data_contract<FiltrationSegment>
{
  using type = json_member_list<json_number<"volume", Real>,
                                json_number<"slope", Real>,
                                json_number<"constant", Real>>;

  static constexpr auto to_json_data(FiltrationSegment const& seg)
  {
    return std::forward_as_tuple(seg.volume, seg.slope, seg.constant);
  }
};

template<>
struct json_data_contract<Filtration>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_variant<"waterway", SingleId>,
                       json_variant<"reservoir", SingleId>,
                       json_number_null<"slope", Real>,
                       json_number_null<"constant", Real>,
                       json_array_null<"segments",
                                       std::vector<FiltrationSegment>,
                                       FiltrationSegment>>;

  static constexpr auto to_json_data(Filtration const& filtration)
  {
    return std::forward_as_tuple(filtration.uid,
                                 filtration.name,
                                 filtration.active,
                                 filtration.waterway,
                                 filtration.reservoir,
                                 filtration.slope,
                                 filtration.constant,
                                 filtration.segments);
  }
};
}  // namespace daw::json
