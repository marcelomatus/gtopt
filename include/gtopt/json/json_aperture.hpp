/**
 * @file      json_aperture.hpp
 * @brief     JSON serialization for the Aperture struct
 * @date      Thu Mar 13 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/aperture.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json
{
using gtopt::Aperture;

template<>
struct json_data_contract<Aperture>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string_null<"name", OptName>,
                       json_number_null<"active", OptBool>,
                       json_number<"source_scenario", Uid>,
                       json_number_null<"probability_factor", OptReal>>;

  [[nodiscard]] constexpr static auto to_json_data(
      Aperture const& aperture)
  {
    return std::forward_as_tuple(aperture.uid,
                                 aperture.name,
                                 aperture.active,
                                 aperture.source_scenario,
                                 aperture.probability_factor);
  }
};

}  // namespace daw::json
