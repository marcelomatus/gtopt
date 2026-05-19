/**
 * @file      json_emission_capture.hpp
 * @brief     JSON serialization for EmissionCapture (CCS inline row)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/emission_capture.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::EmissionCapture;

template<>
struct json_data_contract<EmissionCapture>
{
  using type = json_member_list<
      json_variant<"emission", SingleId>,
      json_variant_null<"rate", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"cost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(EmissionCapture const& c)
  {
    return std::forward_as_tuple(c.emission, c.rate, c.cost);
  }
};

}  // namespace daw::json
