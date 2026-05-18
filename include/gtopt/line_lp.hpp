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

class LineLP : public CapacityObjectLP<Line>
{
public:
  static constexpr std::string_view FlowName {
      "flow"};  ///< compound: `flowp − flown`
  static constexpr std::string_view FlowpName {"flowp"};
  static constexpr std::string_view FlownName {"flown"};
  static constexpr std::string_view LosspName {"lossp"};
  static constexpr std::string_view LossnName {"lossn"};
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
  [[nodiscard]] auto param_lossfactor(StageUid s) const
  {
    return lossfactor.at(s);
  }
  [[nodiscard]] auto param_resistance(StageUid s) const
  {
    return resistance.at(s);
  }
  /// @}

private:
  OptTBRealSched tmax_ba;
  OptTBRealSched tmax_ab;
  OptTBRealSched tmax_normal_ba;
  OptTBRealSched tmax_normal_ab;
  OptTBRealSched tcost;
  OptTBRealSched overload_penalty;
  OptTRealSched lossfactor;
  OptTRealSched reactance;
  OptTRealSched voltage;
  OptTRealSched resistance;
  OptTRealSched tap_ratio;
  OptTRealSched phase_shift_deg;

  STBIndexHolder<ColIndex> flowp_cols;
  STBIndexHolder<ColIndex> flown_cols;
  STBIndexHolder<std::vector<ColIndex>> flowp_seg_cols;
  STBIndexHolder<std::vector<ColIndex>> flown_seg_cols;
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
