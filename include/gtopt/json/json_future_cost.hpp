/**
 * @file      json_future_cost.hpp
 * @brief     JSON serialization for FutureCost (FCF / cost-to-go) elements
 * @date      Sun Jun 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The three scoping fields (`sharing`, `mode`, `valuation`) are authored as
 * JSON strings and converted to their typed enums via a custom constructor —
 * the same pattern MonolithicOptions uses.
 */

#pragma once

#include <utility>

#include <daw/json/daw_json_link.h>
#include <gtopt/future_cost.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::BoundaryCutSharingMode;
using gtopt::BoundaryCutsMode;
using gtopt::BoundaryCutSoftCost;
using gtopt::FutureCost;

/// Custom constructor: converts the JSON enum strings → typed enums.
struct FutureCostConstructor
{
  [[nodiscard]] FutureCost operator()(Uid uid,
                                      Name name,
                                      OptActive active,
                                      OptName description,
                                      OptName cuts_file,
                                      OptReal scale_alpha,
                                      OptBool mean_shift,
                                      OptBool single_cut_equality,
                                      OptName sharing_str,
                                      OptName mode_str,
                                      OptName valuation_str,
                                      OptBool use_user_alpha,
                                      OptUid user_alpha_uid) const
  {
    FutureCost fc {
        .uid = uid,
        .name = std::move(name),
        .active = active,
        .description = std::move(description),
        .cuts_file = std::move(cuts_file),
        .scale_alpha = scale_alpha,
        .mean_shift = mean_shift,
        .single_cut_equality = single_cut_equality,
        .use_user_alpha = use_user_alpha,
        .user_alpha_uid = user_alpha_uid,
    };
    if (sharing_str) {
      fc.sharing =
          gtopt::require_enum<BoundaryCutSharingMode>("sharing", *sharing_str);
    }
    if (mode_str) {
      fc.mode = gtopt::require_enum<BoundaryCutsMode>("mode", *mode_str);
    }
    if (valuation_str) {
      fc.valuation =
          gtopt::require_enum<BoundaryCutSoftCost>("valuation", *valuation_str);
    }
    return fc;
  }
};

template<>
struct json_data_contract<FutureCost>
{
  using constructor_t = FutureCostConstructor;

  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_string_null<"description", OptName>,
                       json_string_null<"cuts_file", OptName>,
                       json_number_null<"scale_alpha", OptReal>,
                       json_bool_null<"mean_shift", OptBool>,
                       json_bool_null<"single_cut_equality", OptBool>,
                       json_string_null<"sharing", OptName>,
                       json_string_null<"mode", OptName>,
                       json_string_null<"valuation", OptName>,
                       json_bool_null<"use_user_alpha", OptBool>,
                       json_number_null<"user_alpha_uid", OptUid>>;

  static auto to_json_data(FutureCost const& fc)
  {
    return std::make_tuple(fc.uid,
                           fc.name,
                           fc.active,
                           fc.description,
                           fc.cuts_file,
                           fc.scale_alpha,
                           fc.mean_shift,
                           fc.single_cut_equality,
                           detail::enum_to_opt_name(fc.sharing),
                           detail::enum_to_opt_name(fc.mode),
                           detail::enum_to_opt_name(fc.valuation),
                           fc.use_user_alpha,
                           fc.user_alpha_uid);
  }
};

}  // namespace daw::json
