/**
 * @file      json_carrier_converter.hpp
 * @brief     JSON serialisation for ``CarrierConverter``
 * @date      Sat May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Two enum fields (``from_carrier`` / ``to_carrier``) are parsed via
 * ``require_enum<Carrier>`` — fail-fast on typo with an
 * ``(expected: electric, water, hydrogen, thermal, ammonia)`` hint.
 * Matches gtopt's ``StrictParsePolicy`` convention.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/carrier.hpp>
#include <gtopt/carrier_converter.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Carrier;
using gtopt::CarrierConverter;

/// Custom constructor: parses the carrier strings into the enum via
/// ``require_enum`` — throws ``std::invalid_argument`` with an
/// ``(expected: electric, water, hydrogen, thermal, ammonia)`` hint
/// on unknown / typo'd values.
struct CarrierConverterConstructor
{
  [[nodiscard]] CarrierConverter operator()(
      Uid uid,
      Name name,
      OptActive active,
      OptName type,
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      Name from_carrier_str,
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      Name to_carrier_str,
      OptSingleId from_node,
      OptSingleId to_node,
      OptTBRealFieldSched efficiency,
      OptTBRealFieldSched ocost,
      OptTRealFieldSched capacity,
      OptTRealFieldSched expcap,
      OptTRealFieldSched expmod,
      OptTRealFieldSched capmax,
      OptTRealFieldSched annual_capcost,
      OptTRealFieldSched annual_derating,
      OptBool integer_expmod) const
  {
    CarrierConverter c;
    c.uid = uid;
    c.name = std::move(name);
    c.active = std::move(active);
    c.type = std::move(type);
    c.from_carrier =
        gtopt::require_enum<Carrier>("from_carrier", from_carrier_str);
    c.to_carrier = gtopt::require_enum<Carrier>("to_carrier", to_carrier_str);
    c.from_node = std::move(from_node);
    c.to_node = std::move(to_node);
    c.efficiency = std::move(efficiency);
    c.ocost = std::move(ocost);
    c.capacity = std::move(capacity);
    c.expcap = std::move(expcap);
    c.expmod = std::move(expmod);
    c.capmax = std::move(capmax);
    c.annual_capcost = std::move(annual_capcost);
    c.annual_derating = std::move(annual_derating);
    c.integer_expmod = std::move(integer_expmod);
    return c;
  }
};

template<>
struct json_data_contract<CarrierConverter>
{
  using constructor_t = CarrierConverterConstructor;

  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_string<"from_carrier", Name>,
      json_string<"to_carrier", Name>,
      json_variant_null<"from_node", OptSingleId, jvtl_SingleId>,
      json_variant_null<"to_node", OptSingleId, jvtl_SingleId>,
      json_variant_null<"efficiency",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"ocost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
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
      json_bool_null<"integer_expmod", OptBool>>;

  static auto to_json_data(CarrierConverter const& c)
  {
    return std::make_tuple(c.uid,
                           c.name,
                           c.active,
                           c.type,
                           std::string(gtopt::enum_name(c.from_carrier)),
                           std::string(gtopt::enum_name(c.to_carrier)),
                           c.from_node,
                           c.to_node,
                           c.efficiency,
                           c.ocost,
                           c.capacity,
                           c.expcap,
                           c.expmod,
                           c.capmax,
                           c.annual_capcost,
                           c.annual_derating,
                           c.integer_expmod);
  }
};
}  // namespace daw::json
