/**
 * @file      line_lp.hpp
 * @brief     LP formulation for transmission lines with linear and quadratic
 *            loss models
 * @date      Sat Mar 29 19:02:33 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the LineLP class which formulates transmission line
 * constraints for the LP model. It supports two loss models:
 *
 * 1. **Linear model** (lossfactor): fixed percentage of flow is lost.
 * 2. **Quadratic model** (resistance + voltage + loss_segments > 1):
 *    piecewise-linear approximation of P_loss = R · f² / V², where
 *    the flow range is divided into N equal segments with increasing
 *    loss factors that approximate the quadratic loss curve.
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
  static constexpr LPClassName ClassName {"Line", "lin"};

  using CapacityBase = CapacityObjectLP<Line>;

  /**
   * @brief Returns the underlying Line object with proper forwarding semantics
   * @tparam Self Deduced type of the calling object (const/non-const,
   * lvalue/rvalue)
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

  // ── Private helpers for add_to_lp ────────────────────────────────

  /**
   * @brief Stage-level parameters for the loss model, computed once per
   *        (scenario, stage) pair and reused across all blocks.
   */
  struct LossParams
  {
    double lossfactor {};  ///< Linear loss factor [p.u.]
    double resistance {};  ///< Line resistance [Ω]
    double V2 {};  ///< Voltage squared [kV²]
    int nseg {1};  ///< Number of piecewise-linear segments
    bool has_linear_loss {};
    bool has_quadratic_loss {};
    bool has_loss {};  ///< has_linear_loss || has_quadratic_loss
  };

  /**
   * @brief Result columns from adding flow variables for one direction.
   */
  struct DirectionResult
  {
    std::optional<ColIndex> flow_col;
    std::optional<ColIndex> loss_col;
    std::optional<RowIndex> capacity_row;
  };

  /**
   * @brief LP label suffixes for one flow direction.
   *
   * Avoids dynamic string formatting — labels are plain string literals,
   * consistent with the rest of the codebase (e.g. "fp", "fn", "capp").
   */
  struct DirectionLabels
  {
    std::string_view flow;  ///< "fp" or "fn"
    std::string_view seg;  ///< "fps" or "fns"
    std::string_view loss;  ///< "lsp" or "lsn"
    std::string_view link;  ///< "lnkp" or "lnkn"
    std::string_view loss_link;  ///< "lslp" or "lsln"
    std::string_view cap;  ///< "capp" or "capn"
  };

  /// Labels for the positive (A→B) flow direction.
  static constexpr DirectionLabels positive_labels {
      .flow = "fp",
      .seg = "fps",
      .loss = "lsp",
      .link = "lnkp",
      .loss_link = "lslp",
      .cap = "capp",
  };

  /// Labels for the negative (B→A) flow direction.
  static constexpr DirectionLabels negative_labels {
      .flow = "fn",
      .seg = "fns",
      .loss = "lsn",
      .link = "lnkn",
      .loss_link = "lsln",
      .cap = "capn",
  };

  /** @brief Compute the stage-level loss model parameters. */
  [[nodiscard]] LossParams compute_loss_params(const SystemContext& sc,
                                               const StageLP& stage) const;

  /**
   * @brief Add piecewise-linear flow and loss variables for one direction.
   *
   * Creates nseg segment variables, one total-flow variable, one loss
   * variable, and the linking / loss-tracking constraints for one flow
   * direction of the quadratic loss model.
   *
   * @param sc             System context providing scenario/stage data.
   * @param scenario       Current scenario LP object.
   * @param stage          Current stage LP object.
   * @param block          Current block LP object.
   * @param lp             Linear problem to add variables and rows to.
   * @param sending_brow   Power-balance row for the sending bus.
   * @param receiving_brow Power-balance row for the receiving bus.
   * @param block_tmax     Maximum line flow for this block [MW].
   * @param block_tcost    Flow cost coefficient for this block.
   * @param loss           Stage-level loss model parameters.
   * @param capacity_col   Optional capacity column for expansion constraints.
   * @param labels         Label suffixes for this direction (positive_labels or
   *                       negative_labels).
   */
  DirectionResult add_quadratic_flow_direction(
      SystemContext& sc,
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BlockLP& block,
      LinearProblem& lp,
      SparseRow& sending_brow,
      SparseRow& receiving_brow,
      double block_tmax,
      double block_tcost,
      const LossParams& loss,
      std::optional<ColIndex> capacity_col,
      const DirectionLabels& labels);

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
