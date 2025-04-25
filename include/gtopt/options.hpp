/**
 * @file      options.hpp
 * @brief     Header of
 * @date      Sun Mar 23 21:39:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Options
{
  OptName input_directory {};
  OptName input_format {};
  OptReal demand_fail_cost {};
  OptReal reserve_fail_cost {};
  OptBool use_line_losses {};
  OptBool use_kirchhoff {};
  OptBool use_single_bus {};
  OptReal kirchhoff_threshold {};
  OptReal scale_objective {};
  OptReal scale_theta {};

  OptName output_directory {};
  OptName output_format {};
  OptName compression_format {};
  OptBool use_lp_names {};
  OptBool use_uid_fname {};
  OptReal annual_discount_rate {};

  Options& merge(Options& opts);
};

}  // namespace gtopt
