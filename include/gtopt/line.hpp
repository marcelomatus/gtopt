/**
 * @file      line.hpp
 * @brief     Header for transmission line components
 * @date      Sun Apr 20 02:12:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing AC transmission lines
 * in power system planning models. In DC power-flow (Kirchhoff) mode the
 * reactance determines the flow split between parallel paths. The line also
 * supports capacity-expansion planning.
 *
 * ### JSON Example (transmission line)
 * ```json
 * {
 *   "uid": 1,
 *   "name": "l1_4",
 *   "bus_a": "b1",
 *   "bus_b": "b4",
 *   "reactance": 0.0576,
 *   "tmax_ab": 250,
 *   "tmax_ba": 250
 * }
 * ```
 *
 * ### JSON Example (phase-shifting transformer)
 * ```json
 * {
 *   "uid": 5,
 *   "name": "pst_1_2",
 *   "bus_a": 1,
 *   "bus_b": 2,
 *   "reactance": 0.05,
 *   "type": "transformer",
 *   "tap_ratio": 1.02,
 *   "phase_shift_deg": -5.0,
 *   "tmax_ab": 300,
 *   "tmax_ba": 300
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 2-D inline array indexed by `[stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Line/`
 */

#pragma once

#include <gtopt/capacity.hpp>
#include <gtopt/line_enums.hpp>
#include <gtopt/lp_class_name.hpp>

