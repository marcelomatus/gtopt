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
  static constexpr LPClassName ClassName {"Line"};
  static constexpr std::string_view FlowName {
      "flow"};  ///< compound: `flowp − flown`
  static constexpr std::string_view FlowpName {"flowp"};
  static constexpr std::string_view FlownName {"flown"};
  static constexpr std::string_view LosspName {"lossp"};
  static constexpr std::string_view LossnName {"lossn"};
  static constexpr std::string_view CapacitypName {"capacityp"};
  static constexpr std::string_view CapacitynName {"capacityn"};
  static constexpr std::string_view ThetaName {"theta"};

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

  [[nodiscard]] constexpr const auto& flowp_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return flowp_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr const auto& flown_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return flown_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr const auto& lossp_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return lossp_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr const auto& lossn_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return lossn_cols.at({scenario.uid(), stage.uid()});
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
  [[nodiscard]] auto param_tcost(StageUid s) const { return tcost.at(s); }
  [[nodiscard]] auto param_reactance(StageUid s) const
  {
    return reactance.at(s);
  }
  /// @}

private:
  OptTBRealSched tmax_ba;
  OptTBRealSched tmax_ab;
  OptTRealSched tcost;
  OptTRealSched lossfactor;
  OptTRealSched reactance;
  OptTRealSched voltage;
  OptTRealSched resistance;
  OptTRealSched tap_ratio;
  OptTRealSched phase_shift_deg;

  STBIndexHolder<ColIndex> flowp_cols;
  STBIndexHolder<ColIndex> flown_cols;
  STBIndexHolder<ColIndex> lossp_cols;
  STBIndexHolder<ColIndex> lossn_cols;
  STBIndexHolder<RowIndex> capacityp_rows;
  STBIndexHolder<RowIndex> capacityn_rows;

  STBIndexHolder<RowIndex> theta_rows;

  /// Kirchhoff row normalization factor per (scenario, stage).
  /// Each Kirchhoff row is divided by |x_tau| so that flow coefficients are
  /// ±1 and theta coefficients are ±1/|x_tau|.  The dual must be multiplied
  /// by this factor to recover physical units.
  STIndexHolder<double> theta_row_scale;

  /**
   * @brief Add Kirchhoff (DC OPF) theta constraints for all blocks.
   *
   * Creates a constraint row per block that links the voltage-angle
   * difference to the total line flow:
   *   θ_a − θ_b + x·fp − x·fn = 0
   */
  void add_kirchhoff_rows(SystemContext& sc,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          LinearProblem& lp,
                          const BusLP& bus_a_lp,
                          const BusLP& bus_b_lp,
                          const BIndexHolder<ColIndex>& fpcols,
                          const BIndexHolder<ColIndex>& fncols);
};

}  // namespace gtopt
