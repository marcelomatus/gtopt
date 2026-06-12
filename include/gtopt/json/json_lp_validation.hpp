/**
 * @file      json_lp_validation.hpp
 * @brief     JSON serialization for LpValidationOptions
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/lp_validation.hpp>

namespace daw::json
{
using gtopt::LpValidationOptions;

struct LpValidationOptionsConstructor
{
  [[nodiscard]] LpValidationOptions operator()(
      OptBool enable,
      OptReal coeff_warn_max,
      OptReal coeff_warn_min,
      OptReal bound_warn_max,
      OptReal rhs_warn_max,
      OptReal obj_warn_max,
      OptInt max_warnings_per_kind) const
  {
    LpValidationOptions opts;
    opts.enable = enable;
    opts.coeff_warn_max = coeff_warn_max;
    opts.coeff_warn_min = coeff_warn_min;
    opts.bound_warn_max = bound_warn_max;
    opts.rhs_warn_max = rhs_warn_max;
    opts.obj_warn_max = obj_warn_max;
    opts.max_warnings_per_kind = max_warnings_per_kind;
    return opts;
  }
};

template<>
struct json_data_contract<LpValidationOptions>
{
  using constructor_t = LpValidationOptionsConstructor;

  using type =
      json_member_list<json_bool_null<"enable", OptBool>,
                       json_number_null<"coeff_warn_max", OptReal>,
                       json_number_null<"coeff_warn_min", OptReal>,
                       json_number_null<"bound_warn_max", OptReal>,
                       json_number_null<"rhs_warn_max", OptReal>,
                       json_number_null<"obj_warn_max", OptReal>,
                       json_number_null<"max_warnings_per_kind", OptInt>>;

  static auto to_json_data(LpValidationOptions const& opt)
  {
    return std::make_tuple(opt.enable,
                           opt.coeff_warn_max,
                           opt.coeff_warn_min,
                           opt.bound_warn_max,
                           opt.rhs_warn_max,
                           opt.obj_warn_max,
                           opt.max_warnings_per_kind);
  }
};

}  // namespace daw::json
