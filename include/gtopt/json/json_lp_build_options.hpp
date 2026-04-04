/**
 * @file      json_lp_build_options.hpp
 * @brief     JSON serialization for LP build options
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/lp_build_options.hpp>

namespace daw::json
{
using gtopt::LpBuildOptions;

/// Custom construction for LpBuildOptions: only the user-facing
/// optional fields are exposed in JSON; all other fields keep defaults.
struct LpBuildOptionsConstructor
{
  [[nodiscard]] LpBuildOptions operator()(OptInt names_level_int,
                                          OptReal lp_coeff_ratio_threshold,
                                          OptName equilibration_method_name,
                                          OptBool compute_stats) const
  {
    LpBuildOptions opts;
    if (names_level_int) {
      opts.names_level = static_cast<gtopt::LpNamesLevel>(*names_level_int);
    }
    opts.lp_coeff_ratio_threshold = lp_coeff_ratio_threshold;
    if (equilibration_method_name) {
      opts.equilibration_method =
          gtopt::enum_from_name<gtopt::LpEquilibrationMethod>(
              *equilibration_method_name);
    }
    opts.compute_stats = compute_stats;
    return opts;
  }
};

template<>
struct json_data_contract<LpBuildOptions>
{
  using constructor_t = LpBuildOptionsConstructor;

  using type =
      json_member_list<json_number_null<"names_level", OptInt>,
                       json_number_null<"lp_coeff_ratio_threshold", OptReal>,
                       json_string_null<"equilibration_method", OptName>,
                       json_bool_null<"compute_stats", OptBool>>;

  static auto to_json_data(LpBuildOptions const& opt)
  {
    const OptInt names_int = opt.names_level
        ? OptInt {static_cast<int>(*opt.names_level)}
        : OptInt {};
    const OptName eq_name = opt.equilibration_method
        ? OptName {std::string(gtopt::enum_name(*opt.equilibration_method))}
        : OptName {};
    return std::make_tuple(
        names_int, opt.lp_coeff_ratio_threshold, eq_name, opt.compute_stats);
  }
};

}  // namespace daw::json
