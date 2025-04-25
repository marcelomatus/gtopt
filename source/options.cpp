/**
 * @file      options.cpp
 * @brief     Header of
 * @date      Tue Apr 22 04:08:32 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/options.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

Options& Options::merge(Options& opts)
{
  merge_opt(input_directory, std::move(opts.input_directory));
  merge_opt(input_format, std::move(opts.input_format));

  merge_opt(demand_fail_cost, opts.demand_fail_cost);
  merge_opt(reserve_fail_cost, opts.reserve_fail_cost);
  merge_opt(use_line_losses, opts.use_line_losses);
  merge_opt(use_kirchhoff, opts.use_kirchhoff);
  merge_opt(use_single_bus, opts.use_single_bus);
  merge_opt(kirchhoff_threshold, opts.kirchhoff_threshold);
  merge_opt(scale_objective, opts.scale_objective);
  merge_opt(scale_theta, opts.scale_theta);

  merge_opt(output_directory, std::move(opts.output_directory));
  merge_opt(output_format, std::move(opts.output_format));
  merge_opt(use_lp_names, opts.use_lp_names);
  merge_opt(use_uid_fname, opts.use_uid_fname);
  merge_opt(annual_discount_rate, opts.annual_discount_rate);

  return *this;
}

}  // namespace gtopt
