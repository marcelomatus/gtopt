/**
 * @file      sddp_method_alpha.cpp
 * @brief     SDDPMethod alpha-variable & state-variable lifecycle
 * @date      2026-04-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Sibling translation unit of ``sddp_method.cpp``; carries a
 * focused subset of ``SDDPMethod``'s member functions to keep
 * each TU under ~700 LoC.  Split landed in commit referenced by
 * Phase B of the gtopt-hygiene refactor.  See
 * ``include/gtopt/sddp_method.hpp`` for the class declaration
 * and ``source/sddp_method.cpp`` for the constructor / solver
 * lifecycle helpers.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_status.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

void SDDPMethod::initialize_alpha_variables(SceneIndex scene_index)
{
  auto& phase_states = m_scene_phase_states_[scene_index];
  phase_states.resize(planning_lp().simulation().phases().size());

  gtopt::register_alpha_variables(
      planning_lp(), scene_index, m_options_.scale_alpha);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Adds α (future-cost) to every phase — including the last — with
// bootstrap `lowb = uppb = 0`.  α is pinned at zero until a cut
// arrives: backward-pass cuts free it on non-last phases and
// boundary / named hot-start cut loaders free it on the last
// phase.  Keeps the iter-0 LP bounded without a positive lower
// bound and keeps the last phase symmetric with the others — the
// only difference is *when* α gets freed.
void register_alpha_variables(PlanningLP& planning_lp,
                              SceneIndex scene_index,
                              double scale_alpha)
{
  auto& sim = planning_lp.simulation();
  const auto& phases = sim.phases();

  for (auto&& [pi, phase] : enumerate<PhaseIndex>(phases)) {
    if (find_alpha_state_var(sim, scene_index, pi) != nullptr) {
      continue;  // already registered — idempotent
    }
    auto& li = planning_lp.system(scene_index, pi).linear_interface();
    // α bootstrap: bidirectional pin `lowb = uppb =
    // sddp_alpha_bootstrap_min (=0)` freezes α at 0 until an
    // installed cut releases it via `free_alpha`.
    //
    // Rationale (supersedes the earlier `uppb=+∞` bootstrap): in
    // iter-0 the forward LP has no cuts and α has cost
    // `scale_alpha > 0`, so with `uppb=+∞` the minimiser drives α
    // to its floor of 0 anyway — but *without* pinning, the Chinneck
    // Phase-1 elastic filter (which zeros every objective coefficient
    // including α's) has no cost signal on α, so simplex returns
    // whatever basic value it picks.  That value gets captured into
    // `state_var.col_sol`, contaminates downstream trial propagation
    // and the bcut fallback's Z.  Pinning α bidirectionally removes
    // the α column as a free variable from the Phase-1 LP and keeps
    // the elastic clone's α = 0 regardless of objective zeroing.
    //
    // Phase 0's α IS released: the aperture backward pass iterates
    // `phase_index` ∈ [T-1 .. 1] with `src_phase_index =
    // previous(phase_index)` ∈ [T-2 .. 0], and
    // `install_aperture_backward_cut` calls
    // `free_alpha(scene_index, src_phase_index)` on both the
    // expected-cut path (`sddp_aperture_pass.cpp:160`) and the bcut
    // fallback path (`sddp_aperture_pass.cpp:270`).  So phase 0 is
    // freed in the final step of every backward pass.
    const auto alpha_sparse = SparseCol {
        .lowb = sddp_alpha_bootstrap_min / scale_alpha,
        .uppb = sddp_alpha_bootstrap_min / scale_alpha,
        .cost = scale_alpha,
        .is_state = true,
        .scale = scale_alpha,
        .class_name = sddp_alpha_class_name,
        .variable_name = sddp_alpha_col_name,
        // Without variable_uid the column label serialises to
        // `sddp_alpha_-1_<scene>_<phase>` (unknown_uid = -1), whose
        // embedded `-` char is rejected by CoinLpIO's name validator
        // — CBC then strips every col/row label from the written LP.
        // Mirrors master #426 (a8a0e452) which set this on cut rows.
        // α is unique per (scene, phase), so `sddp_alpha_uid = 0`
        // disambiguates trivially.
        .variable_uid = sddp_alpha_uid,
        .context =
            make_scene_phase_context(sim.uid_of(scene_index), phase.uid()),
    };
    const auto alpha_col = li.add_col(alpha_sparse);

    // Track dynamic column for low_memory reconstruction.
    planning_lp.system(scene_index, pi).record_dynamic_col(alpha_sparse);

    // Register α as a regular state variable so all label-based
    // machinery (state CSV I/O, cut CSV I/O, cross-level resolution)
    // treats it uniformly with reservoir/storage state vars.
    std::ignore = sim.add_state_variable(
        StateVariable::Key {
            .uid = sddp_alpha_uid,
            .col_name = sddp_alpha_col_name,
            .class_name = sddp_alpha_lp_class,
            .lp_key = {.scene_index = scene_index, .phase_index = pi},
        },
        alpha_col,
        0.0,  // scost: no elastic penalty on alpha
        scale_alpha,  // var_scale: same as SparseCol.scale
        alpha_sparse.context);
  }
}

void SDDPMethod::free_alpha(SceneIndex scene_index, PhaseIndex phase_index)
{
  gtopt::free_alpha(planning_lp(), scene_index, phase_index);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Provides the shared free-α primitive used by both `SDDPMethod`
// (backward / feasibility / aperture cut paths) and
// `load_boundary_cuts_csv` (last-phase boundary-cut install).
void free_alpha(PlanningLP& planning_lp,
                SceneIndex scene_index,
                PhaseIndex phase_index)
{
  const auto* svar =
      find_alpha_state_var(planning_lp.simulation(), scene_index, phase_index);
  if (svar == nullptr) {
    return;  // alpha not registered on this (scene, phase) — nothing to free.
  }
  auto& sys = planning_lp.system(scene_index, phase_index);
  sys.ensure_lp_built();
  sys.linear_interface().set_col_low_raw(svar->col(), -LinearProblem::DblMax);
  sys.linear_interface().set_col_upp_raw(svar->col(), LinearProblem::DblMax);
  // Mirror the release into the persistent `m_dynamic_cols_` entry so
  // a subsequent `release_backend` + `ensure_backend` (compress /
  // rebuild) replays α with the freed bounds via
  // `apply_post_load_replay`.  Without this the release+reload cycle
  // would restore the bootstrap `lowb = uppb = 0`, re-pinning α
  // until the next cut install dominates on the next solve.
  sys.update_dynamic_col_bounds(sddp_alpha_class_name,
                                sddp_alpha_col_name,
                                -LinearProblem::DblMax,
                                LinearProblem::DblMax);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Consolidates the "only release α for cuts that actually reference α"
// gate used by every optimality-cut install site.  Prevents an
// optimality cut whose α coefficient was filtered at save time (e.g.
// by `cut_coeff_eps`) or a pure state-coupling cut from releasing
// the bootstrap pin, which would let α drift negative under the
// bidirectional α bootstrap (observed on juan/gtopt_iplp as LB=0).
void free_alpha_for_cut(PlanningLP& planning_lp,
                        SceneIndex scene_index,
                        PhaseIndex phase_index,
                        const SparseRow& cut)
{
  const auto* alpha_svar =
      find_alpha_state_var(planning_lp.simulation(), scene_index, phase_index);
  if (alpha_svar == nullptr) {
    return;  // α not registered on this cell — nothing to free.
  }
  if (!cut.cmap.contains(alpha_svar->col())) {
    return;  // cut does not reference α — leave the bootstrap pin.
  }
  free_alpha(planning_lp, scene_index, phase_index);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// One unified cut-install entry point: releases α iff optimality +
// cut references α, then adds the row via LinearInterface::add_cut_row
// (which also records the cut for low-memory replay).  Callers that
// also persist into SDDPCutStore invoke `SDDPMethod::store_cut`
// separately with the returned RowIndex — `store_cut` no longer
// re-records for replay to avoid double-registering.
RowIndex add_cut_row(PlanningLP& planning_lp,
                     SceneIndex scene_index,
                     PhaseIndex phase_index,
                     CutType cut_type,
                     const SparseRow& cut,
                     double eps)
{
  if (cut_type == CutType::Optimality) {
    free_alpha_for_cut(planning_lp, scene_index, phase_index, cut);
  }
  return planning_lp.system(scene_index, phase_index)
      .linear_interface()
      .add_cut_row(cut, eps);
}

void SDDPMethod::collect_state_variable_links(SceneIndex scene_index)
{
  const auto& sim = planning_lp().simulation();
  const auto& phases = sim.phases();
  const auto last_phase_index = sim.last_phase_index();

  auto& phase_states = m_scene_phase_states_[scene_index];

  for (auto&& [phase_index, _ph] : enumerate<PhaseIndex>(phases)) {
    // The last phase produces no outgoing state-variable links to a
    // next phase (there is no next phase), so there is nothing to
    // collect and we break.  Incidentally this also avoids touching
    // the last phase's backend — which, under low_memory modes, is
    // still released at this point because `initialize_alpha_variables`
    // only added alpha (and thus only reconstructed) for non-last
    // phases — but the real reason is structural.
    if (phase_index == last_phase_index) {
      break;
    }

    auto& state = phase_states[phase_index];

    // Read column bounds from the source phase LP
    const auto& src_li =
        planning_lp().system(scene_index, phase_index).linear_interface();
    const auto col_lo = src_li.get_col_low_raw();
    const auto col_hi = src_li.get_col_upp_raw();
    const auto scale_obj = src_li.scale_objective();

    const auto next_phase_index = next(phase_index);

    for (const auto& [key, svar] :
         sim.state_variables(scene_index, phase_index))
    {
      // Defensive skip: α is registered as a state variable for CSV/JSON
      // I/O uniformity, but it is a BACKWARD-propagating variable (the
      // future-cost estimator populated by backward-pass cuts), not a
      // forward state like reservoir energy.  It must never enter
      // `outgoing_links`, because the elastic filter relaxes each link's
      // dependent column with slack variables — and we explicitly don't
      // want slacks on α.  In practice α has no `dependent_variables()`
      // entry (`register_alpha_variables` doesn't set one), so this
      // skip is also a no-op by construction — kept explicit to protect
      // against a future change that might add cross-phase α linking.
      // α is registered with class_name = sddp_alpha_lp_class.
      // LPClassName's operator== compares the underlying full_name,
      // so value-compare works against either another LPClassName
      // or (via implicit conversion) a string_view.
      if (key.class_name == sddp_alpha_lp_class) {
        continue;
      }

      // Per-variable state cost from StateVariable (set at registration time
      // by ReservoirLP, BatteryLP, etc.).  Pre-divide by scale_objective so
      // it is consistent with the global penalty.
      const auto link_scost =
          (svar.scost() > 0.0) ? svar.scost() / scale_obj : 0.0;

      for (const auto& dep : svar.dependent_variables()) {
        if (dep.phase_index() != next_phase_index
            || dep.scene_index() != scene_index)
        {
          continue;
        }

        // `source_low` / `source_upp` are documented as *physical*
        // bounds (see `StateVarLink` in benders_cut.hpp), so scale by
        // `var_scale` at capture time.  The `relax_fixed_state_variable`
        // consumer then applies them via the physical setter
        // `set_col_low` / `set_col_upp`, which divides by the dependent
        // column's own `col_scale` — scale-agnostic across phases even
        // when `var_scale(source) != col_scale(dependent)`.
        const double svar_scale = svar.var_scale();
        // Effective LP-to-physical scale on the source column: includes
        // any ruiz-added factor on top of the user's var_scale.  Used to
        // lift raw LP bounds `col_lo/col_hi` into physical units; using
        // `svar_scale` alone undercounts the ruiz factor and the cut-
        // RHS clamp then kicks in at the wrong physical boundary.
        const double src_col_scale = src_li.get_col_scale(svar.col());
        // Reverse-lookup human-readable element name for diagnostic
        // logs (e.g. "Reservoir:LMAULE:efin" instead of
        // "Reservoir:1:efin").  The map itself is populated once at
        // `SystemLP` construction via `register_all_ampl_element_names`;
        // by the time SDDP solve runs it is read-only and safe for
        // the linear scan.  `lookup_ampl_element_name` handles the
        // PascalCase/snake_case mismatch internally (the StateVariable
        // key stores class_name in PascalCase while the AMPL registry
        // uses snake_case).  Empty string_view when the element has
        // no registered AMPL name (test fixtures, plain JSON without
        // the `name:` field) — diagnostic logs then fall back to
        // numeric uid.
        const auto element_name =
            sim.lookup_ampl_element_name(key.class_name, key.uid);
        state.outgoing_links.push_back(StateVarLink {
            .source_col = svar.col(),
            .dependent_col = dep.col(),
            .source_phase_index = phase_index,
            .target_phase_index = dep.phase_index(),
            .source_low = col_lo[svar.col()] * src_col_scale,
            .source_upp = col_hi[svar.col()] * src_col_scale,
            .var_scale = svar_scale,
            .scost = link_scost,
            // Raw pointer into the simulation's state-variable registry
            // (flat_map, stable for the full solver lifetime — same
            // lifetime that already couples source and dependent LP cols).
            .state_var = &svar,
            // Identity for diagnostic logs (e.g. "Reservoir:LMAULE:efin"
            // when `name` is resolved, falls back to "Reservoir:8:efin").
            .class_name = key.class_name,
            .col_name = key.col_name,
            .uid = key.uid,
            .name = element_name,
        });
      }
    }

    // Coverage audit: one TRACE line per (scene, phase) with both
    // counts shown side-by-side.  Readers grep for `(N/M)` where N !=
    // M to spot skip-ahead couplings or — more importantly — state
    // variables that were registered but have no dependent link in
    // the next phase, which would silently defeat the elastic filter.
    SPDLOG_TRACE(
        "SDDP: scene {} phase {} outgoing state-variable links: {}/{} "
        "(links/registered state vars)",
        scene_index,
        phase_index,
        state.outgoing_links.size(),
        sim.state_variables(scene_index, phase_index).size());
  }
}

void SDDPMethod::capture_state_variable_values(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    const ScaledView& col_sol_phys,
    std::span<const double> reduced_costs) const noexcept
{
  const auto& sim = planning_lp().simulation();

  // 1. Always write col_sol for every state variable in THIS phase.
  //    Consumed by the next phase's propagate_trial_values().
  //    col_sol_phys is physical (and clamped to physical bounds when
  //    the last solve was optimal).  Recover the clean raw value via
  //    `phys / var_scale` — one division on an already-clean number,
  //    so it can't re-introduce the bound violation that clamping at
  //    physical removed.
  const auto ncols = col_index_size(col_sol_phys);
  for (const auto& [key, svar] : sim.state_variables(scene_index, phase_index))
  {
    const auto col = svar.col();
    if (col < ncols) {
      const double phys = col_sol_phys[col];
      const double vs = svar.var_scale();
      svar.set_col_sol((vs != 0.0) ? phys / vs : phys);
    }
  }

  // 2. Write per-link reduced_cost onto the *source* state variables
  //    in the previous phase (whose outgoing_links have dependent_col
  //    in THIS phase's LP).  No previous phase on phase 0.
  if (!phase_index) {
    return;
  }
  const auto prev_phase_index = previous(phase_index);
  const auto& prev_state = m_scene_phase_states_[scene_index][prev_phase_index];

  for (const auto& link : prev_state.outgoing_links) {
    if (link.state_var == nullptr) {
      continue;
    }
    const auto dep = link.dependent_col;
    if (dep < col_index_size(reduced_costs)) {
      link.state_var->set_reduced_cost(reduced_costs[dep]);
    }
  }
}

// ── Elastic filter via LP clone (PLP pattern) ───────────────────────────────

std::optional<SDDPMethod::ElasticResult> SDDPMethod::elastic_solve(
    SceneIndex scene_index, PhaseIndex phase_index, const SolverOptions& opts)
{
  if (!phase_index) {
    return std::nullopt;
  }

  const auto& li =
      planning_lp().system(scene_index, phase_index).linear_interface();
  const auto prev_phase_index = previous(phase_index);
  const auto& prev_state = m_scene_phase_states_[scene_index][prev_phase_index];

  // Delegate to BendersCut member (uses work pool when set).
  auto elastic_opts = opts;
  // Crossover MUST stay enabled: the feasibility-cut builder reads
  // row duals (`get_row_price()`) off the fixing equations to
  // compute Farkas-ray coefficients for the classical Benders fcut
  // (PLP / Birge-Louveaux convention).  Barrier solutions without
  // crossover produce interior-point multipliers that are noisy and
  // non-vertex — they cannot be used as a Farkas ray.  The crossover
  // step converts to a vertex optimum where row duals are well-
  // defined and α-free fcuts have non-trivial state coefficients.
  elastic_opts.crossover = true;

  // Scale the elastic penalty by cost_factor so it is consistent with all
  // other LP objective coefficients that go through stage_ecost / cost_factor.
  // The per-variable physical-unit scaling (var_scale) is applied inside
  // relax_fixed_state_variable() using each link's var_scale field.
  //
  // Slack-cost scaling: the elastic filter's slack variables (sup/sdn)
  // were previously given cost = 1 (Chinneck's pure-feasibility
  // convention).  On plp_2_years iter 0 this produced a degeneracy
  // where every α-free fcut contributed a tiny hyperplane on the
  // state and no signal on future cost, stalling convergence as
  // phases 13-25 thrashed through 100 backtracks without closing
  // the gap.  Multiplying the slack cost by the target phase's
  // discount factor puts each slack on the same economic footing
  // as dispatch costs at that phase — the elastic objective now
  // reflects "discounted cost of violating the state pin" rather
  // than "unit-feasibility gap", breaking the degeneracy when no
  // optimality cuts have arrived yet at intermediate phases.
  const auto scale_obj = li.scale_objective();
  const auto& target_phase = planning_lp().simulation().phases()[phase_index];
  const double phase_discount = target_phase.stages().empty()
      ? 1.0
      : target_phase.stages().front().discount_factor();
  // Slack-cost base matches PLP's Chinneck Phase-1 convention:
  // `osicallsc.cpp:658` passes obj=1.0 flat to every slack when
  // `objs == nullptr` (PLP's AgrElastici call site at
  // `plp-agrespd.f:673`).  gtopt's `relax_fixed_state_variable`
  // prices each slack at `penalty` (no `× var_scale`, no
  // `/ scale_obj`), so with `elastic_penalty = 1.0` (default) the
  // cloned LP's slack obj equals `phase_discount ≈ 1.0` — exactly
  // PLP's raw unit cost.  `phase_discount` stays as gtopt-local
  // economic weighting so the slack stays commensurate with
  // dispatch cost at the target phase.  `scale_obj` retained for
  // signature stability only.
  (void)scale_obj;
  const auto scaled_penalty = m_options_.elastic_penalty * phase_discount;

  // Chinneck IIS mode runs an extra re-solve to filter non-essential
  // relaxed bounds before cut construction.  Other modes use the regular
  // elastic filter (cuts may be averaged via build_multi_cuts at the
  // call site).
  auto result = (m_options_.elastic_filter_mode == ElasticFilterMode::chinneck)
      ? chinneck_filter_solve(
            li, prev_state.outgoing_links, scaled_penalty, elastic_opts)
      : m_benders_cut_.elastic_filter_solve(
            li, prev_state.outgoing_links, scaled_penalty, elastic_opts);

  if (result.has_value()) {
    // The clone's solve activity (resolve, fallbacks, kappa, wall
    // time) lives on the clone's own SolverStats.  Fold it back into
    // the owning system so the end-of-run aggregate reflects the true
    // backend workload, including elastic retries.
    planning_lp()
        .system(scene_index, phase_index)
        .merge_solver_stats(result->clone.solver_stats());

    SPDLOG_TRACE(
        "SDDP elastic: scene {} phase {} solved via clone "
        "(obj={:.4f})",
        scene_index,
        phase_index,
        result->clone.get_obj_value());
  }

  return result;
}

}  // namespace gtopt
