/**
 * @file      json_emission_source.hpp
 * @brief     JSON serialization for EmissionSource
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Canonical top-level form (every field explicit):
 * ```json
 * {"uid": 1, "name": "ngcc_co2",
 *  "generator": "ngcc_la", "zone": "global_co2", "emission": "co2",
 *  "rate": 0.4,           // combustion (TTW) tons / MWh
 *  "upstream_rate": 0.05  // optional well-to-tank (WTT) tons / MWh
 * }
 * ```
 *
 * Inline-on-generator shorthand (`Generator.emissions[]`): omits
 * `uid` / `name` / `generator`, which are auto-filled at parse time
 * by `System::expand_emission_sources()`.
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
  // `uid`, `name`, and `generator` are nullable because the inline
  // form on `Generator.emissions[]` omits them — `expand_emission_sources()`
  // stamps them at parse time.  `zone` and `emission` are required
  // (they identify which zone-balance and which pollutant the source
  // contributes to).
  using type = json_member_list<
      json_number_null<"uid", Uid>,
      json_string_null<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"generator", OptSingleId, jvtl_SingleId>,
      json_variant<"zone", SingleId>,
      json_variant<"emission", SingleId>,
      json_variant_null<"rate", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"upstream_rate",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(EmissionSource const& s)
  {
    return std::forward_as_tuple(s.uid,
                                 s.name,
                                 s.active,
                                 s.generator,
                                 s.zone,
                                 s.emission,
                                 s.rate,
                                 s.upstream_rate);
  }
};

}  // namespace daw::json
