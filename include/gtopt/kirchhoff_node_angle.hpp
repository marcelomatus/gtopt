/**
 * @file      kirchhoff_node_angle.hpp
 * @brief     B–θ (node-angle) Kirchhoff Voltage Law row assembly
 * @date      2026-04-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One of two strategies for emitting DC-OPF KVL constraints — selected
 * via `KirchhoffMode::node_angle` (the default).  The classical B–θ
 * form: one bus angle (theta) variable per active bus + one KVL
 * equality per active line per block:
 *
 *   −θ_a + θ_b + x_τ · f_p − x_τ · f_n  =  −φ
 *
 * where `x_τ = τ · X / V²` is the off-nominal-tap-corrected susceptance
 * and `φ` is the line's phase-shift angle (radians).  In
 * `piecewise_direct` line-loss mode the `f_p` / `f_n` aggregator
 * columns are absent and each segment is stamped directly with `±x_τ`
 * — the per-block segment lists are passed via `seg_p_cols` /
 * `seg_n_cols` and the function picks the right path automatically.
 *
 * The sibling `KirchhoffMode::cycle_basis` strategy is not yet
 * implemented (forward-declared in `KirchhoffMode`).  See
 * `docs/formulation/mathematical-formulation.md` and Hörsch et al.
 * 2018 for the loop-flow derivation.
 */

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/sparse_col.hpp>  // ColIndex
#include <gtopt/sparse_row.hpp>  // RowIndex

namespace gtopt
{
class BusLP;
class LinearProblem;
class ScenarioLP;
class StageLP;
class SystemContext;
}  // namespace gtopt

namespace gtopt::kirchhoff::node_angle
{

/// Per-line scalars resolved at the (line, stage) level — extracted by
/// `LineLP::add_kirchhoff_rows` and passed in so the row-assembly
/// helper stays free of `LineLP` private state.
struct LineKvlInputs
{
  Uid line_uid;  ///< the line's strong-typed Uid
  std::string_view class_name;  ///< `LineLP::ClassName.full_name()` ("Line")
  std::string_view theta_constraint_name;  ///< `LineLP::ThetaName` ("theta")
  std::optional<double> reactance;  ///< `sc.stage_reactance(stage, …)`
  double voltage;  ///< per-stage V (default 1.0 if unset)
  double tap_ratio;  ///< per-stage τ (default 1.0)
  double phase_shift_deg;  ///< per-stage φ in degrees (default 0.0)
};

/// Emit one KVL equality row per block for a single line.  Returns
/// the per-block `RowIndex` map so the caller can store it (typically
/// in `LineLP::theta_rows[(scenario, stage)]`) for later dual output.
/// Returns an empty map when the line has no reactance, when the
/// effective `x_τ = τ · X / V²` is zero, or when either bus has no
/// theta column for this `(scenario, stage)`.
[[nodiscard]] BIndexHolder<RowIndex> add_line_kvl_rows(
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
    const BIndexHolder<std::vector<ColIndex>>& fnsegcols);

}  // namespace gtopt::kirchhoff::node_angle
