/**
 * @file      line_lp.hpp
 * @brief     LP formulation for transmission lines
 * @date      Sat Mar 29 19:02:33 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the LineLP class which formulates transmission line
 * constraints for the LP model.  Loss modeling is delegated to the modular
 * losses engine in line_losses.hpp, which supports multiple modes:
 * none, linear, piecewise, bidirectional, adaptive, and dynamic.
 *
 * @see line_losses.hpp for the loss model implementations.
 * @see line_enums.hpp  for LineLossesMode enum documentation.
 */

#pragma once

#include <vector>

#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/line.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

class LineLP;

/// SingleId-style FK alias for ``LineLP``.  Used by elements that
/// reference a Line through ``ElementIndex`` (e.g. ``LineCommitmentLP``
/// per issue #509).
using LineLPSId = ObjectSingleId<LineLP>;

class LineLP : public CapacityObjectLP<Line>
{
public:
  static constexpr std::string_view FlowName {
      "flow"};  ///< compound signed primal: `flowp − flown`
  /// Excluded-direction stems (extras gate): ``flown_sol`` is the raw
  /// negative-direction primal (always ≥ 0; only when the LP has two
  /// directional columns), and ``flowe_cost`` is the EXCLUDED reduced
  /// cost — the rc that the sign-based ``flow_cost`` rule did NOT
  /// pick.  ``flowe`` (not ``flown``) for the rc so the naming
  /// signals "excluded", not "negative direction".
  static constexpr std::string_view FlownName {"flown"};
  static constexpr std::string_view FloweName {"flowe"};
  /// Internal-only LP variable names used during ``add_to_lp`` (not
  /// emitted in default output stems; consumers read ``flow_sol`` /
  /// ``flow_cost`` / ``flown_sol`` / ``flowe_cost`` instead).
  static constexpr std::string_view FlowpName {"flowp"};
  /// Signed flow column emitted by `LineLossesMode::tangent_signed_flow`
  /// (Coffrin outer approximation; one column per (line, block) covering
  /// `[−tmax_ba, +tmax_ab]`).  Distinct from `FlowpName` / `FlownName`
  /// so AMPL / output consumers can opt in to the signed shape when
  /// they want it; existing tools keep reading `flowp_sol` / `flown_sol`
  /// (derived `max(0, f)` / `max(0, −f)` are emitted for back-compat).
  static constexpr std::string_view FlowsName {"flows"};
  /// Auxiliary `|f|`-envelope column emitted by
  /// `LineLossesMode::tangent_signed_flow` to tighten the upper bound on
  /// the loss column.  Bounded `[0, fmax]` and tied to the signed flow
  /// column `f` via two rows `v ≥ +f` and `v ≥ −f` (so `v ≥ |f|` at the
  /// optimum).  Replaces the loose constant loss UB `ℓ ≤ R·fmax²/V²`
  /// with the linear-in-`v` chord `ℓ ≤ (R·fmax/V²)·v`, which is the
  /// secant chord of the convex quadratic `R·v²/V²` on `[0, fmax]`.
  /// See `add_tangent_signed_flow` for the math + derivation.
  static constexpr std::string_view FlowAbsName {"flow_abs"};
  /// Lambda-form SOS2 L-secant breakpoint weight col (issue #504 SOS2
  /// path).  ``2L+1`` cols per (line, block) emitted when
  /// ``tangent_signed_flow`` is paired with ``loss_use_sos2 = true``
  /// and ``loss_secant_segments > 1``.  Indexed ``0..2L`` so each
  /// label encodes the breakpoint position ``b_l = (l − L) · w``
  /// on ``[−fmax, +fmax]``.
  static constexpr std::string_view FlowLambdaName {"flow_lambda"};
  static constexpr std::string_view LosspName {"lossp"};
  static constexpr std::string_view LossnName {"lossn"};
  /// Consolidated per-(line, block) loss output emitted as
  /// ``Line/loss_sol.parquet`` under the ``solution`` write-out gate
  /// (NOT ``extras`` — a previous version of this note was stale; the
  /// emitter is the ``solution``-gated ``add_col_sol`` /
  /// ``add_col_sol_values`` path).  For the PWL modes
  /// (``piecewise`` / ``bidirectional`` / ``adaptive``) it equals
  /// ``LP(lossp) + LP(lossn)`` per cell — at most one of the two
  /// direction-specific cols is populated on any given block, so the
  /// sum is the total dissipated energy on that line that block.  For
  /// ``piecewise_direct`` (no explicit loss column) it is reconstructed
  /// as ``Σ_k lf_k · seg_k_sol`` summed over both directional segment
  /// sets (see ``add_to_output``).  Replaces the paired ``lossp_sol`` /
  /// ``lossn_sol`` emission so downstream consumers see a single,
  /// schema-stable loss stream regardless of which direction the LP
  /// routed flow.
  static constexpr std::string_view LossName {"loss"};
  static constexpr std::string_view CapacitypName {"capacityp"};
  static constexpr std::string_view CapacitynName {"capacityn"};
  static constexpr std::string_view ThetaName {"theta"};
  /// Overload-slack column emitted only when ``Line.tmax_normal_ab`` and
  /// ``Line.overload_penalty`` are configured.  Equals ``max(0, flowp −
  /// tmax_normal_ab)`` and is bounded by ``tmax_ab − tmax_normal_ab``.
  static constexpr std::string_view OverloadpName {"overloadp"};
  /// B→A counterpart of ``overloadp``.
  static constexpr std::string_view OverloadnName {"overloadn"};
  /// Filter metadata keys published by `add_to_lp` for `sum(...)`
  /// predicate matching.
  static constexpr std::string_view TypeKey {"type"};
  static constexpr std::string_view BusAKey {"bus_a"};
  static constexpr std::string_view BusBKey {"bus_b"};

