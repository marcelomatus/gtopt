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

  gtopt::register_alpha_variables(planning_lp(),
                                  scene_index,
                                  m_options_.scale_alpha,
                                  m_options_.cut_sharing,
                                  m_options_.boundary_cut_sharing);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Adds α (future-cost) to every phase — including the last — with
// bootstrap `lowb = uppb = 0`.  α is pinned at zero until a cut
// arrives: backward-pass cuts free it on non-last phases and
// boundary / named hot-start cut loaders free it on the last
// phase.  Keeps the iter-0 LP bounded without a positive lower
// bound and keeps the last phase symmetric with the others — the
// only difference is *when* α gets bounded.
void register_alpha_variables(PlanningLP& planning_lp,
                              SceneIndex scene_index,
                              double scale_alpha,
                              CutSharingMode cut_sharing,
                              BoundaryCutSharingMode boundary_cut_sharing)
{
  auto& sim = planning_lp.simulation();
  const auto& phases = sim.phases();
  const auto num_scenes = static_cast<std::size_t>(sim.scene_count());
  const auto n_phases = phases.size();

  // Under `multicut`, each scene-LP carries N future-cost columns
  // (`varphi_0..N-1`, one per SOURCE scene), each priced uniformly 1/N so the
  // objective's Σ_s (1/N)·varphi_s reconstructs the expected cost-to-go (PLP
  // `defprbpd.f:810`).  A scenario-s backward cut is later routed onto
  // `varphi_s` (uid = sddp_alpha_uid + s) in EVERY destination scene-LP, never
  // the destination's own α — that routing is what keeps the bound valid.
  // The intermediate phases follow `cut_sharing`; the terminal phase follows
  // `boundary_cut_sharing` (its boundary cuts land on the terminal α(s)).  For
  // every non-multicut mode n_alpha == 1 → the legacy single-α layout (uid 0).
  const bool intermediate_multi = (cut_sharing == CutSharingMode::multicut);
  const bool terminal_multi =
      (boundary_cut_sharing == BoundaryCutSharingMode::multicut);

  // Add `n_alpha` α columns to `li` and register each as a state variable in
  // the registry selected by `kind`.  Shared by the forward system and (when
  // present) the parallel aperture system so both LPs carry α + its cuts.
  const auto register_alpha_on = [&](LinearInterface& li,
                                     PhaseIndex pi,
                                     PhaseUid phase_uid,
                                     SystemKind kind,
                                     std::size_t n_alpha)
  {
    const double unit_cost = 1.0 / static_cast<double>(n_alpha);
    for (std::size_t s = 0; s < n_alpha; ++s) {
      const auto alpha_uid =
          static_cast<Uid>(sddp_alpha_uid + static_cast<Uid>(s));
      // α bootstrap: bidirectional pin `lowb = uppb = 0` keeps α at 0 until an
      // installed cut triggers `bound_alpha` to compute a floor (see the long
      // rationale below the loop).  `variable_uid` avoids a `-` in the column
      // label that CoinLpIO would reject.
      const auto alpha_sparse = SparseCol {
          .lowb = 0.0,
          .uppb = 0.0,
          .cost =
              unit_cost,  // physical cost: α is in $ — 1/N average under
                          // multicut; scaling handled by emit_col_to_backend
          .is_state = true,
          .scale = scale_alpha,
          .class_name = sddp_alpha_class_name,
          .variable_name = sddp_alpha_col_name,
          .variable_uid = alpha_uid,
          .context =
              make_scene_phase_context(sim.uid_of(scene_index), phase_uid),
      };
      const auto alpha_col = li.add_col(alpha_sparse);
      std::ignore = sim.add_state_variable(
          StateVariable::Key {
              .uid = alpha_uid,
              .col_name = sddp_alpha_col_name,
              .class_name = sddp_alpha_lp_class,
              .lp_key =
                  {
                      .scene_index = scene_index,
                      .phase_index = pi,
                      .kind = kind,
                  },
          },
          alpha_col,
          0.0,  // scost: no elastic penalty on alpha
          scale_alpha,  // var_scale: same as SparseCol.scale
          alpha_sparse.context);
    }
  };

  for (auto&& [pi, phase] : enumerate<PhaseIndex>(phases)) {
    const bool is_last = (static_cast<std::size_t>(pi) + 1 == n_phases);
    const bool multi = is_last ? terminal_multi : intermediate_multi;
    const std::size_t n_alpha =
        multi ? std::max<std::size_t>(num_scenes, 1) : 1;

    // Mirror α onto the aperture system (if any) so the backward-pass
    // aperture clones carry α and the cuts installed on it.  Independent
    // idempotency check against the aperture registry.
    if (auto* ap_sys = planning_lp.aperture_system(scene_index, pi);
        ap_sys != nullptr
        && find_alpha_state_var(sim, scene_index, pi, SystemKind::aperture)
            == nullptr)
    {
      register_alpha_on(ap_sys->linear_interface(),
                        pi,
                        phase.uid(),
                        SystemKind::aperture,
                        n_alpha);
    }

    if (find_alpha_state_var(sim, scene_index, pi) != nullptr) {
      continue;  // forward α already registered — idempotent
    }
    // Forward α.  Bootstrap pin `lowb = uppb = 0` keeps α at 0 until an
    // installed cut triggers `bound_alpha` to compute a floor.  In iter-0
    // the forward LP has no cuts and α has cost `scale_alpha > 0`, so the
    // minimiser would drive α to its floor anyway — but without the pin the
    // Chinneck Phase-1 elastic filter (which zeros every objective
    // coefficient, α included) leaves α a free basic variable, contaminating
    // captured trial values and the bcut fallback's Z.  Phase 0's α is later
    // released by the backward pass's final `bound_alpha(scene, 0)` call.
    register_alpha_on(planning_lp.system(scene_index, pi).linear_interface(),
                      pi,
                      phase.uid(),
                      SystemKind::forward,
                      n_alpha);
  }
}

