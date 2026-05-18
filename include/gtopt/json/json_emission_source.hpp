/**
 * @file      json_emission_source.hpp
 * @brief     JSON serialization for EmissionSource
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/emission_source.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::EmissionSource;

template<>
struct json_data_contract<EmissionSource>
{
  // `uid`, `name`, and `generator` are nullable because the inline form
  // on `Generator.emissions[]` omits them — `System::expand_emission_sources()`
  // auto-allocates the uid, synthesizes the name from the parent
  // (`<generator>_emission_<uid>`), and stamps the `generator` FK at
  // expansion time.  The canonical top-level form may still set all
  // three explicitly.
  using type = json_member_list<
      json_number_null<"uid", Uid>,
      json_string_null<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"generator", OptSingleId, jvtl_SingleId>,
      json_variant<"zone", SingleId>,
      json_variant_null<"rate", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(EmissionSource const& s)
  {
    return std::forward_as_tuple(
        s.uid, s.name, s.active, s.generator, s.zone, s.rate);
  }
};

}  // namespace daw::json