namespace gtopt
{

/**
 * @struct Line
 * @brief Represents a transmission line (branch) connecting two buses
 *
 * A line carries active power flow `f ∈ [-tmax_ba, tmax_ab]` between
 * `bus_a` and `bus_b`. In Kirchhoff mode the flow is constrained by
 * `f = V² / X · (θ_a − θ_b)` where V is the line voltage and X the
 * reactance.  When voltage is omitted (defaults to 1.0) both V and X
 * are in per-unit; when voltage is in kV, reactance must be in Ω.
 * Optional expansion variables allow the solver to invest in additional
 * transfer capacity.
 *
 * @see Bus for connected bus definitions
 * @see LineLP for the LP formulation
 */
struct Line
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `LineLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Line::class_name` directly (or
  /// `LineLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Line"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Line name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional line type tag (e.g. "ac", "dc", "transformer")
  OptName description {};  ///< Optional free-text description (e.g. conversion
                           ///< provenance)

  SingleId bus_a {unknown_uid};  ///< Sending-end (from) bus ID
  SingleId bus_b {unknown_uid};  ///< Receiving-end (to) bus ID

  OptTRealFieldSched voltage {};  ///< Nominal voltage level [kV].
                                  ///< Omit or set to 1.0 for per-unit mode.
  OptTRealFieldSched resistance {};  ///< Series resistance [Ω].
                                     ///< Use p.u. when voltage is omitted.
  OptTRealFieldSched reactance {};  ///< Series reactance for DC power flow [Ω].
                                    ///< Use p.u. when voltage is omitted
                                    ///< (default). Susceptance: B = V² / X.
  OptTBRealFieldSched lossfactor {};  ///< Lumped loss factor [p.u.]
                                      ///< per-(stage, block).
  OptBool use_line_losses {};  ///< @deprecated Use `line_losses_mode` instead.
                               ///< Kept for backward compatibility:
                               ///< `true` maps to the global default mode,
                               ///< `false` maps to `none`.
  OptName
      line_losses_mode {};  ///< Loss model selection (per-line override).
                            ///< See LineLossesMode for valid values:
                            ///< `"none"`, `"linear"`, `"piecewise"`,
                            ///< `"bidirectional"`, `"adaptive"`, `"dynamic"`.
                            ///< When unset, inherits from ModelOptions.
  OptInt loss_segments {};  ///< Number of piecewise-linear segments for
                            ///< quadratic losses (default: from Options)

  OptName loss_pwl_layout {};  ///< Segment-layout strategy for the PWL
                               ///< loss approximation.  See LinePwlLayout
                               ///< for valid values: `"uniform"` (default,
                               ///< equal-width secant chords), `"equal_error"`
                               ///< (√-spaced minimax), `"tangent"` (outer
                               ///< approximation, future).  Active only when
                               ///< `line_losses_mode` selects a PWL flavour.
                               ///< When unset, defaults to `uniform` —
                               ///< preserving pre-2026-05 behaviour.

  OptName loss_allocation_mode {};  ///< How losses are allocated between
                                    ///< sender and receiver buses:
                                    ///< `"receiver"` (default), `"sender"`,
                                    ///< or `"split"` (50/50, PLP default).

  OptTRealFieldSched loss_envelope {};  ///< Upper envelope [MW] over which the
                                        ///< K loss-PWL segments are spread.
                                        ///< DECOUPLED from the flow cap
                                        ///< (`tmax_ab`/`tmax_ba`): set this to
                                        ///< the ORIGINAL line rating when the
                                        ///< cap is inflated by a soft-cap /
                                        ///< `enforce_level` lift, so the
                                        ///< segments concentrate over the
                                        ///< realistic loading range instead of
                                        ///< the lifted cap.  When unset
                                        ///< (default), the envelope falls back
                                        ///< to `fmax = max(tmax_ab, tmax_ba)`
                                        ///< (backward compatible).  Flow past
                                        ///< the envelope extrapolates on the
                                        ///< last segment's slope.

  /// Parse line_losses_mode string to enum (nullopt = inherit from global).
  [[nodiscard]] constexpr std::optional<LineLossesMode> line_losses_mode_enum()
      const noexcept
  {
    if (line_losses_mode.has_value()) {
      return enum_from_name<LineLossesMode>(*line_losses_mode);
    }
    return std::nullopt;
  }

  /// Parse loss_pwl_layout string to enum (defaults to `uniform`).
  [[nodiscard]] constexpr LinePwlLayout loss_pwl_layout_enum() const noexcept
  {
    if (loss_pwl_layout.has_value()) {
      return enum_from_name<LinePwlLayout>(*loss_pwl_layout)
          .value_or(LinePwlLayout::uniform);
    }
    return LinePwlLayout::uniform;
  }

  /// Parse loss_allocation_mode string to enum (receiver if unset).
  [[nodiscard]] constexpr LossAllocationMode loss_allocation_mode_enum()
      const noexcept
  {
    if (loss_allocation_mode.has_value()) {
      return enum_from_name<LossAllocationMode>(*loss_allocation_mode)
          .value_or(LossAllocationMode::receiver);
    }
    return LossAllocationMode::receiver;
  }

  OptTBRealFieldSched tmax_ba {};  ///< Maximum power flow in B→A direction [MW]
  OptTBRealFieldSched tmax_ab {};  ///< Maximum power flow in A→B direction [MW]
  /// Per-(stage, block) in-service flag mirroring PLEXOS ``Line.Units``
  /// (the ``Lin_Units.csv`` DataFile): ``1`` = line in service, ``0`` =
  /// line out (maintenance / forced outage) for that block.  Unlike the
  /// per-stage ``active`` element-activation flag, this resolves at block
  /// granularity so an intra-stage maintenance window can be modelled.
  /// When a block resolves to ``0`` the LP emits **no** flow column,
  /// capacity row, loss segment, balance contribution, or KVL angle row
  /// for that block — the line is a true open circuit and its two buses
  /// are decoupled.  Absent (default) = in service in every block.
  OptTBIntBoolFieldSched in_service {};
  /// Thermal-limit enforcement mode, mirroring PLEXOS's
  /// ``Line.Enforce Limits`` property:
  ///
  ///   * ``0`` — never enforce.  The LP does NOT add the hard
  ///     ``|flow| <= tmax_{ab,ba}`` bound; ``tmax_ab`` / ``tmax_ba``
  ///     are carried purely for **loss-segment discretization** (the
  ///     piecewise-linear loss approximation needs an upper envelope
  ///     to pick segment widths; without it
  ///     ``seg_width = DblMax / nseg`` produces meaningless
  ///     segments).  Use this for lines that PLEXOS labels EL=0
  ///     ("Never enforce") and for selectively-lifted EL=1 lines
  ///     whose flow physically exceeds the rating (e.g.
  ///     Capricornio110→LaNegra110 on the CEN PCP weekly bundle,
  ///     where AC voltage support requires ~200 MW vs the 76 MW
  ///     steady-state cap).
  ///   * ``1`` — voltage-conditional (PLEXOS).  In our LP we treat
  ///     this identically to level ``2`` (hard cap), because we have
  ///     no AC voltage-feasibility iteration to consult.  PLEXOS
  ///     empirically enforces EL=1 caps in its economic dispatch on
  ///     the CEN PCP weekly bundle (88 of 89 EL=1 lines with flow
  ///     stay at or below cap; the one outlier — Capricornio — is
  ///     the line we lift to ``0`` explicitly).
  ///   * ``2`` — always enforce (the historical hard-cap behaviour
  ///     and the schema default).
  ///
  /// Default = ``2``.
  OptInt enforce_level {};
  OptTBRealFieldSched tcost {};  ///< Variable transmission cost [$/MWh]
                                 ///< — per-(stage, block).  Accepts a
                                 ///< scalar (broadcasts), a 2-D nested
                                 ///< array, or a file-backed schedule.

  /// Small positive cost per MWh of dissipated loss, breaks LP-relax
  /// bidirectional-flow degeneracy.
  ///
  /// In ``piecewise`` / ``bidirectional`` modes the LP has a per-direction
  /// loss column (``loss_p`` / ``loss_n``).  For any required net flow
  /// ``f = fp − fn ≥ 0`` the convex PWL of the loss curve is strictly
  /// minimised at ``fn = 0`` (single-direction dispatch).  Adding a tiny
  /// positive cost ``ε`` on the loss columns makes the LP STRICTLY pick
  /// that single-direction optimum, eliminating LP-degeneracy phantom
  /// bidirectional flow without SOS1, MIP, or binaries.
  ///
  /// Recommended value: ``1e-6`` $/MWh — essentially zero objective
  /// impact (well below LP optimality tolerance, ~1e-9 relative) yet
  /// strictly breaks the degeneracy.  Default ``0.0`` (unset)
  /// preserves legacy behaviour.  Per-line override beats the global
  /// ``model_options.loss_cost_eps`` default.  Inert for ``none``,
  /// ``linear`` and ``piecewise_direct`` — those have no loss column.
  ///
  /// SIZING for negative-price exposure: ε lands on the LOSS columns
  /// in every mode — in ``tangent_signed_flow`` the abs-flow proxy
  /// ``v`` carries only an internal 1e-6 degeneracy pin (activated
  /// when ε > 0) — so its incidence is loss-scaled: overhead
  /// ≈ ε·2λ per marginal MWh (λ = R·fmax/V², a few %), NOT a flow
  /// toll.  Loss dumping earns |π_a + π_b|/2 per MWh whenever the
  /// bus-dual pair-sum goes negative (KVL congestion or curtailment
  /// produce this with all-positive costs), hence
  ///   ε ≥ ½·|worst credible pair-sum| (+ margin)
  /// closes the arbitrage on the guarded line — typical per-line
  /// values 1–20 $/MWh cost only centavos of LMP distortion.  Set it
  /// per line on flagged corridors and RE-RUN: the sink migrates to
  /// the next unguarded lossy line, iterate until no new flags.
  /// ``linear`` / ``piecewise_direct`` cannot be ε-guarded (no loss
  /// column).  The exact fix remains the SOS2 λ-form
  /// (``loss_use_sos2`` + ``loss_secant_segments ≥ 2``).  See
  /// ``test_line_losses_negative_lmp_kvl.cpp``.
  OptReal loss_cost_eps {};

  /// Soft (normal) flow threshold in the B→A direction [MW].  When set
  /// and `overload_penalty > 0`, the LP allows `flown` up to `tmax_ba`
  /// (the hard cap, unchanged) but adds a per-MWh cost
  /// `overload_penalty × max(0, flown − tmax_normal_ba)` to the
  /// objective.  Mirrors UC.jl's `Normal flow limit (MW)` and PLEXOS's
  /// `Normal Rating` paired with an overload penalty.  When unset (or
  /// `>= tmax_ba`) the LP falls back to the pure hard-cap behavior.
  OptTBRealFieldSched tmax_normal_ba {};
  /// Soft (normal) flow threshold in the A→B direction [MW].  Same
  /// semantics as `tmax_normal_ba` for the A→B leg.
  OptTBRealFieldSched tmax_normal_ab {};
  /// Overload penalty above the soft thresholds [$/MWh] —
  /// per-(stage, block).  Accepts a scalar (broadcasts), a 2-D nested
  /// array ``[[block, …], …]``, or a file-backed schedule.  Per-block
  /// duration is factored in by the LP build (matches the convention
  /// of other cost coefficients).  When zero or unset, no slack
  /// columns are emitted and ``tmax_normal_*`` are ignored.
  OptTBRealFieldSched overload_penalty {};

  OptTRealFieldSched capacity {};  ///< Installed transfer capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched
      capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable

  /// Off-nominal tap ratio [p.u.].  For transformers only; ignored for
  /// plain lines.  When set to a value other than 1.0 the effective
  /// susceptance of the branch in the DC power-flow (Kirchhoff) constraint
  /// is scaled by `1/tap_ratio`, i.e. `B_eff = V²/(tap_ratio · X)`.
  /// Defaults to 1.0 (nominal tap, no correction).
  OptTRealFieldSched tap_ratio {};

  /// Phase-shift angle [degrees].  Models a phase-shifting transformer
  /// (PST) by adding a constant angle offset φ to the Kirchhoff equality
  /// constraint: `f = B_eff · (θ_a − θ_b − φ)`.  Positive values reduce
  /// the natural power flow from bus_a to bus_b.  Defaults to 0.0.
  OptTRealFieldSched phase_shift_deg {};

  /// Number of L-secant segments for the ``tangent_signed_flow`` loss-
  /// upper-bound chord (issue #504).  When ``L > 1`` the single
  /// ``v ≥ |f|`` auxiliary column is replaced by ``L`` segment columns
  /// ``v_l ∈ [0, envelope/L]`` and the chord upper bound becomes the
  /// piecewise sum ``ℓ ≤ Σ chord_slope_l · v_l`` with per-segment slope
  /// ``(R/V²) · (envelope/L) · (2l − 1)``.  Combined with
  /// ``loss_use_sos2`` this yields a tight piecewise-linear upper bound
  /// on the convex quadratic loss curve, lowering worst-case
  /// overstatement from ``c·envelope²/4`` (L=1 secant) to
  /// ``c·envelope²/(4·L²)``.
  ///
  /// Per-line override beats ``model_options.loss_secant_segments``.
  /// Unset → ``1`` (single-secant chord, current production behaviour).
  /// Inert outside ``tangent_signed_flow`` mode.
  OptInt loss_secant_segments {};

  /// Enable SOS2 enforcement on the ``L`` secant-segment columns
  /// emitted when ``loss_secant_segments > 1`` (issue #504).  Without
  /// SOS2 the LP exploits the segment freedom to maximise the chord
  /// ceiling; with SOS2 at most two consecutive ``v_l`` are non-zero,
  /// forcing fill order ``v_1`` → ``v_2`` → … → ``v_L`` and making
  /// the chord piecewise-tight at every breakpoint ``b_l = l·w``.
  ///
  /// REQUIRES a MIP-capable LP backend (CPLEX, Gurobi, HiGHS ≥ 1.6);
  /// gtopt's CBC/CLP plugins do not currently expose SOS2 — the LP
  /// build raises a structured error if SOS2 is requested with an
  /// unsupporting backend so the misconfig surfaces at build time.
  ///
  /// Per-line override beats ``model_options.loss_use_sos2``.
  /// Unset → false (no SOS2 declaration; behaviour identical to the
  /// pre-#504 single-secant chord).  Inert outside
  /// ``tangent_signed_flow`` mode or when ``loss_secant_segments ≤ 1``.
  OptBool loss_use_sos2 {};
};

}  // namespace gtopt
