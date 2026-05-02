/**
 * @file      reservoir_seepage_lp.cpp
 * @brief     Implementation of ReservoirSeepageLP methods
 * @date      Thu Jul 31 23:33:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the linear programming formulation for seepage systems.
 * Per-stage slope/constant schedules (from plpmanfi.dat Parquet files) are
 * read at construction time and applied directly as LP matrix coefficients
 * during add_to_lp() for each stage.  When piecewise-linear segments are
 * present, the LP constraint coefficients are updated dynamically based on
 * the reservoir volume via update_lp().
 */

#include <gtopt/input_context.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirSeepageLP::ReservoirSeepageLP(const ReservoirSeepage& pseepage,
                                       InputContext& ic)
    : ObjectLP<ReservoirSeepage>(pseepage)
    , m_slope_sched_(ic, Element::class_name, id(), std::move(seepage().slope))
    , m_constant_sched_(
          ic, Element::class_name, id(), std::move(seepage().constant))
{
}

bool ReservoirSeepageLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  // Intentional exception: the PAMPL class name here is SeepageName
  // ("seepage") rather than Element::class_name.snake_case()
  // ("reservoir_seepage"). Matching the downstream PAMPL convention that names
  // this element after the seepage constraint — not the ObjectLP wrapper — so
  // `seepage.flow` remains the canonical variable path.  Element-name
  // registration is hoisted into
  // `system_lp.cpp::register_all_ampl_element_names`.
  static constexpr std::string_view ampl_name = SeepageName;

  if (!is_active(stage)) {
    return true;
  }

  const auto& waterway = sc.element<WaterwayLP>(waterway_sid());
  const auto& reservoir = sc.element<ReservoirLP>(reservoir_sid());

  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);
  const auto eini_col = reservoir.eini_col_at(scenario, stage);
  const auto efin_col = reservoir.efin_col_at(scenario, stage);

  // Determine effective slope and intercept (RHS).
  // Priority: piecewise segments (volume-dependent) > per-stage schedule >
  // scalar default 0.0.
  Real effective_slope = m_slope_sched_.at(stage.uid()).value_or(0.0);
  Real effective_rhs = m_constant_sched_.at(stage.uid()).value_or(0.0);

  if (!seepage().segments.empty()) {
    const auto eini_vol = reservoir.reservoir().eini.value_or(0.0);
    const auto coeffs = select_seepage_coeffs(seepage().segments, eini_vol);
    effective_slope = coeffs.slope;
    effective_rhs = coeffs.intercept;
  }

  // Physical slope — flatten() applies col_scale to matrix coefficients.
  const Real lp_slope = effective_slope;

  const auto& blocks = stage.blocks();

  BIndexHolder<RowIndex> frows;
  BIndexHolder<ColIndex> fcols;
  map_reserve(frows, blocks.size());
  map_reserve(fcols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto fcol = flow_cols.at(buid);

    auto frow =
        SparseRow {
            .class_name = Element::class_name.full_name(),
            .constraint_name = SeepageName,
            .variable_uid = uid(),
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid()),
        }
            .equal(effective_rhs);

    // Anchor seepage to vfin only: q_filt = constant + slope · efin.
    // This makes the linearised constraint physically consistent at the
    // segment endpoints — in particular, when vfin → 0 in Tramo 1
    // (constant = 0, slope = first-segment slope), the row forces
    // q_filt → 0 as required by the PLP physics.  The previous
    // mid-stage average form (-0.5·slope on each of eini and efin)
    // pinned q_filt to a non-zero value even at vfin = 0 because of
    // the eini contribution.
    frow[efin_col] = -lp_slope;

    frow[fcol] = 1;

    frows[buid] = lp.add_row(std::move(frow));
    fcols[buid] = fcol;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  seepage_rows[st_key] = std::move(frows);
  seepage_cols[st_key] = std::move(fcols);

  // Register PAMPL-visible column under the canonical `flow` name
  // (matching waterway/flow_right).  The seepage constant/slope row is
  // exposed as a dual via add_to_output.
  if (!seepage_cols.at(st_key).empty()) {
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         WaterwayLP::FlowName,
                         scenario,
                         stage,
                         seepage_cols.at(st_key));
  }

  // Store the coefficient state for later updates
  m_states_[st_key] = ReservoirSeepageState {
      .eini_col = eini_col,
      .efin_col = efin_col,
      .current_slope = effective_slope,
      .current_rhs = effective_rhs,
  };

  return true;
}

bool ReservoirSeepageLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_col_sol(cname, SeepageName, pid, seepage_cols);
  out.add_col_cost(cname, SeepageName, pid, seepage_cols);
  out.add_row_dual(cname, SeepageName, pid, seepage_rows);

  return true;
}

int ReservoirSeepageLP::update_lp(SystemLP& sys,
                                  const ScenarioLP& scenario,
                                  const StageLP& stage)
{
  if (seepage().segments.empty()) {
    return 0;
  }

  auto& li = sys.linear_interface();
  const auto& rsv = sys.element<ReservoirLP>(reservoir_sid());
  const auto default_volume = rsv.reservoir().eini.value_or(0.0);

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  auto& state = m_states_.at(st_key);

  // Segment selection uses the *start-of-stage* volume (vini), which by
  // the time `update_lp` runs has already been propagated from the
  // predecessor phase's solved efin via `physical_eini`'s cross-phase
  // lookup (storage_lp.hpp::physical_eini(sys,...,sid) walks back to
  // `prev_sys->element(...).physical_efin(...)`).  Using vini matches
  // PLP's filtration construction, which evaluates the segment at the
  // start-of-stage state — the only volume value that is actually known
  // at the moment we build / update the stage's LP, since the
  // current-stage `efin` is still a free variable awaiting solve.
  //
  // The previous form anchored on `physical_efin(sys, stage, ...)` at the
  // CURRENT stage, which at iter-0 first attempt has no prior solve and
  // therefore returns the default `eini` fallback (= the JSON-level
  // initial volume, e.g. 1731 Hm³ for ELTORO).  That pinned segment 2
  // (covering [400, 2700]) for every stage of every scenario, even when
  // the predecessor's efin had already drained ELTORO well below 400 Hm³
  // in the forward pass.  At efin → 0 the segment-2 line forces a
  // 15.09 m³/s seepage from an empty reservoir, structurally breaking
  // the LP — observed on juan/gtopt_iplp p27/p37 cascade.
  const auto vini =
      rsv.physical_eini(sys, scenario, stage, default_volume, reservoir_sid());
  const auto coeffs = select_seepage_coeffs(seepage().segments, vini);

  const auto new_slope = coeffs.slope;
  const auto new_rhs = coeffs.intercept;

  if (new_slope == state.current_slope && new_rhs == state.current_rhs) {
    return 0;
  }

  int total = 0;
  const auto& frows = seepage_rows.at(st_key);

  for (const auto& [buid, row] : frows) {
    if (new_slope != state.current_slope) {
      // Anchor on efin only — see add_to_lp() above for rationale.
      li.set_coeff(row, state.efin_col, -new_slope);
    }
    if (new_rhs != state.current_rhs) {
      li.set_rhs(row, new_rhs);
    }
    ++total;
  }

  SPDLOG_TRACE(
      "ReservoirSeepageLP uid={}: updated constraints "
      "(vini={:.1f}, slope={:.6f}, rhs={:.6f})",
      uid(),
      vini,
      new_slope,
      new_rhs);

  state.current_slope = new_slope;
  state.current_rhs = new_rhs;

  return total;
}

}  // namespace gtopt
