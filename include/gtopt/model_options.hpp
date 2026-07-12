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
  /// Kirchhoff Voltage Law formulation: `"node_angle"` (B–θ) or
  /// `"cycle_basis"` (loop-flow, default).  See KirchhoffMode enum.
  /// When unset, defaults to `"cycle_basis"` — strictly smaller LP for
  /// meshed grids (no theta column per bus, fewer KVL rows).  Override
  /// with `"node_angle"` for cases with per-stage topology changes or
  /// phase-shift transformers.
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
  /// Per-unit resistance floor `|R/V²|` below which a transmission line's
  /// quadratic losses are dropped (the line is treated as lossless).  Mirrors
  /// `dc_line_reactance_threshold`: `PlanningLP::validate_line_resistance`
  /// rewrites the line's resistance schedule to scalar `0.0`, so the loss
  /// assembler (`line_losses`) takes its `R ≤ 0 → lossless` path and omits the
  /// loss-PWL entirely (both the loss-pricing row AND the `line_loss*_link`
  /// row).  This removes the huge `loss-link` coefficients (`∝ 1/(R/V²)`) that
  /// very-low-R lines — transformers, busbar segments, or V-vs-kV unit-typo
  /// lines (e.g. `R/V² ≈ 5e-8`) — otherwise inject as the largest matrix
  /// entries, blowing up `--no-scale` kappa.  Default (when unset): `1e-7`
  /// p.u. — one decade BELOW the reactance floor because real lines run lower
  /// in `R/V²` than `X/V²` (EHV lines have `R ≪ X`; a real 220 kV `R = 0.01 Ω`
  /// line is at `R/V² = 2e-7`), so `1e-7` spares genuine lossy lines while
  /// catching the degenerate ones.  Set to `0.0` to disable.
  OptReal dc_line_resistance_threshold {};
  /// Number of piecewise-linear segments for quadratic line losses.
  OptInt loss_segments {};
  /// Divisor for all objective coefficients (numerical stability).
  OptReal scale_objective {};
  /// Scaling factor for voltage-angle variables.
  OptReal scale_theta {};
  /// Row-scale factor for the line-loss linking constraint
  /// (`loss − Σ loss_k · seg_k = 0`).  When unset, `auto_scale_loss_link`
  /// computes a power-of-10 factor from `median(R/V²)` so the smallest
  /// segment coefficient `seg_width · R · 1 / V²` is lifted into a
  /// numerically tractable range.  Set explicitly to override; set to
  /// `1.0` to disable row scaling.  Has no effect on `linear`,
  /// `piecewise_direct`, or `none` loss modes.
  OptReal scale_loss_link {};
  /// Global per-MWh cost on the per-direction loss columns
  /// (``loss_p``/``loss_n`` in ``piecewise``/``bidirectional`` modes).
  /// Strictly breaks the LP-relax bidirectional-flow degeneracy: among
  /// all primal-feasible solutions sharing the same net dispatch, the
  /// LP picks the one with single-direction flow (``fn = 0`` for net
  /// flow ``f = fp − fn ≥ 0``).  Recommended: ``1e-6`` $/MWh — well
  /// below LP optimality tolerance so the objective is essentially
  /// unchanged.  Default ``0.0`` (unset) preserves legacy behaviour.
  /// Per-line ``Line.loss_cost_eps`` overrides this global default.
  /// Inert for ``none``, ``linear`` and ``piecewise_direct`` (no loss
  /// column).  In ``tangent_signed_flow`` ε prices the LOSS column
  /// (the abs-flow proxy ``v`` gets an internal 1e-6 degeneracy pin
  /// when ε > 0), and ε > 0 is REQUIRED when
  /// ``loss_secant_segments > 1`` without SOS2.  Sized
  /// ≥ ½·|worst credible bus-dual pair-sum| it is the pure-LP
  /// arbitrage guard, at loss-scaled (≈2λ·ε) incidence; see the
  /// sizing guidance at ``Line.loss_cost_eps`` (line.hpp).
  OptReal loss_cost_eps {};
  /// Global default for ``Line.loss_secant_segments`` (issue #504).
  /// When ``> 1`` and ``loss_use_sos2`` is enabled, the
  /// ``tangent_signed_flow`` loss model emits ``L`` segment columns
  /// per (line, block) with an SOS2 ordering constraint, tightening
  /// the loss upper bound from a single secant to a piecewise chord.
  /// Per-line ``Line.loss_secant_segments`` overrides this default.
  /// Unset → ``1`` (single-secant chord, current production behaviour).
  OptInt loss_secant_segments {};
  /// Global default for ``Line.loss_use_sos2`` (issue #504).
  /// Toggles SOS2 enforcement on the ``L`` secant-segment columns
  /// emitted by the ``tangent_signed_flow`` model when
  /// ``loss_secant_segments > 1``.  REQUIRES a MIP-capable LP
  /// backend with native SOS2 (CPLEX / Gurobi / HiGHS ≥ 1.6);
  /// the LP build raises a structured error if SOS2 is requested
  /// with an unsupporting backend.  Per-line override beats this
  /// global.  Unset → false.
  OptBool loss_use_sos2 {};
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
  OptReal reserve_shortage_cost {};
  /// Default penalty cost for unmet hydro rights [$/m³].
  /// Per-element `fail_cost` overrides this global default.
  OptReal hydro_spill_cost {};
  /// Default value (benefit) of exercising hydro rights [$/m³].
  /// Per-element `use_value` overrides this global default.
  OptReal hydro_use_value {};
  /// Penalty cost for state variable violations in SDDP elastic filter
  /// [$/MWh].  Used as fallback when a reservoir (or other storage element)
  /// does not define its own `scost`.  Converted to physical units using
  /// the element's `mean_production_factor`.
  OptReal state_violation_cost {};

  /// Demand-failure substitution with RHS shift (renamed from the
  /// legacy `demand_option_c` per §11.10 of
  /// `docs/analysis/naming-conventions.md`; both Options A and C
  /// substitute `fail = lmax − load`, but Option C *additionally*
  /// shifts the `+fail_cost·ecost·lmax` baseline off `obj_constant`
  /// onto the bus-balance and capacity rows — the new name
  /// describes that distinguishing behaviour rather than referencing
  /// an internal code label).  The legacy `demand_option_c` JSON
  /// key is accepted as an alias via the naming-dialects registry.
  ///
  /// When false (default): demand_lp emits the column as
  /// `load ∈ [0, lmax]` with cost = `−fail_cost × ecost` and folds
  /// the `+fail_cost × ecost × lmax` baseline into
  /// `lp.add_obj_constant(...)` (Option A — current behaviour).
  ///
  /// When true: column is `neg_fail = load − lmax ∈ [−lmax, 0]`,
  /// `obj_constant` stays at 0, and the baseline is absorbed by RHS
  /// shifts on the bus-balance row (`+(1+loss)·lmax`) and capacity
  /// row (`+lmax`).  The AMPL resolver receives a per-block offset
  /// so user constraints referencing `demand.load` keep their
  /// physical meaning (resolved as `col + lmax`).
  ///
  /// Option C eliminates the ~$105 B obj_constant baseline visible
  /// on juan-scale runs — `get_obj_value()` matches
  /// `get_obj_value_raw() × scale_objective` exactly with no
  /// large-magnitude cancellation noise.  Currently opt-in because
  /// LP-side consumers that reference the demand column directly
  /// (Converter, Battery interactions, etc.) are not yet
  /// Option-C-aware — they assume the column carries `load`, not
  /// `neg_fail`, so enabling this with a converter-tied demand
  /// produces a mathematically wrong row.  See
  /// `source/demand_lp.cpp` and the related deferred-follow-up note.
  OptBool demand_fail_rhs_shift {};

  /// Phase range expression controlling which phases use LP relaxation
  /// (all integer/binary variables become continuous).
  /// Syntax: `"all"`, `"none"` (default), `"0"`, `"1,3:5,8:"`, `":3"`.
  /// Sets `phase.continuous = true` on matching phases at LP setup time.
  /// Settable per cascade level or globally via
  /// `--set model_options.continuous_phases="all"`.
  OptName continuous_phases {};

  /// Name of the naming dialect to enforce on input and output.
  ///
  /// When set, the alias→canonical canonicalization (input) emits a
  /// once-per-alias warning whenever an input key matches an alias whose
  /// `dialect` tag differs from this value — useful for catching
  /// mixed-dialect input.  The reverse output rename (canonical → alias)
  /// is also driven by this field: the JSON writer rewrites canonical
  /// keys to the chosen dialect's aliases when emitting `planning.json`
  /// (parquet column rename is deferred to a follow-up).
  ///
  /// Recognised values are the `dialect` tags in
  /// `share/gtopt/naming_dialects.json` (e.g. `"gtopt"`, `"plp"`,
  /// `"sddp"`, `"plexos"`, `"pypsa"`, `"pandapower"`).  Unset (default)
  /// preserves the pre-feature behaviour: every alias is accepted on
  /// input, every key is emitted in canonical form on output.
  ///
  /// Settable via `--naming-dialect <name>` or
  /// `--set model_options.naming_dialect=<name>`.
  OptName naming_dialect {};

  /// Objective formulation switch (issue #519).
  ///
  ///   * ``"cost"`` (default, or unset) — minimize the standard
  ///     dispatch cost: Σ (generator gcost + fuel·heat_rate·gen +
  ///     demand_fail·fail + line_overload·slack + … etc.).
  ///   * ``"emissions"`` — minimize Σ (emission_rate × generation),
  ///     with every other cost coefficient zeroed.  By LP duality,
  ///     the resulting ``Reservoir/water_value_dual`` and
  ///     ``Battery/energy_dual`` parquets then carry the carbon
  ///     opportunity cost of stored water / energy (tCO2eq per
  ///     MWh-equivalent) directly — the theoretically correct
  ///     answer to the "storage marginal emission" question that
  ///     ``gtopt_marginal_units`` currently approximates with a
  ///     merit-ladder consequential-MOER heuristic.  Used as the
  ///     second pass of the two-solve workflow orchestrated by
  ///     ``scripts/gtopt_emission_dual/`` (see issue #519).
  ///
  /// LP shape is identical between modes; only objective
  /// coefficients change.  See ``GeneratorLP::add_to_lp`` for the
  /// per-mode coefficient swap.
  ///
  /// Settable via ``--objective-mode <name>`` or
  /// ``--set model_options.objective_mode=<name>``.
  OptName objective_mode {};

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

  /// Assembly-time elimination of provably-zero LP columns/rows at the
  /// SOURCE element (e.g. the generator skips its generation column when
  /// `pmax == pmin == 0`; the reservoir skips a zero extraction column;
  /// reserve/inertia provisions skip a zero-ceiling column; the inertia
  /// zone skips a zero-requirement row).  Default `false`.
  ///
  /// DEFAULT OFF — the un-reduced LP is the honest model and a modern
  /// solver's presolve (CPLEX / Gurobi) reduces it to the same core, often
  /// marginally faster (PLEXOS 20260517: CPLEX presolve takes both LPs to
  /// ~555k×820k; the un-reduced solve is ~12 s faster).  Enable with
  /// `--lp-reduction` (or `model_options.lp_reduction = true`) for the
  /// weak-presolve backends (CLP / CBC / HiGHS) that benefit from the
  /// smaller LP up front — they will not otherwise strip ~575k provably
  /// zero columns, and a serial-barrier solver (HiGHS IPM) factorizes the
  /// bigger matrix on one core.
  ///
  /// NOT strictly optimum-neutral: the reservoir / provision / requirement
  /// skips are pure `[0,0]` columns or `Σ≥0` rows and never move the
  /// optimum, but the GENERATOR skip cascades into dropping a commitment
  /// `u/v/w` for the pmax==pmin==0 unit — a real `[0,1]` binary that lets
  /// the unit be committed (must-run noload / synchronous-condenser
  /// inertia).  Enabling reduction therefore encodes "no generation column
  /// ⟹ no commitment ⟹ no ancillary participation" and can shift the
  /// optimum slightly (+$20k / 0.004% on 20260517, ~94% of it a
  /// demand-failure / reserve-shortage penalty slack).  Leaving it OFF
  /// keeps the faithful model.  Consumers never need this flag — the
  /// switch lives ONLY at the source.
  OptBool lp_reduction {};

  /// Relax the per-bus power-balance equality (`Σ generation·(1−lf) ±
  /// flows = Σ demand·(1+lf)`) to a `≥` inequality (`generation ≥
  /// demand`), i.e. free disposal of surplus: over-generation is allowed
  /// and the excess is discarded at zero cost.  Default `false` (strict
  /// balance).
  ///
  /// Reserve and inertia PRODUCTION are already `≥ requirement`
  /// (`reserve_zone_lp` / `inertia_zone_lp` emit `greater_equal` rows), so
  /// this flag only affects the bus balance.  Water balances, KVL, and
  /// commitment-logic equalities are NOT relaxed (they are physical /
  /// structural, not supply-demand balances).
  ///
  /// Marginal-theory note: with `≥`, a bus dual is `≥ 0` (over-supplied
  /// buses price at 0), removing the negative-LMP artefacts that a strict
  /// `=` balance produces under must-run/ancillary over-production.  Use
  /// with care — it changes the reported LMPs and lets the model spill
  /// generation for free.  `--set model_options.allow_oversupply=true`.
  OptBool allow_oversupply {};

  void merge(const ModelOptions& opts)
  {
    merge_opt(use_single_bus, opts.use_single_bus);
    merge_opt(use_kirchhoff, opts.use_kirchhoff);
    merge_opt(kirchhoff_mode, opts.kirchhoff_mode);
    merge_opt(use_line_losses, opts.use_line_losses);
    merge_opt(line_losses_mode, opts.line_losses_mode);
    merge_opt(kirchhoff_threshold, opts.kirchhoff_threshold);
    merge_opt(dc_line_reactance_threshold, opts.dc_line_reactance_threshold);
    merge_opt(dc_line_resistance_threshold, opts.dc_line_resistance_threshold);
    merge_opt(loss_segments, opts.loss_segments);
    merge_opt(scale_objective, opts.scale_objective);
    merge_opt(scale_theta, opts.scale_theta);
    merge_opt(scale_loss_link, opts.scale_loss_link);
    merge_opt(loss_cost_eps, opts.loss_cost_eps);
    merge_opt(loss_secant_segments, opts.loss_secant_segments);
    merge_opt(loss_use_sos2, opts.loss_use_sos2);
    merge_opt(theta_max, opts.theta_max);
    merge_opt(auto_scale, opts.auto_scale);
    merge_opt(demand_fail_cost, opts.demand_fail_cost);
    merge_opt(reserve_shortage_cost, opts.reserve_shortage_cost);
    merge_opt(hydro_spill_cost, opts.hydro_spill_cost);
    merge_opt(hydro_use_value, opts.hydro_use_value);
    merge_opt(state_violation_cost, opts.state_violation_cost);
    merge_opt(demand_fail_rhs_shift, opts.demand_fail_rhs_shift);
    merge_opt(continuous_phases, opts.continuous_phases);
    merge_opt(naming_dialect, opts.naming_dialect);
    merge_opt(objective_mode, opts.objective_mode);
    merge_opt(strict_storage_emin, opts.strict_storage_emin);
    merge_opt(lp_reduction, opts.lp_reduction);
    merge_opt(allow_oversupply, opts.allow_oversupply);
  }

  /// True if any field is set.
  [[nodiscard]] bool has_any() const noexcept
  {
    return use_single_bus.has_value() || use_kirchhoff.has_value()
        || kirchhoff_mode.has_value() || use_line_losses.has_value()
        || line_losses_mode.has_value() || kirchhoff_threshold.has_value()
        || dc_line_reactance_threshold.has_value()
        || dc_line_resistance_threshold.has_value() || loss_segments.has_value()
        || scale_objective.has_value() || scale_theta.has_value()
        || scale_loss_link.has_value() || loss_cost_eps.has_value()
        || loss_secant_segments.has_value() || loss_use_sos2.has_value()
        || theta_max.has_value() || demand_fail_cost.has_value()
        || reserve_shortage_cost.has_value() || hydro_spill_cost.has_value()
        || hydro_use_value.has_value() || state_violation_cost.has_value()
        || demand_fail_rhs_shift.has_value() || continuous_phases.has_value()
        || naming_dialect.has_value() || objective_mode.has_value()
        || strict_storage_emin.has_value() || lp_reduction.has_value()
        || allow_oversupply.has_value();
  }

  /// True iff every field set in `other` has an equal value in `*this`.
  /// Fields that `other` leaves unset are ignored.  Semantically: "applying
  /// `other` as an override on top of `*this` would not change anything".
  [[nodiscard]] bool covers(const ModelOptions& other) const
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
        && covers_opt(dc_line_resistance_threshold,
                      other.dc_line_resistance_threshold)
        && covers_opt(loss_segments, other.loss_segments)
        && covers_opt(scale_objective, other.scale_objective)
        && covers_opt(scale_theta, other.scale_theta)
        && covers_opt(scale_loss_link, other.scale_loss_link)
        && covers_opt(loss_cost_eps, other.loss_cost_eps)
        && covers_opt(loss_secant_segments, other.loss_secant_segments)
        && covers_opt(loss_use_sos2, other.loss_use_sos2)
        && covers_opt(theta_max, other.theta_max)
        && covers_opt(demand_fail_cost, other.demand_fail_cost)
        && covers_opt(reserve_shortage_cost, other.reserve_shortage_cost)
        && covers_opt(hydro_spill_cost, other.hydro_spill_cost)
        && covers_opt(hydro_use_value, other.hydro_use_value)
        && covers_opt(state_violation_cost, other.state_violation_cost)
        && covers_opt(demand_fail_rhs_shift, other.demand_fail_rhs_shift)
        && covers_opt(continuous_phases, other.continuous_phases)
        && covers_opt(naming_dialect, other.naming_dialect)
        && covers_opt(objective_mode, other.objective_mode)
        && covers_opt(strict_storage_emin, other.strict_storage_emin)
        && covers_opt(lp_reduction, other.lp_reduction)
        && covers_opt(allow_oversupply, other.allow_oversupply);
  }
};

}  // namespace gtopt
