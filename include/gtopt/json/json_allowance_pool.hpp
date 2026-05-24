/**
 * @file      json_allowance_pool.hpp
 * @brief     JSON serialisation for ``AllowancePool`` objects
 * @date      Sat May 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``json_thermal_storage.hpp`` retargeted to the tCO₂
 * allowance pool.  Field-name differences from the storage peers:
 *   * ``emission`` (FK to Emission element, replaces
 *     ``thermal_node`` / ``hydrogen_node`` / ``ammonia_node``).
 *   * ``delivery`` (free-allocation per stage, mirrors
 *     ``LngTerminal.delivery``).
 *   * ``auction_price`` + ``auction_cap`` (Phase 4 market-purchase
 *     fields; data-only on this commit, LP wiring lands later).
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/allowance_pool.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::AllowancePool;

template<>
struct json_data_contract<AllowancePool>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_variant_null<"emission", OptSingleId, jvtl_SingleId>,
      json_variant_null<"emin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"emax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"ecost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_number_null<"eini", OptReal>,
      json_number_null<"efin", OptReal>,
      json_number_null<"efin_cost", OptReal>,
      json_variant_null<"soft_emin",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"soft_emin_cost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"delivery", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"auction_price",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"auction_cap",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expcap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expmod", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_bool_null<"integer_expmod", OptBool>,
      json_bool_null<"use_state_variable", OptBool>,
      json_bool_null<"daily_cycle", OptBool>>;

  [[nodiscard]] constexpr static auto to_json_data(
      AllowancePool const& ap) noexcept
  {
    return std::forward_as_tuple(ap.uid,
                                 ap.name,
                                 ap.active,
                                 ap.type,
                                 ap.emission,
                                 ap.emin,
                                 ap.emax,
                                 ap.ecost,
                                 ap.eini,
                                 ap.efin,
                                 ap.efin_cost,
                                 ap.soft_emin,
                                 ap.soft_emin_cost,
                                 ap.delivery,
                                 ap.auction_price,
                                 ap.auction_cap,
                                 ap.capacity,
                                 ap.expcap,
                                 ap.expmod,
                                 ap.capmax,
                                 ap.annual_capcost,
                                 ap.annual_derating,
                                 ap.integer_expmod,
                                 ap.use_state_variable,
                                 ap.daily_cycle);
  }
};
}  // namespace daw::json