  using CapacityBase = CapacityObjectLP<Line>;

  /**
   * @brief Returns the underlying Line object with proper forwarding semantics
   * @param self The calling object (implicit)
   * @return Reference to the Line object with same value category and
   * const-ness as self
   */
  [[nodiscard]] constexpr auto&& line(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return self.object();
  }

  [[nodiscard]] constexpr auto bus_a_sid() const noexcept -> BusLPSId
  {
    return BusLPSId {line().bus_a};
  }

  [[nodiscard]] constexpr auto bus_b_sid() const noexcept -> BusLPSId
  {
    return BusLPSId {line().bus_b};
  }

  [[nodiscard]] constexpr bool is_loop() const
  {
    return line().bus_a == line().bus_b;
  }

  explicit LineLP(const Line& pline, const InputContext& ic);

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  /// All four flow-col accessors below tolerate a missing
  /// `(scenario, stage)` outer key by returning a reference to a
  /// static empty BIndexHolder via `find_or_empty_inner`.
  /// Pre-2026-05-14 the assignment in ``add_to_lp`` was unconditional
  /// even when the inner map was empty (e.g. `flowp_seg_cols` for
  /// non-direct modes, `flowp_cols` for `piecewise_direct`), keeping
  /// the outer key present.  The new conditional-assignment pattern
  /// only inserts the outer key when at least one block populated
  /// the inner map, so callers must handle the missing-key case —
  /// these helpers centralise it.
  [[nodiscard]] constexpr const auto& flowp_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return find_or_empty_inner(flowp_cols, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& flown_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return find_or_empty_inner(flown_cols, scenario, stage);
  }