void SDDPMethod::bound_alpha(SceneIndex scene_index, PhaseIndex phase_index)
{
  gtopt::bound_alpha(planning_lp(), scene_index, phase_index);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Provides the shared free-α primitive used by both `SDDPMethod`
// (backward / feasibility / aperture cut paths) and
// `load_boundary_cuts_csv` (last-phase boundary-cut install).
//
// ── Terminal α≥0 floor ────────────────────────────────────────────
// At the SDDP horizon's last phase, α represents the future cost
// beyond the planning horizon.  Under the gtopt convention every
// stage cost is non-negative (dispatch cost, demand-failure penalty,
// CAPEX annualisation, slack penalties), so the true value function
// is non-negative and α_T ≥ 0 is always a valid weak lower bound.
//
// Boundary cuts (`load_boundary_cuts_csv`) impose α + Σ coef·v ≥ rhs
// at the last phase to ENCODE the value-function support points from
// PLP's planos data, but the cuts only cover the trial-state regions
// they were generated at.  Aperture-perturbed trial states can land
// outside the cuts' polyhedral approximation, leaving α effectively
// unbounded below on those branches.  The juan/gtopt_iplp_plain
// iter-20 LB collapse (every aperture at p51 returning
// `CPX_STAT_UNBOUNDED`) is the visible symptom.
//
// Pinning α's column lower bound at `0` instead of `-∞` at the last
// phase gives an unconditional safety net.  When boundary cuts
// produce a tighter (positive) bound, they dominate via the cut row;
// when they don't, the column bound prevents the unbounded behaviour.
// Both paths are mathematically correct.
//
// Non-terminal phases keep the `-∞` release: cuts on α_t for
// intermediate phases accumulate during the SDDP backward sweep, and
// the bootstrap pin at iter-0 (`uppb = lowb = 0`) is preserved until
// the first cut row arrives.
void bound_alpha(PlanningLP& planning_lp,
                 SceneIndex scene_index,
                 PhaseIndex phase_index,
                 SystemKind kind)
{
  apply_alpha_floor(planning_lp, scene_index, phase_index, kind);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Consolidates the "only release α for cuts that actually reference α"
// gate used by every optimality-cut install site.  Prevents an
// optimality cut whose α coefficient was filtered at save time (e.g.
// by `cut_coeff_eps`) or a pure state-coupling cut from releasing
// the bootstrap pin, which would let α drift negative under the
// bidirectional α bootstrap (observed on juan/gtopt_iplp as LB=0).
void bound_alpha_for_cut(PlanningLP& planning_lp,
                         SceneIndex scene_index,
                         PhaseIndex phase_index,
                         const SparseRow& cut,
                         SystemKind kind)
{
  const auto& sim = planning_lp.simulation();
  // Fire iff the cut references ANY future-cost column on this cell.
  // Single-α modes register only `varphi_0`; under multicut a scenario-s
  // backward cut references its own `varphi_s`.  `bound_alpha` →
  // `apply_alpha_floor` then re-floors every `varphi` from its own cuts
  // (idempotent for the columns this cut doesn't touch).  α uids are
  // contiguous from `sddp_alpha_uid`, so the first registry miss ends
  // the scan.
  bool references_alpha = false;
  for (const auto src : iota_range<SceneIndex>(0, sim.scene_count())) {
    const auto* svar =
        find_alpha_state_var(sim, scene_index, phase_index, src, kind);
    if (svar == nullptr) {
      break;  // α not registered (or contiguous uids exhausted)
    }
    if (cut.cmap.contains(svar->col())) {
      references_alpha = true;
      break;
    }
  }
  if (!references_alpha) {
    return;  // cut does not reference α — leave the bootstrap pin.
  }
  bound_alpha(planning_lp, scene_index, phase_index, kind);
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// Derives a universal lower-bound floor on α at a given (scene, phase)
// from the cuts already installed there and pins α's column lower
// bound at it.  Closes the "aperture clone returns CPX_STAT_UNBOUNDED"
// hole observed on juan/gtopt_iplp_plain at iter 20+: boundary cuts
// cover only the trial-state regions they were generated at, and
// aperture-perturbed states can fall outside that polyhedral
// approximation.  The cut-derived floor is mathematically tighter
// than a flat ``α ≥ seed`` because it folds in the per-cut RHS and
// coefficients; it is also strictly above ``−∞`` whenever at least
// one cut is installed, so the column is never left unbounded below.
//
// ── State-variable formulation ──────────────────────────────────────
// A cut couples α to a set S of STATE VARIABLES `s` — NOT only reservoir
// `efin`: any registered state column (battery energy, etc.) can appear,
// each with a SIGNED subgradient `g_s` and a SIGNED feasible value range
// `x_s ∈ [x_s_min, x_s_max]` (x_s_min may be < 0).  The cut is
//
//     α + Σ_{s∈S} g_s · x_s ≥ rhs        ⇒   α ≥ rhs − Σ_s g_s · x_s.
//
// Taking the extremes of `g_s · x_s` over the state box gives a CONSTANT
// (state-independent) signed box on the α column:
//
//     floor_cut = rhs − Σ_s max(g_s·x_s_min, g_s·x_s_max)
//     ceil_cut  = rhs − Σ_s min(g_s·x_s_min, g_s·x_s_max)     [bound_above]
//
// Per state variable, by the SIGN of `g_s` (correct for any sign of x_s):
//   g_s > 0 →  max = g_s·x_s_max,  min = g_s·x_s_min
//   g_s < 0 →  max = g_s·x_s_min,  min = g_s·x_s_max
// `max(...)` (the largest subtrahend) makes the floor weakest/safest;
// `min(...)` (smallest subtrahend) makes the ceiling loosest.  Across
// multiple cuts: floor = min_k floor_cut (rows enforce the tight max),
// ceil = max_k ceil_cut.  α is SIGNED — floor may be < 0, ceil > 0; the
// seed-0 floor is only a fallback when no cut references α.
//
// ── Unit conventions ────────────────────────────────────────────────
// All arithmetic runs in physical-space units.  ``active_cuts()``
// returns rows BEFORE ``compose_physical``.  Column bounds are read
// via ``get_col_low()`` / ``get_col_upp()``.  The floor is written via
// ``set_col_low(α, floor_phys)`` (divides by ``scale_alpha``).
//
// ── Infinity propagation ────────────────────────────────────────────
// If the bound that achieves the worst-case subtrahend is unbounded,
// the cut contributes no useful floor — skip it.  Unboundedness is
// tested against raw LP bounds via ``li.is_pos_inf()`` /
// ``li.is_neg_inf()`` to avoid unit mismatches on columns with
// ``col_scale ≠ 1``.
void apply_alpha_floor(PlanningLP& planning_lp,
                       SceneIndex scene_index,
                       PhaseIndex phase_index,
                       SystemKind kind,
                       bool bound_above)
{
  const auto& sim = planning_lp.simulation();

  // Select the forward or aperture system for this cell.  When the aperture
  // system is requested but absent, there is nothing to floor.
  SystemLP* sys_ptr = (kind == SystemKind::aperture)
      ? planning_lp.aperture_system(scene_index, phase_index)
      : &planning_lp.system(scene_index, phase_index);
  if (sys_ptr == nullptr) {
    return;
  }

  // Enumerate every α (future-cost) column on this cell.  Single-α modes
  // register exactly one (uid offset 0); under `CutSharingMode::multicut`
  // there are N (`varphi_0..N-1`, uid = sddp_alpha_uid + source_scene).
  // Each `varphi_s` is floored INDEPENDENTLY from the cuts that reference
  // it, so a scenario-s cut never floors scenario-d's future-cost column
  // (the cross-scene routing in `share_cuts_for_phase` keeps each
  // scenario's cuts pinned to its own `varphi_s`).  The uid-keyed
  // `update_dynamic_col_bounds` overload below mirrors each release into
  // the correct replay entry — the name-only overload would always match
  // `varphi_0`.  α uids are contiguous from `sddp_alpha_uid`, so the
  // first registry miss ends the list.
  const auto num_scenes = sim.scene_count();
  std::vector<std::pair<ColIndex, Uid>> alpha_cols;
  for (const auto src : iota_range<SceneIndex>(0, num_scenes)) {
    const auto* svar =
        find_alpha_state_var(sim, scene_index, phase_index, src, kind);
    if (svar == nullptr) {
      break;  // contiguous uids — first gap ends the list
    }
    const auto alpha_uid = static_cast<Uid>(
        sddp_alpha_uid + static_cast<Uid>(static_cast<std::size_t>(src)));
    alpha_cols.emplace_back(svar->col(), alpha_uid);
  }
  if (alpha_cols.empty()) {
    return;
  }

  auto& sys = *sys_ptr;
  sys.ensure_lp_built();
  auto& li = sys.linear_interface();

  // PHYSICAL bounds for coef_phys · v_phys multiplication.
  const auto col_low_phys = li.get_col_low();
  const auto col_upp_phys = li.get_col_upp();
  // RAW bounds for unboundedness check (same units as li.infinity()).
  const auto col_low_raw = li.get_col_low_raw();
  const auto col_upp_raw = li.get_col_upp_raw();

  for (const auto& [alpha_col, alpha_uid] : alpha_cols) {
    double tightest_floor_phys = 0.0;
    bool any_cut_floor = false;
    // Symmetric CEILING accumulator (only used when `bound_above`).  The
    // ceiling is the LOOSEST (largest) upper bound across cuts:
    //   ceil_cut = rhs − Σⱼ min(coefⱼ·vⱼ)   with min(coefⱼ·vⱼ) over [vⱼ_min,
    //   vⱼ_max] = coefⱼ·vⱼ_min for coefⱼ>0, coefⱼ·vⱼ_max for coefⱼ<0.
    // At the optimum α equals the highest binding cut floor, which never
    // exceeds that cut's own ceiling — so α ≤ max_k ceil_k is valid and
    // does not cut the optimum.  Taking the MAX across cuts keeps it loose.
    double loosest_ceil_phys = 0.0;
    bool any_cut_ceil = false;
    [[maybe_unused]] std::size_t cuts_with_alpha = 0;
    for (const auto& cut : li.active_cuts()) {
      const auto alpha_it = cut.cmap.find(alpha_col);
      if (alpha_it == cut.cmap.end()) {
        continue;
      }
      ++cuts_with_alpha;

      double sup_sum_phys = 0.0;  // Σ max(coef·v) → floor = rhs − sup_sum
      double inf_sum_phys = 0.0;  // Σ min(coef·v) → ceil  = rhs − inf_sum
      bool sup_unbounded = false;
      bool inf_unbounded = false;
      for (const auto& [col, coef_phys] : cut.cmap) {
        if (col == alpha_col) {
          continue;
        }
        if (coef_phys == 0.0) {
          continue;
        }
        const double v_min_phys = col_low_phys[col];
        const double v_max_phys = col_upp_phys[col];
        const auto c = static_cast<size_t>(col);
        if (coef_phys > 0.0) {
          // max(coef·v) = coef·v_max ; min(coef·v) = coef·v_min
          if (li.is_pos_inf(col_upp_raw[c])) {
            sup_unbounded = true;
          } else {
            sup_sum_phys += coef_phys * v_max_phys;
          }
          if (li.is_neg_inf(col_low_raw[c])) {
            inf_unbounded = true;
          } else {
            inf_sum_phys += coef_phys * v_min_phys;
          }
        } else {
          // max(coef·v) = coef·v_min ; min(coef·v) = coef·v_max
          if (li.is_neg_inf(col_low_raw[c])) {
            sup_unbounded = true;
          } else {
            sup_sum_phys += coef_phys * v_min_phys;
          }
          if (li.is_pos_inf(col_upp_raw[c])) {
            inf_unbounded = true;
          } else {
            inf_sum_phys += coef_phys * v_max_phys;
          }
        }
      }
      if (!sup_unbounded) {
        const double cut_floor_phys = cut.lowb - sup_sum_phys;
        tightest_floor_phys = any_cut_floor
            ? std::min(tightest_floor_phys, cut_floor_phys)
            : cut_floor_phys;
        any_cut_floor = true;
      }
      if (bound_above && !inf_unbounded) {
        const double cut_ceil_phys = cut.lowb - inf_sum_phys;
        loosest_ceil_phys = any_cut_ceil
            ? std::max(loosest_ceil_phys, cut_ceil_phys)
            : cut_ceil_phys;
        any_cut_ceil = true;
      }
    }

    // Upper bound: default +∞ (lift from the bootstrap pin's 0 so cuts can
    // be satisfied — and so future SDDP cuts can push α up).  When
    // `bound_above` is requested AND at least one cut produced a bounded
    // ceiling, clamp α from above to the loosest cut-derived ceiling,
    // turning α into a finite box `[floor, ceil]` (the GPU-clean form, no
    // free column).  `set_col_upp` divides by α's col_scale (= scale_alpha)
    // just like `set_col_low`, and `ceil` is in the rebased-physical frame
    // (`cut.lowb` already carries the mean-shift offset `c`).
    const double upper_phys = (bound_above && any_cut_ceil)
        ? loosest_ceil_phys
        : LinearProblem::DblMax;
    if (li.is_pos_inf(upper_phys) || li.is_neg_inf(upper_phys)) {
      li.set_col_upp_raw(alpha_col, upper_phys);
    } else {
      li.set_col_upp(alpha_col, upper_phys);
    }
    // Use the raw setter for ±∞ sentinel floor values to avoid
    // set_col_low's internal division by col_scale on sentinels.
    if (li.is_pos_inf(tightest_floor_phys)
        || li.is_neg_inf(tightest_floor_phys))
    {
      li.set_col_low_raw(alpha_col, tightest_floor_phys);
    } else {
      li.set_col_low(alpha_col, tightest_floor_phys);
    }
    // uid-keyed update so the correct `varphi_s` replay entry is mirrored
    // under multicut (all N share class/variable name, differ only by uid).
    std::ignore = sys.update_dynamic_col_bounds(sddp_alpha_class_name,
                                                sddp_alpha_col_name,
                                                alpha_uid,
                                                tightest_floor_phys,
                                                upper_phys);

    SPDLOG_DEBUG("SDDP: α floor at (s{} p{} uid{}) → {:.6e} from {} cut(s)",
                 sim.uid_of(scene_index),
                 sim.uid_of(phase_index),
                 alpha_uid,
                 tightest_floor_phys,
                 cuts_with_alpha);
  }
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// One-line wrapper around `apply_alpha_floor` that targets the last
// phase with the universal weak floor ``α_T ≥ 0`` (cost-to-go is
// non-negative under non-negative stage costs).
void apply_terminal_alpha_floor(PlanningLP& planning_lp, SceneIndex scene_index)
{
  apply_alpha_floor(
      planning_lp, scene_index, planning_lp.simulation().last_phase_index());
}

// Free-function implementation declared in <gtopt/sddp_types.hpp>.
// One unified cut-install entry point: releases α iff optimality +
// cut references α, then adds the row via LinearInterface::add_cut_row
// (which also records the cut for low-memory replay).  Callers that
// also persist into SDDPCutManager invoke `SDDPMethod::store_cut`
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
    bound_alpha_for_cut(planning_lp, scene_index, phase_index, cut);
  }
  const auto row = planning_lp.system(scene_index, phase_index)
                       .linear_interface()
                       .add_cut_row(cut, eps);

  // ── Dual install on the aperture system ───────────────────────────────
  // When this cell has a backward-pass aperture system, the same cut must
  // also be installed there so the next backward step (which clones the
  // aperture LP) recurses with the updated value function.  The cut's
  // columns are in the forward LP's space; remap each to the aperture LP's
  // column via the uid-keyed state-variable registry (identity when the
  // aperture layout matches forward).  If any referenced column has no
  // aperture counterpart (reduced topology dropped a state var the cut
  // needs), skip the dual install with a warning rather than install a
  // malformed row.
  if (auto* ap_sys = planning_lp.aperture_system(scene_index, phase_index)) {
    const auto& sim = planning_lp.simulation();
    // forward col → aperture col for every state variable on this cell
    // (includes α, which is registered as a state var in both registries).
    std::vector<std::pair<ColIndex, ColIndex>> fwd2ap;
    for (const auto& [key, svar] :
         sim.state_variables(scene_index, phase_index, SystemKind::forward))
    {
      auto ap = sim.state_variable(StateVariable::Key {
          .scenario_uid = key.scenario_uid,
          .stage_uid = key.stage_uid,
          .uid = key.uid,
          .col_name = key.col_name,
          .class_name = key.class_name,
          .lp_key =
              {
                  .scene_index = scene_index,
                  .phase_index = phase_index,
                  .kind = SystemKind::aperture,
              },
      });
      if (ap) {
        fwd2ap.emplace_back(svar.col(), ap->get().col());
      }
    }

    SparseRow ap_cut = cut;
    ap_cut.cmap.clear();
    bool remapped_ok = true;
    ColIndex missing_col {unknown_index};
    for (const auto& [col, coeff] : cut.cmap) {
      const auto it =
          std::ranges::find(fwd2ap, col, &std::pair<ColIndex, ColIndex>::first);
      if (it == fwd2ap.end()) {
        remapped_ok = false;
        missing_col = col;
        break;
      }
      ap_cut.cmap.emplace(it->second, coeff);
    }

    if (remapped_ok) {
      if (cut_type == CutType::Optimality) {
        bound_alpha_for_cut(planning_lp,
                            scene_index,
                            phase_index,
                            ap_cut,
                            SystemKind::aperture);
      }
      std::ignore = ap_sys->linear_interface().add_cut_row(ap_cut, eps);
    } else {
      SPDLOG_WARN(
          "aperture dual cut-install skipped at (s{} p{}): cut references "
          "col {} with no aperture counterpart (cut_cols={}, fwd2ap={})",
          sim.uid_of(scene_index),
          sim.uid_of(phase_index),
          static_cast<int>(missing_col),
          cut.cmap.size(),
          fwd2ap.size());
    }
  }

  return row;
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
            .scenario_uid = key.scenario_uid,
            .stage_uid = key.stage_uid,
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
  const auto& li =
      planning_lp().system(scene_index, phase_index).linear_interface();

  // 0. (A3 sync 2026-04-30) Refresh each state-variable's cached
  //    `m_var_scale_` from the authoritative
  //    `LinearInterface::get_col_scale(col())`.  At StateVariable
  //    construction time only the user-set var_scale is available
  //    (LP not yet flattened), but ruiz equilibration may have
  //    multiplied an additional `ruiz_factor` into m_col_scales_
  //    later.  Without this sync, `state_var.var_scale()` returns
  //    the pre-equilibration value while `LinearInterface::get_col_cost()`
  //    divides by the post-equilibration value — the two cut
  //    construction overloads (overload 1 reads via state_var,
  //    overload 2 reads via LI's ScaledView) would then compute
  //    different `rc_phys` for the same column under ruiz mode.
  //    Cheap (one assignment per state variable per forward solve).
  for (const auto& [key, svar] : sim.state_variables(scene_index, phase_index))
  {
    const auto col = svar.col();
    if (col < li.numcols_as_index()) {
      svar.set_var_scale(li.get_col_scale(col));
    }
  }

  // 1. Always write col_sol for every state variable in THIS phase.
  //    Consumed by the next phase's propagate_trial_values().
  //    col_sol_phys is physical (and clamped to physical bounds when
  //    the last solve was optimal).  Recover the clean raw value via
  //    `phys / var_scale` — one division on an already-clean number,
  //    so it can't re-introduce the bound violation that clamping at
  //    physical removed.  Uses the post-step-0 sync'd var_scale.
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
