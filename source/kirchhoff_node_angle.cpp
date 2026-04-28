// SPDX-License-Identifier: BSD-3-Clause

#include <numbers>

#include <gtopt/bus_lp.hpp>
#include <gtopt/kirchhoff_node_angle.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt::kirchhoff::node_angle
{

BIndexHolder<RowIndex> add_line_kvl_rows(
    SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    LinearProblem& lp,
    const BusLP& bus_a_lp,
    const BusLP& bus_b_lp,
    const LineKvlInputs& inputs,
    const BIndexHolder<ColIndex>& fpcols,
    const BIndexHolder<ColIndex>& fncols,
    const BIndexHolder<std::vector<ColIndex>>& fpsegcols,
    const BIndexHolder<std::vector<ColIndex>>& fnsegcols)
{
  // Skip Kirchhoff for lines without reactance (DC/HVDC lines).  A
  // zero-reactance line would create a degenerate constraint
  // (θ_a = θ_b) that doesn't model DC power flow correctly.
  if (!inputs.reactance || inputs.reactance.value() == 0.0) {
    return {};
  }

  const auto& blocks = stage.blocks();
  const auto& theta_a_cols =
      bus_a_lp.theta_cols_at(sc, scenario, stage, lp, blocks);
  const auto& theta_b_cols =
      bus_b_lp.theta_cols_at(sc, scenario, stage, lp, blocks);

  if (theta_a_cols.empty() || theta_b_cols.empty()) {
    return {};
  }

  const double X = inputs.reactance.value();
  // V defaults to 1.0 (per-unit mode).  When V is in kV, X must be in Ω
  // so that B = V²/X yields consistent susceptance units.
  const double V = inputs.voltage;
  // Physical susceptance term x = X / V² (reactance per V²).
  const double x = X / (V * V);

  // Off-nominal tap ratio: scales effective susceptance by τ.
  const double tau = inputs.tap_ratio;
  const double x_tau = tau * x;
  if (x_tau == 0.0) {
    return {};
  }

  // Phase-shift angle in radians; shifts the equality constraint RHS.
  const double phi_rad = inputs.phase_shift_deg * std::numbers::pi / 180.0;

  // Natural Kirchhoff form (no manual row pre-scaling):
  //
  //   -θ_a + θ_b + x_tau·f_p − x_tau·f_n = −φ_rad
  //
  // Prior versions divided the entire row by |x_tau| so that flow
  // coefficients became ±1, but this required a dual back-scale factor
  // (`theta_row_scale`) to recover physical units on output, and it
  // defeated the LP layer's row-max equilibration (which already
  // handles row norms internally).
  //
  // Writing the row in its natural form hands row scaling back to
  // `linear_problem.cpp` row-max equilibration, which auto-unscales
  // duals.  The theta column is separately col-scaled by `scale_theta`
  // (chosen as median(|x_tau|) in `planning_lp.cpp:auto_scale_theta`),
  // so after equilibration the median line's row has near-unit
  // coefficients both in the theta and flow directions.
  const double kirchhoff_rhs = -phi_rad;

  BIndexHolder<RowIndex> trows;
  map_reserve(trows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    auto trow =
        SparseRow {
            .class_name = inputs.class_name,
            .constraint_name = inputs.theta_constraint_name,
            .variable_uid = inputs.line_uid,
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid()),
        }
            .equal(kirchhoff_rhs);

    // piecewise_direct mode stamps each segment column directly with
    // ±x_τ (PLP genpdlin.f); other modes stamp the aggregator.  Per
    // block, exactly one of {segs, aggregator} is populated per
    // direction.  Pre-reserve roughly: 2 thetas + segs + aggregator.
    const auto fp_seg_it = fpsegcols.find(buid);
    const auto fn_seg_it = fnsegcols.find(buid);
    const auto fp_seg_n =
        (fp_seg_it != fpsegcols.end()) ? fp_seg_it->second.size() : 0;
    const auto fn_seg_n =
        (fn_seg_it != fnsegcols.end()) ? fn_seg_it->second.size() : 0;
    trow.reserve(2 + fp_seg_n + fn_seg_n + 2);

    trow[theta_a_cols.at(buid)] = -1.0;
    trow[theta_b_cols.at(buid)] = +1.0;

    if (fp_seg_n != 0) {
      for (const auto& col : fp_seg_it->second) {
        trow[col] = +x_tau;
      }
    } else if (auto fit = fpcols.find(buid); fit != fpcols.end()) {
      trow[fit->second] = +x_tau;
    }
    if (fn_seg_n != 0) {
      for (const auto& col : fn_seg_it->second) {
        trow[col] = -x_tau;
      }
    } else if (auto fit = fncols.find(buid); fit != fncols.end()) {
      trow[fit->second] = -x_tau;
    }

    trows[buid] = lp.add_row(std::move(trow));
  }

  return trows;
}

}  // namespace gtopt::kirchhoff::node_angle

namespace gtopt::kirchhoff
{

BIndexHolder<RowIndex> add_line_kvl_rows(
    SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    LinearProblem& lp,
    const LineLP& line,
    const BusLP& bus_a_lp,
    const BusLP& bus_b_lp,
    const BIndexHolder<ColIndex>& fpcols,
    const BIndexHolder<ColIndex>& fncols,
    const BIndexHolder<std::vector<ColIndex>>& fpsegcols,
    const BIndexHolder<std::vector<ColIndex>>& fnsegcols)
{
  switch (sc.options().kirchhoff_mode()) {
    case KirchhoffMode::node_angle: {
      // The dispatcher only fires when `use_kirchhoff` is true, so
      // `sc.stage_reactance` (which gates on `use_kirchhoff`) would
      // be redundant — call `param_reactance` directly.
      const node_angle::LineKvlInputs inputs {
          .line_uid = line.uid(),
          .class_name = LineLP::ClassName.full_name(),
          .theta_constraint_name = LineLP::ThetaName,
          .reactance = line.param_reactance(stage.uid()),
          .voltage = line.param_voltage(stage.uid()).value_or(1.0),
          .tap_ratio = line.param_tap_ratio(stage.uid()).value_or(1.0),
          .phase_shift_deg =
              line.param_phase_shift_deg(stage.uid()).value_or(0.0),
      };
      return node_angle::add_line_kvl_rows(sc,
                                           scenario,
                                           stage,
                                           lp,
                                           bus_a_lp,
                                           bus_b_lp,
                                           inputs,
                                           fpcols,
                                           fncols,
                                           fpsegcols,
                                           fnsegcols);
    }
    case KirchhoffMode::cycle_basis:
      // Loop-flow formulation: KVL rows are emitted at the system
      // level by `kirchhoff::cycle_basis::add_kvl_rows` after every
      // line has finished creating its flow vars.  Per-line dispatch
      // is therefore a no-op here.
      return {};
  }
  return {};  // unreachable, silences -Wreturn-type on some compilers
}

}  // namespace gtopt::kirchhoff
