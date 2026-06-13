/**
 * @file      reservoir_discharge_limit_lp.cpp
 * @brief     Implementation of ReservoirDischargeLimitLP methods
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the LP formulation for volume-dependent peak-discharge limits.
 * For each (scenario, stage, block) triple emits one row
 * `flow_b - slope·efin ≤ intercept`, enforcing the physical penstock cap on
 * every block independently (max-of-blocks).  When piecewise-linear segments
 * are present, update_lp() dynamically adjusts coefficient/RHS on every
 * block-level row of the active stage from the current reservoir volume.
 */

#include <gtopt/input_context.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_discharge_limit_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirDischargeLimitLP::ReservoirDischargeLimitLP(
    const ReservoirDischargeLimit& ddl, [[maybe_unused]] InputContext& ic)
    : ObjectLP<ReservoirDischargeLimit>(ddl)
{
}

bool ReservoirDischargeLimitLP::add_to_lp(const SystemContext& sc,
                                          const ScenarioLP& scenario,
                                          const StageLP& stage,
                                          LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto& ddl = reservoir_discharge_limit();
  if (ddl.segments.empty()) {
    return true;
  }

  // Flow source: either a classic Waterway's flow column (legacy
  // Reservoir→Waterway topology) or a built-in waterway turbine's own flow
  // column (the new ``Turbine.junction_a/b`` mode that subsumes the penstock).
  // ReservoirDischargeLimit accepts ``waterway`` OR ``turbine`` (validated
  // exactly-one-of in `validate_planning.cpp`).
  const auto& flow_cols = uses_turbine()
      ? sc.element<TurbineLP>(turbine_sid()).flow_cols_at(scenario, stage)
      : sc.element<WaterwayLP>(waterway_sid()).flow_cols_at(scenario, stage);
  const auto& reservoir = sc.element<ReservoirLP>(reservoir_sid());

  const auto eini_col = reservoir.eini_col_at(scenario, stage);
  const auto efin_col = reservoir.efin_col_at(scenario, stage);
  // Select initial segment based on reservoir initial volume
  const auto eini_vol = reservoir.reservoir().eini.value_or(0.0);
  const auto coeffs = select_rdl_coeffs(ddl.segments, eini_vol);

  // Physical slope — flatten() applies col_scale to matrix coefficients.
  const Real lp_slope = coeffs.slope;
  // First-segment feasibility (qeh ≥ 0 at efin = emin requires
  // `intercept + slope · emin ≥ 0`) is validated in
  // `validate_planning.cpp` against the raw input struct so the
  // schedule-form `emin` can be resolved without dragging
  // InputContext into add_to_lp.

  const auto& blocks = stage.blocks();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  // Per-block peak-flow rows: flow_b − slope · efin ≤ intercept
  //
  // Replaces the prior stage-average formulation (qeh = Σ_b
  // (dur_b/dur_stage)·flow_b, qeh − slope·efin ≤ intercept) with one
  // row per block, enforcing the DCMax cap on every block individually
  // instead of on the duration-weighted average.  Rationale:
  //
  //   * DCMax(V) is a **peak-flow** capacity (penstock m³/s from
  //     plpmaness.dat), not an energy.  The averaged form let a
  //     peaky pattern (e.g. one block at 8×DCMax for 1h, the rest
  //     at zero) satisfy `qeh ≤ DCMax` while violating the physical
  //     penstock in the peak block.  The max-of-blocks form rules
  //     this out.
  //
  //   * Anchoring on `efin` only (not `0.5·(eini + efin)`) keeps the
  //     constraint feasible at the segment-boundary `efin = emin`
  //     whenever the input data satisfies `intercept + slope·emin ≥ 0`
  //     (validated in `validate_planning.cpp`), and keeps segment
  //     selection consistent with `ReservoirSeepageLP`.  The averaging
  //     form could go infeasible mid-segment if `eini` was much
  //     smaller than `emin` during SDDP iter-1+ when a backward cut
  //     pulled `eini` toward zero.
  //
  //   * Eliminates `eini`'s coefficient entirely from the row, which
  //     removes a state-link coefficient and simplifies the
  //     cut-construction reduced-cost accounting on the SDDP backward
  //     pass.
  for (auto&& block : blocks) {
    const auto buid = block.uid();
    // A built-in waterway Turbine omits its per-block flow column for
    // blocks whose generator pmax is zero (the LP-elimination shortcut in
    // TurbineLP).  No flow column ⇒ no discharge to limit for that block,
    // so skip the row.  Classic Waterway flow columns are never elided, so
    // this only fires on the ``turbine`` path.
    const auto fcol_it = flow_cols.find(buid);
    if (fcol_it == flow_cols.end()) {
      continue;
    }
    const auto fcol = fcol_it->second;

    auto vol_row =
        SparseRow {
            .class_name = Element::class_name.full_name(),
            .constraint_name = DvolName,
            .variable_uid = uid(),
            .context = make_block_context(scenario.uid(), stage.uid(), buid),
        }
            .less_equal(coeffs.intercept);

    vol_row[fcol] = 1.0;
    vol_row[efin_col] = -lp_slope;

    vol_rows[st_key][buid] = lp.add_row(std::move(vol_row));
  }

  // Store state for update_lp.  The reservoir cache eliminates
  // `sys.element<ReservoirLP>(reservoir_sid())` from the update_lp path.
  m_states_[st_key] = RDLState {
      .eini_col = eini_col,
      .efin_col = efin_col,
      .current_slope = coeffs.slope,
      .current_rhs = coeffs.intercept,
      .reservoir_cache = make_reservoir_ref_cache(reservoir, scenario, stage),
  };

  return true;
}

bool ReservoirDischargeLimitLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_row_dual(cname, DvolName, pid, vol_rows);

  return true;
}

int ReservoirDischargeLimitLP::update_lp(SystemLP& sys,
                                         const ScenarioLP& scenario,
                                         const StageLP& stage)
{
  if (reservoir_discharge_limit().segments.size() < 2) {
    return 0;
  }

  auto& li = sys.linear_interface();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  // SDDP apertures reuse the base scene's discharge rows but carry their
  // own scenario uids; ``update_lp`` is invoked for those uids even though
  // ``add_to_lp`` only keyed the base scene.  There is nothing to
  // re-adjust for an aperture (it inherits the base row), so no-op rather
  // than throw — mirrors ``ReservoirProductionFactorLP::update_lp``.
  const auto state_it = m_states_.find(st_key);
  if (state_it == m_states_.end()) {
    return 0;
  }
  auto& state = state_it->second;

  // Volume derived from `state.reservoir_cache` — no `sys.element<ReservoirLP>`
  // on the current sys.  Cross-phase efin lookup goes through the
  // pre-bound `prev_phase_efin_col` (production) or the element-lookup
  // fallback (test paths) — both routes return identical numerics.
  const Real volume =
      average_volume_from_cache(sys, scenario, stage, state.reservoir_cache);

  const auto coeffs =
      select_rdl_coeffs(reservoir_discharge_limit().segments, volume);

  const auto new_slope = coeffs.slope;
  const auto new_rhs = coeffs.intercept;

  // No in-memory short-circuit: see the matching comment in
  // `reservoir_seepage_lp.cpp::update_lp` for rationale.  Under
  // `LowMemoryMode::compress` / `snapshot` the LP's matval / RHS
  // revert to construction-time on every `load_flat`, but
  // `state.current_slope` / `state.current_rhs` survive
  // unchanged — an equality short-circuit silently skipped
  // re-issuing the writes and produced primal-infeasible target
  // re-solves under compress while off ran clean.
  //
  // efin-only formulation — see `add_to_lp` above for the
  // feasibility rationale.  Only the `efin_col` coefficient is
  // re-issued; `eini_col`'s coefficient is permanently 0 in this
  // row.  Max-of-blocks formulation: re-issue on every block's row.
  int total = 0;
  const auto brows_it = vol_rows.find(st_key);
  if (brows_it == vol_rows.end()) {
    return total;
  }
  const auto& brows = brows_it->second;

  for (const auto& [buid, row] : brows) {
    li.set_coeff(row, state.efin_col, -new_slope);
    // Use ``set_row_upp`` (not ``set_rhs``).  The row was constructed
    // in ``add_to_lp`` with ``.less_equal(coeffs.intercept)`` — i.e.
    // a ``<=`` row with ``lowb = -DblMax, uppb = intercept``.
    // ``LinearInterface::set_rhs_raw`` calls
    // ``backend->set_row_bounds(row, rhs, rhs)`` which silently
    // rewrites BOTH bounds, converting the row to an equality at
    // ``rhs`` and forcing ``flow_b - slope·efin == new_rhs``.  On
    // CEN65 ``plp_2_years`` this turned RALCO's DCMax cap into a
    // hard discharge requirement: stages where the turbine was in
    // maintenance (``pmax`` drops to 24.64 MW at block 141) couldn't
    // satisfy the ≥72 m³/s implied by the equality, and the SDDP
    // forward pass on scene 14 / 18 at phase 27 reported "elastic
    // filter produced no feasibility cut".  ``set_row_upp`` updates
    // only the upper bound, preserving the original ``<=`` sense.
    li.set_row_upp(row, new_rhs);
    total += 2;
  }

  SPDLOG_TRACE(
      "ReservoirDischargeLimitLP uid={}: updated {} block rows "
      "(volume={:.1f}, slope={:.6f}, rhs={:.6f})",
      uid(),
      brows.size(),
      volume,
      new_slope,
      new_rhs);

  state.current_slope = new_slope;
  state.current_rhs = new_rhs;

  return total;
}

}  // namespace gtopt
