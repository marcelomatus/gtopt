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
  /// Kirchhoff Voltage Law formulation: `"node_angle"` (B–θ, default)
  /// or `"cycle_basis"` (loop-flow).  See KirchhoffMode enum.
  /// When unset, defaults to `"node_angle"`.
  OptName kirchhoff_mode {};
  /// @deprecated Use `line_losses_mode` instead.
  OptBool use_line_losses {};
  /// Line losses model selection.  See LineLossesMode enum for values:
  /// `"none"`, `"linear"`, `"piecewise"`, `"bidirectional"`, `"adaptive"`,
  /// `"dynamic"`.  When unset, defaults to `"adaptive"`.
  OptName line_losses_mode {};
  /// Minimum bus voltage [kV] below which Kirchhoff is not applied.
  OptReal kirchhoff_threshold {};
  /// Per-unit reactance floor `|X/V²|` below which a transmission line
  /// is auto-promoted to a "DC line" (no Kirchhoff coupling).  The
  /// validation step in `PlanningLP::validate_line_reactance` rewrites
  /// the line's reactance schedule to scalar `0.0` so the LP assembler
  /// skips the θ-row, capping the spread of `x_τ = τ·X/V²` coefficients
  /// in the Kirchhoff matrix and dropping kappa.  Default (when unset):
  /// `1e-6` p.u. — conservative enough that real transmission lines
  /// (`x_pu ≥ 1e-3`) and real distribution lines/cables (`x_pu ≥ 1e-4`)
  /// are never falsely promoted, while V-vs-kV unit-typo lines
  /// (`x_pu ≈ 1e-7…1e-9`) and HVDC/phase-shifter sentinels (`X = 0`)
  /// are caught.  Set to `0.0` to disable promotion entirely.
  OptReal dc_line_reactance_threshold {};
  /// Number of piecewise-linear segments for quadratic line losses.
  OptInt loss_segments {};
  /// Divisor for all objective coefficients (numerical stability).
  OptReal scale_objective {};
  /// Scaling factor for voltage-angle variables.
  OptReal scale_theta {};
  /// Bound for voltage-angle variables: `θ ∈ [−theta_max, +theta_max]`.
  /// When unset, `PlanningLP::auto_scale_theta` computes it as
  /// `Σ_l tmax_l · x_τ_l` (a topology-aware upper bound on the
  /// largest possible θ spread between any two buses) so the bound
  /// never artificially caps line flows below their `tmax`.  The
  /// historical hardcoded `2π` default is preserved as a fallback when
  /// `auto_scale=false`.
  OptReal theta_max {};
  /// Enable per-element automatic scaling (reservoir energy/flow, LNG
  /// terminal energy, bus theta) that `PlanningLP` computes at
  /// construction time.  When unset or true, the default heuristics
  /// (adaptive emax/fmax → power-of-10 scale, median x_τ for
  /// scale_theta) run.  When set to false (typically via the
  /// `--no-scale` CLI flag), all three auto-scale passes are
  /// skipped so LP coefficients stay in raw physical units —
  /// useful for debug / coefficient validation, at the cost of
  /// much higher solver kappa.
  OptBool auto_scale {};
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
  /// Penalty cost for state variable violations in SDDP elastic filter
  /// [$/MWh].  Used as fallback when a reservoir (or other storage element)
  /// does not define its own `scost`.  Converted to physical units using
  /// the element's `mean_production_factor`.
  OptReal state_fail_cost {};

  /// System-wide CO2 emission cost [$/tCO2].
  /// When set, generators with a non-zero `emission_factor` incur an
  /// additional objective cost of emission_cost × emission_factor per MWh.
  OptTRealFieldSched emission_cost {};

  /// System-wide CO2 emission cap [tCO2/year] per stage.
  /// When set, a constraint is added per stage:
  ///   sum_g sum_b (emission_factor_g × p_{g,b} × duration_b) ≤ cap_s
  /// The dual of this constraint is the endogenous carbon price.
  OptTRealFieldSched emission_cap {};

  /// Phase range expression controlling which phases use LP relaxation
  /// (all integer/binary variables become continuous).
  /// Syntax: `"all"`, `"none"` (default), `"0"`, `"1,3:5,8:"`, `":3"`.
  /// Sets `phase.continuous = true` on matching phases at LP setup time.
  /// Settable per cascade level or globally via
  /// `--set model_options.continuous_phases="all"`.
  OptName continuous_phases {};

  /// Whether to enforce the per-stage `emin` floor as a HARD lower bound
  /// on the reservoir's stage-end volume (`efin =
  /// reservoir_energy_<last_block>`) and on the stage-start volume
  /// (`reservoir_sini`).
  ///
  /// `true` (default): both columns get `lowb = stage_emin`.  The floor is a
  /// hard constraint at the inter-stage handoff state, giving the strictest
  /// volume-constraint enforcement.  Intra-stage blocks still use `lowb = 0`
  /// (see `storage_lp.hpp`) so the energy-balance row keeps PLP-style
  /// headroom mid-stage.
  ///
  /// `false` (opt-out, PLP-style): both columns have `lowb = 0`.  The floor
  /// is not enforced as a constraint; SDDP convergence is responsible for
  /// keeping the trajectory above `emin`.  Matches PLP's per-stage LP where
  /// `ve<u>` is `Free` mid-stage and only `vf<u>` (future volume) has the
  /// `vmin` lower bound.  Use this if iter-0 of an SDDP cascade
  /// over-constrains the forward pass when state vars cluster at the floor.
  OptBool strict_storage_emin {};

  void merge(const ModelOptions& opts)
  {
    merge_opt(use_single_bus, opts.use_single_bus);
    merge_opt(use_kirchhoff, opts.use_kirchhoff);
    merge_opt(kirchhoff_mode, opts.kirchhoff_mode);
    merge_opt(use_line_losses, opts.use_line_losses);
    merge_opt(line_losses_mode, opts.line_losses_mode);
    merge_opt(kirchhoff_threshold, opts.kirchhoff_threshold);
    merge_opt(dc_line_reactance_threshold, opts.dc_line_reactance_threshold);
    merge_opt(loss_segments, opts.loss_segments);
    merge_opt(scale_objective, opts.scale_objective);
    merge_opt(scale_theta, opts.scale_theta);
    merge_opt(theta_max, opts.theta_max);
    merge_opt(auto_scale, opts.auto_scale);
    merge_opt(demand_fail_cost, opts.demand_fail_cost);
    merge_opt(reserve_fail_cost, opts.reserve_fail_cost);
    merge_opt(hydro_fail_cost, opts.hydro_fail_cost);
    merge_opt(hydro_use_value, opts.hydro_use_value);
    merge_opt(state_fail_cost, opts.state_fail_cost);
    merge_opt(emission_cost, opts.emission_cost);
    merge_opt(emission_cap, opts.emission_cap);
    merge_opt(continuous_phases, opts.continuous_phases);
    merge_opt(strict_storage_emin, opts.strict_storage_emin);
  }

  /// True if any field is set.
  [[nodiscard]] bool has_any() const noexcept
  {
    return use_single_bus.has_value() || use_kirchhoff.has_value()
        || kirchhoff_mode.has_value() || use_line_losses.has_value()
        || line_losses_mode.has_value() || kirchhoff_threshold.has_value()
        || dc_line_reactance_threshold.has_value() || loss_segments.has_value()
        || scale_objective.has_value() || scale_theta.has_value()
        || theta_max.has_value() || demand_fail_cost.has_value()
        || reserve_fail_cost.has_value() || hydro_fail_cost.has_value()
        || hydro_use_value.has_value() || state_fail_cost.has_value()
        || emission_cost.has_value() || emission_cap.has_value()
        || continuous_phases.has_value() || strict_storage_emin.has_value();
  }

  /// True iff every field set in `other` has an equal value in `*this`.
  /// Fields that `other` leaves unset are ignored.  Semantically: "applying
  /// `other` as an override on top of `*this` would not change anything".
  [[nodiscard]] bool covers(const ModelOptions& other) const noexcept
  {
    const auto covers_opt = [](const auto& self, const auto& override_val)
    { return !override_val.has_value() || self == override_val; };
    return covers_opt(use_single_bus, other.use_single_bus)
        && covers_opt(use_kirchhoff, other.use_kirchhoff)
        && covers_opt(kirchhoff_mode, other.kirchhoff_mode)
        && covers_opt(use_line_losses, other.use_line_losses)
        && covers_opt(line_losses_mode, other.line_losses_mode)
        && covers_opt(kirchhoff_threshold, other.kirchhoff_threshold)
        && covers_opt(dc_line_reactance_threshold,
                      other.dc_line_reactance_threshold)
        && covers_opt(loss_segments, other.loss_segments)
        && covers_opt(scale_objective, other.scale_objective)
        && covers_opt(scale_theta, other.scale_theta)
        && covers_opt(theta_max, other.theta_max)
        && covers_opt(demand_fail_cost, other.demand_fail_cost)
        && covers_opt(reserve_fail_cost, other.reserve_fail_cost)
        && covers_opt(hydro_fail_cost, other.hydro_fail_cost)
        && covers_opt(hydro_use_value, other.hydro_use_value)
        && covers_opt(state_fail_cost, other.state_fail_cost)
        && covers_opt(emission_cost, other.emission_cost)
        && covers_opt(emission_cap, other.emission_cap)
        && covers_opt(continuous_phases, other.continuous_phases)
        && covers_opt(strict_storage_emin, other.strict_storage_emin);
  }
};

}  // namespace gtopt