  /// Per-block signed-flow column map (populated only under
  /// `LineLossesMode::tangent_signed_flow`).  When present, the
  /// `flowp_cols` / `flown_cols` maps are empty for this `(scenario,
  /// stage)` and KVL stamps `+x_τ` on each signed flow column.  Same
  /// missing-key tolerance as the other flow accessors.
  [[nodiscard]] constexpr const auto& flows_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return find_or_empty_inner(flows_cols, scenario, stage);
  }

  /// Per-block segment columns for the A→B direction (only populated
  /// in `piecewise_direct` line-loss mode; empty otherwise).  Returns
  /// the per-block map; each block maps to a `std::vector<ColIndex>`
  /// of segment cols.  Used by `kirchhoff::cycle_basis::add_kvl_rows`
  /// to stamp segments directly into per-cycle KVL rows when the
  /// flowp aggregator was elided.
  [[nodiscard]] constexpr const auto& flowp_seg_cols_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return find_or_empty_inner(flowp_seg_cols, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& flown_seg_cols_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return find_or_empty_inner(flown_seg_cols, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& lossp_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return find_or_empty_inner(lossp_cols, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& lossn_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return find_or_empty_inner(lossn_cols, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& capacityp_rows_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return find_or_empty_inner(capacityp_rows, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& capacityn_rows_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return find_or_empty_inner(capacityn_rows, scenario, stage);
  }

  /// Overload-slack column accessors.  Return a reference to a static
  /// empty BIndexHolder when the soft-cap feature is not active for
  /// this `(scenario, stage)` (no `tmax_normal_*` + positive
  /// `overload_penalty` resolved here).  Mirrors the missing-key
  /// tolerance of the flow accessors above so tests / PAMPL consumers
  /// can blindly query without branching.
  [[nodiscard]] constexpr const auto& overloadp_cols_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return find_or_empty_inner(overloadp_cols, scenario, stage);
  }

  [[nodiscard]] constexpr const auto& overloadn_cols_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return find_or_empty_inner(overloadn_cols, scenario, stage);
  }

  /// Check if this line created Kirchhoff (theta) rows for a given
  /// (scenario, stage) pair.
  [[nodiscard]] constexpr bool has_theta_rows(
      const std::pair<ScenarioUid, StageUid>& st_key) const
  {
    return theta_rows.contains(st_key) && !theta_rows.at(st_key).empty();
  }

  /// Per-(scenario, stage) Kirchhoff KVL row indices.  Returns the
  /// inner per-block map for the ``(scenario, stage)`` cell, or a
  /// static empty map when this line has no KVL rows for that cell
  /// (radial network, ``node_angle`` mode skipped, etc.).  Read by
  /// ``LineCommitmentLP`` (issue #509) when the OTS big-M
  /// disjunction rewrite needs to mutate the existing equality row.
  [[nodiscard]] constexpr const auto& theta_rows_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return find_or_empty_inner(theta_rows, scenario, stage);
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_tmax_ab(StageUid s, BlockUid b) const
  {
    return tmax_ab.at(s, b);
  }
  [[nodiscard]] auto param_tmax_ba(StageUid s, BlockUid b) const
  {
    return tmax_ba.at(s, b);
  }
  [[nodiscard]] auto param_tcost(StageUid s, BlockUid b) const
  {
    return tcost.at(s, b);
  }
  [[nodiscard]] auto param_reactance(StageUid s) const
  {
    return reactance.at(s);
  }
  [[nodiscard]] auto param_voltage(StageUid s) const { return voltage.at(s); }
  [[nodiscard]] auto param_tap_ratio(StageUid s) const
  {
    return tap_ratio.at(s);
  }
  [[nodiscard]] auto param_phase_shift_deg(StageUid s) const
  {
    return phase_shift_deg.at(s);
  }
  [[nodiscard]] auto param_lossfactor(StageUid s, BlockUid b) const
  {
    return lossfactor.at(s, b);
  }
  [[nodiscard]] auto param_resistance(StageUid s) const
  {
    return resistance.at(s);
  }
  [[nodiscard]] auto param_loss_envelope(StageUid s) const
  {
    return loss_envelope.at(s);
  }
  /// @}

private:
  OptTBRealSched tmax_ba;
  OptTBRealSched tmax_ab;
  /// Per-(stage, block) in-service flag (PLEXOS ``Line.Units``).  When a
  /// block resolves to ``0`` the line is open for that block: no flow
  /// column, capacity row, loss segment, balance contribution, or KVL
  /// row is emitted.  Absent ⇒ in service in every block.
  OptTBIntBoolSched in_service;
  OptTBRealSched tmax_normal_ba;
  OptTBRealSched tmax_normal_ab;
  OptTBRealSched tcost;
  OptTBRealSched overload_penalty;
  OptTBRealSched lossfactor;
  OptTRealSched reactance;
  OptTRealSched voltage;
  OptTRealSched resistance;
  OptTRealSched loss_envelope;
  OptTRealSched tap_ratio;
  OptTRealSched phase_shift_deg;

  STBIndexHolder<ColIndex> flowp_cols;
  STBIndexHolder<ColIndex> flown_cols;
  STBIndexHolder<ColIndex> flows_cols;  ///< signed flow (tangent_signed_flow)
  STBIndexHolder<std::vector<ColIndex>> flowp_seg_cols;
  STBIndexHolder<std::vector<ColIndex>> flown_seg_cols;
  /// Per-segment physical loss factors `lf_k` parallel to
  /// `flowp_seg_cols` / `flown_seg_cols` (same `(scenario, stage) →
  /// (buid → vector)` keying and same per-segment order).  Populated
  /// only under `LineLossesMode::piecewise_direct`, which has no
  /// explicit loss LP column.  `add_to_output` reconstructs the exact
  /// LP-consistent loss `Σ_k lf_k · primal(seg_col_k)` summed over BOTH
  /// directional segment sets, capturing bidirectional loss-arbitrage
  /// flow (which a `R·f²/V²`-at-net-flow approximation would hide).
  STBIndexHolder<std::vector<double>> flowp_seg_loss;
  STBIndexHolder<std::vector<double>> flown_seg_loss;
  STBIndexHolder<ColIndex> lossp_cols;
  STBIndexHolder<ColIndex> lossn_cols;
  STBIndexHolder<ColIndex> overloadp_cols;
  STBIndexHolder<ColIndex> overloadn_cols;
  STBIndexHolder<RowIndex> capacityp_rows;
  STBIndexHolder<RowIndex> capacityn_rows;
  STBIndexHolder<RowIndex> overloadp_rows;
  STBIndexHolder<RowIndex> overloadn_rows;

  STBIndexHolder<RowIndex> theta_rows;
};

// Pin the data-struct constant value so an accidental rename of the
// `Line::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Line"`).
static_assert(LineLP::Element::class_name == LPClassName {"Line"},
              "Line::class_name must remain \"Line\"");

}  // namespace gtopt
