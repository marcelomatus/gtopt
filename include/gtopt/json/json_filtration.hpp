#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/filtration.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Filtration;

template<>
struct json_data_contract<Filtration>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_variant<"waterway", SingleId>,
                       json_variant<"reservoir", SingleId>,
                       json_number<"slope", Real>,
                       json_number<"constant", Real>>;

  constexpr static auto to_json_data(Filtration const& filtration)
  {
    return std::forward_as_tuple(filtration.uid,
                                 filtration.name,
                                 filtration.active,
                                 filtration.waterway,
                                 filtration.reservoir,
                                 filtration.slope,
                                 filtration.constant);
  }
};
}  // namespace daw::json
