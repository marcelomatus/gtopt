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

private:
  OptTBRealSched tmax_ba;
  OptTBRealSched tmax_ab;
  OptTRealSched tcost;
  OptTRealSched lossfactor;
  OptTRealSched reactance;
  OptTRealSched voltage;
  OptTRealSched resistance;

  STBIndexHolder<ColIndex> flowp_cols;
  STBIndexHolder<ColIndex> flown_cols;
  STBIndexHolder<ColIndex> lossp_cols;
  STBIndexHolder<ColIndex> lossn_cols;
  STBIndexHolder<RowIndex> capacityp_rows;
  STBIndexHolder<RowIndex> capacityn_rows;

  STBIndexHolder<RowIndex> theta_rows;

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
   * @param dir "p" (A→B) or "n" (B→A) — used for LP variable names
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
      std::string_view dir);

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
