/**
 * @file      model_options.hpp
 * @brief     Power system model configuration for LP construction
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief Power system model configuration for LP construction.
 *
 * Groups all options that affect how the mathematical model is
 * formulated: network topology, Kirchhoff constraints, line losses,
 * scaling factors, penalty costs, and discount rate.
 *
 * Used both as a global sub-object in Options (`model_options`) and
 * as per-level overrides in CascadeLevel (`model_options`).
 * When used in a cascade level, only set fields override the global;
 * absent fields inherit from the global configuration.
 */
struct ModelOptions
{
  /// Collapse the network to a single bus (copper-plate model).
  OptBool use_single_bus {};
  /// Apply DC Kirchhoff voltage-law constraints.
  OptBool use_kirchhoff {};
  /// @deprecated Use `line_losses_mode` instead.
  OptBool use_line_losses {};
  /// Line losses model selection.  See LineLossesMode enum for values:
  /// `"none"`, `"linear"`, `"piecewise"`, `"bidirectional"`, `"adaptive"`,
  /// `"dynamic"`.  When unset, defaults to `"adaptive"`.
  OptName line_losses_mode {};
  /// Minimum bus voltage [kV] below which Kirchhoff is not applied.
  OptReal kirchhoff_threshold {};
  /// Number of piecewise-linear segments for quadratic line losses.
  OptInt loss_segments {};
  /// Divisor for all objective coefficients (numerical stability).
  OptReal scale_objective {};
  /// Scaling factor for voltage-angle variables.
  OptReal scale_theta {};
  /// Penalty cost for unserved demand [$/MWh].
  OptReal demand_fail_cost {};
  /// Penalty cost for unserved spinning-reserve [$/MWh].
  OptReal reserve_fail_cost {};
  /// Default penalty cost for unmet hydro rights [$/m3].
  /// Per-element `fail_cost` overrides this global default.
  OptReal hydro_fail_cost {};
  /// Default value (benefit) of exercising hydro rights [$/m3].
  /// Per-element `use_value` overrides this global default.
  OptReal hydro_use_value {};

  void merge(const ModelOptions& opts)
  {
    merge_opt(use_single_bus, opts.use_single_bus);
    merge_opt(use_kirchhoff, opts.use_kirchhoff);
    merge_opt(use_line_losses, opts.use_line_losses);
    merge_opt(line_losses_mode, opts.line_losses_mode);
    merge_opt(kirchhoff_threshold, opts.kirchhoff_threshold);
    merge_opt(loss_segments, opts.loss_segments);
    merge_opt(scale_objective, opts.scale_objective);
    merge_opt(scale_theta, opts.scale_theta);
    merge_opt(demand_fail_cost, opts.demand_fail_cost);
    merge_opt(reserve_fail_cost, opts.reserve_fail_cost);
    merge_opt(hydro_fail_cost, opts.hydro_fail_cost);
    merge_opt(hydro_use_value, opts.hydro_use_value);
  }

  /// True if any field is set.
  [[nodiscard]] bool has_any() const noexcept
  {
    return use_single_bus.has_value() || use_kirchhoff.has_value()
        || use_line_losses.has_value() || line_losses_mode.has_value()
        || kirchhoff_threshold.has_value() || loss_segments.has_value()
        || scale_objective.has_value() || scale_theta.has_value()
        || demand_fail_cost.has_value() || reserve_fail_cost.has_value()
        || hydro_fail_cost.has_value() || hydro_use_value.has_value();
  }
};

}  // namespace gtopt
