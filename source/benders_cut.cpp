/**
 * @file      benders_cut.cpp
 * @brief     Modular Benders cut construction and handling – implementation
 * @date      2026-03-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the cut-creation building blocks declared in benders_cut.hpp.
 * The free functions moved here were originally part of sddp_solver.cpp.
 * The BendersCut class implementation is also here.
 */

#include <cmath>
#include <ranges>

#include <gtopt/benders_cut.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/work_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Optimality cut ─────────────────────────────────────────────────────────

void propagate_trial_values(std::span<StateVarLink> links,
                            std::span<const double> source_solution,
                            LinearInterface& target_li) noexcept
{
  for (auto& link : links) {
    link.trial_value = source_solution[link.source_col];
    target_li.set_col_low_raw(link.dependent_col, link.trial_value);
    target_li.set_col_upp_raw(link.dependent_col, link.trial_value);
  }
}

void propagate_trial_values(std::span<StateVarLink> links,
                            LinearInterface& target_li) noexcept
{
  // Cross-phase propagation must go through **physical space** because
  // the source phase's `var_scale` and the target phase's
  // `col_scale(dependent_col)` are not guaranteed to match.  Writing
  // `state_var->col_sol()` (which is `phys / var_scale_source`)
  // straight to `set_col_low_raw(dep)` silently pins the dependent
  // column at `phys / var_scale_source × col_scale_dep` — off by a
  // factor of `col_scale_dep / var_scale_source` whenever those two
  // scales disagree.  Diagnosed on plp_2_years/juan_iplp: RALCO
  // phase-2 sini ended up at 73.82 LP (= 233 phys / 3.162²) when the
  // correct raw bound was 73.82 LP = 233 phys / 3.162.
  //
  // The fix: read the physical trial value via `col_sol_physical()`
  // and pin the dependent bound via the physical setter, which
  // internally divides by the dependent column's own `col_scale`.
  for (auto& link : links) {
    const double v_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    target_li.set_col_low(link.dependent_col, v_phys);
    target_li.set_col_upp(link.dependent_col, v_phys);
    // Keep `trial_value` in dependent-column raw LP units so
    // downstream cut-builders (which compare against the bound
    // snapshot in `StateVarLink::source_{low,upp}` also captured in
    // raw) see a self-consistent value.
    link.trial_value = v_phys / target_li.get_col_scale(link.dependent_col);
  }
}

auto build_benders_cut_physical(ColIndex alpha_col,
                                std::span<const StateVarLink> links,
                                std::span<const double> reduced_costs_physical,
                                std::span<const double> trial_values_physical,
                                double objective_value_physical,
                                double cut_coeff_eps) -> SparseRow
{
  // Physical-space Benders optimality cut:
  //   α_phys ≥ z_t_phys + Σ_i rc_phys_i · (x_{t-1,i}_phys − v̂_i_phys)
  //
  // α coefficient is 1.0; `LinearInterface::add_row` on an
  // equilibrated LP folds `col_scales[alpha]` automatically.  No
  // `scale_alpha` or `inv_scale_obj` arithmetic here — every input is
  // already in $ / physical-units, matching what `target_li.get_col_cost()`
  // and `target_li.get_obj_value_physical()` return at the call site.
  auto row = SparseRow {
      .lowb = objective_value_physical,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = 1.0;

  for (const auto& [i, link] : std::views::enumerate(links)) {
    const auto rc_phys = reduced_costs_physical[link.dependent_col];
    if (std::abs(rc_phys) < cut_coeff_eps) {
      continue;
    }
    const auto v_hat_phys = trial_values_physical[static_cast<std::size_t>(i)];
    row[link.source_col] = -rc_phys;
    row.lowb -= rc_phys * v_hat_phys;
  }

  // No `already_lp_space` flag: this row IS physical.  `add_row` on an
  // equilibrated LP will apply col_scales + row-max composition.
  return row;
}

auto build_benders_cut_physical(ColIndex alpha_col,
                                std::span<const StateVarLink> links,
                                double objective_value_physical,
                                double scale_objective,
                                double cut_coeff_eps) -> SparseRow
{
  // Physical-space Benders optimality cut, reading rc and trial from
  // each link's back-pointer StateVariable.  The forward pass mirrors
  // the target LP's solution onto every StateVariable via
  // `capture_state_variable_values`, so the live values are fresh at
  // backward-pass time without needing per-LP snapshots.
  auto row = SparseRow {
      .lowb = objective_value_physical,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = 1.0;

  for (const auto& link : links) {
    if (link.state_var == nullptr) {
      continue;
    }
    const auto rc_phys = link.state_var->reduced_cost_physical(scale_objective);
    if (std::abs(rc_phys) < cut_coeff_eps) {
      continue;
    }
    const auto v_hat_phys = link.state_var->col_sol_physical();
    row[link.source_col] = -rc_phys;
    row.lowb -= rc_phys * v_hat_phys;
  }

  return row;
}

auto build_benders_cut_physical(ColIndex alpha_col,
                                std::span<const StateVarLink> links,
                                const LinearInterface& rc_source,
                                double objective_value_physical,
                                double cut_coeff_eps) -> SparseRow
{
  // Physical-space Benders cut that takes rc from an arbitrary solved
  // LinearInterface (elastic clone / aperture clone) and trial from
  // each link's source StateVariable.  Used by the forward-pass
  // feasibility cut, the aperture per-aperture cut, and the aperture
  // fallback bcut — all paths where the reduced cost comes from a
  // local solve rather than the base forward pass mirror.
  auto row = SparseRow {
      .lowb = objective_value_physical,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha_col] = 1.0;

  const auto rc_view = rc_source.get_col_cost();
  for (const auto& link : links) {
    const auto rc_phys = rc_view[link.dependent_col];
    if (std::abs(rc_phys) < cut_coeff_eps) {
      continue;
    }
    const auto v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    row[link.source_col] = -rc_phys;
    row.lowb -= rc_phys * v_hat_phys;
  }

  return row;
}

auto build_feasibility_cut_physical(std::span<const StateVarLink> links,
                                    std::span<const RelaxedVarInfo> link_infos,
                                    LinearInterface& rc_source,
                                    double cut_coeff_eps,
                                    int niter) -> SparseRow
{
  // Classical Benders feasibility cut (PLP convention): no α column,
  // pure state-coupling built from **row duals** of the fixing
  // equations `dep + sup − sdn = v̂` in the elastic clone.  The
  // dual value π_i at the clone's Phase-1 optimum is the Farkas
  // ray component for state variable i — matches
  // `plp-agrespd.f::AgrElastici` which reads
  // `lp->getRowPrice()[row]`.  Column reduced costs (the old
  // implementation) are zero for basic state columns and produced
  // an empty `0 ≥ Z` row when the slack absorbed the gap.
  //
  // The RHS term per link is π_i · (v̂_i + slack_i) where
  // slack_i = x[sup_col] - x[sdn_col] is the net slack activation.
  // Matches PLP's `rhs[i] = (b[row] + dx) * ray[i]`.
  //
  // Crossover must be enabled on the elastic solve (see
  // `SDDPMethod::elastic_solve`) so `get_row_price()` returns vertex
  // duals rather than interior-point multipliers.
  auto row = SparseRow {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
  };

  auto duals = rc_source.get_row_dual();
  const auto sol_raw = rc_source.get_col_sol_raw();

  const auto n = std::min(links.size(), link_infos.size());
  for (std::size_t i = 0; i < n; ++i) {
    const auto& link = links[i];
    const auto& info = link_infos[i];
    if (!info.relaxed || info.fixing_row == RowIndex {unknown_index}) {
      continue;
    }
    // Farkas-ray sign convention: PLP's `osi_lp_get_feasible_cut`
    // stores `ray[i] = -dual[row]`.  Our fixing equation is
    // `dep + sup − sdn = v̂` (gtopt's convention) vs PLP's
    // `dep − sp + sn = v̂`, so PLP's sp = our sdn, sn = our sup.
    // With `pi = -dual`, `dx = sdn − sup` (= PLP's `sp − sn`), the
    // resulting cut `pi·source ≥ pi·(v̂ + dx)` simplifies to
    // `source ≥ dep_clone` when sdn is active (clone chose
    // `dep_clone = v̂ + sdn` to meet downstream feasibility).  That
    // is the classical "master state must be at least the
    // clone-feasible dep value" Benders fcut.
    const double pi = -duals[info.fixing_row];
    if (std::abs(pi) < cut_coeff_eps) {
      continue;
    }

    const double sup_val = (info.sup_col != ColIndex {unknown_index})
        ? sol_raw[info.sup_col]
        : 0.0;
    const double sdn_val = (info.sdn_col != ColIndex {unknown_index})
        ? sol_raw[info.sdn_col]
        : 0.0;
    const double dx = sdn_val - sup_val;

    // Relative low-activation filter — same form as `build_multi_cuts`:
    // drop the link when `|π · dx|` is below `1e-12 × (|π · v̂| + 1e-8)`.
    // Without this filter, a link whose slack barely activated (trial
    // at bound → dx ≈ 0, π large) injects `π · v̂` into the RHS and
    // the source_col into the LHS, biasing the aggregate toward
    // `Σ πᵢ · sᵢ ≥ Σ πᵢ · v̂ᵢ`.
    {
      const double rhs_scale =
          (std::abs(pi) * std::abs(link.trial_value)) + 1e-8;
      if (std::abs(pi * dx) < 1e-12 * rhs_scale) {
        continue;
      }
    }

    // dx is in slack-col raw LP units; lift to physical via the
    // state variable's var_scale so `v̂_phys + dx · var_scale`
    // equals the clone's physical `dep` optimum.  Without the
    // `× var_scale` factor the RHS silently miscomputes whenever
    // var_scale ≠ 1 (RALCO/ELTORO energy state variables at √10
    // triggered this in plp_2_years).
    const double v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    const double dep_clone_phys = v_hat_phys + (dx * link.var_scale);

    row[link.source_col] = pi;
    row.lowb += pi * dep_clone_phys;
  }

  // Outward perturbation on the aggregated RHS, scaled by `niter` (the
  // number of times this (scene, phase) has already emitted a cut in
  // this iteration).  Starting from 0 means the first cut has no
  // perturbation; each repeat bumps ε × |rhs| upward, helping the
  // master escape a degenerate LP vertex that keeps regenerating the
  // same row.  Matches PLP's escalating retry strategy.
  const double eps_factor = 1e-10 * static_cast<double>(niter);
  if (eps_factor > 0.0) {
    row.lowb += eps_factor * std::abs(row.lowb);
  }

  return row;
}

// ─── Elastic filter ─────────────────────────────────────────────────────────

RelaxedVarInfo relax_fixed_state_variable(
    LinearInterface& li,
    const StateVarLink& link,
    [[maybe_unused]] PhaseIndex phase_index,
    double penalty)
{
  const auto dep = link.dependent_col;
  const auto lo = li.get_col_low_raw()[dep];
  const auto hi = li.get_col_upp_raw()[dep];

  if (std::abs(lo - hi) >= 1e-10) {
    return {};
  }

  // Relax to the physical source-column bounds captured into
  // `StateVarLink` at SDDPMethod link-collection time.  Using the
  // physical setter `set_col_low` / `set_col_upp` makes this path
  // scale-agnostic: if `col_scale(source)` disagrees with
  // `col_scale(dependent)`, the physical setter divides by the
  // dependent column's own scale, pinning the correct raw LP
  // bound regardless of cross-phase scale drift.
  li.set_col_low(dep, link.source_low);
  li.set_col_upp(dep, link.source_upp);

  // Elastic slack variables (sup / sdn).  The caller
  // (`elastic_filter_solve`) has zeroed every original objective
  // coefficient on the clone, so the relaxed LP's optimum equals
  // `slack_cost × Σ(s⁺ + s⁻)` — a weighted feasibility gap.
  //
  // Slack cost = `penalty × link.var_scale`.  The caller
  // (`SDDPMethod::elastic_solve`) passes
  //   penalty = 100 × target_phase_discount / scale_objective
  // which is the LP-space per-unit cost of a physical-unit
  // violation at the target phase.  We multiply by `link.var_scale`
  // here because the fixing equation
  //   dep + sup − sdn = trial_value
  // has unit coefficients on sup/sdn while `dep` lives in LP
  // units = physical / var_scale.  So a unit of `sup` / `sdn`
  // represents one LP-unit of `dep`, which equals `var_scale`
  // physical units.  Pricing the slack at `penalty × var_scale`
  // keeps a consistent "per physical unit violated" cost across
  // state variables with different scales (e.g. RALCO energy at
  // √10 vs PEHUENCHE at 1.0).
  //
  // Empirical: on plp_2_years iter 0 the var_scale-aware price
  // pushes the forward pass ~6 phases deeper than the LP-only
  // price before the `forward_max_attempts=100` budget exhausts
  // (p19 vs p15), breaking the α-free degeneracy that was
  // diagnosed in the phase-13 thrash.  Previously hard-coded to
  // 1.0 (Chinneck (2008) § 4 pure-feasibility).
  // P0-A fix (2026-04-23): per-variable `link.scost` overrides the
  // global penalty when set (> 0).  `scost` is populated in
  // `collect_state_variable_links` from `svar.scost() / scale_obj`,
  // matching the pre-divide-by-scale_obj convention of `penalty`.
  // Multi-reservoir fixtures with differing water values previously
  // had this field silently discarded — every slack was priced at
  // the same global penalty, breaking the elastic filter's ability
  // to prefer cheap-to-violate variables over expensive ones.
  const double base_penalty = (link.scost > 0.0) ? link.scost : penalty;
  const double slack_cost = base_penalty * link.var_scale;

  // D3 — finite slack upper bounds matching PLP's
  // `osicallsc.cpp::osi_lp_get_feasible_cut` convention.
  //
  // PLP's fixing equation is  `dep − sp + sn = trial`  (osicallsc.cpp
  // :668,681 — sp has element −1.0, sn has element +1.0):
  //     sp lifts dep UP,  sp.upper = colUpp − trial
  //     sn pulls dep DOWN, sn.upper = trial − colLow
  //
  // gtopt's fixing equation is  `dep + sup − sdn = trial`:
  //     sdn lifts dep UP,  sdn.upper = colUpp_LP − trial
  //     sup pulls dep DOWN, sup.upper = trial − colLow_LP
  //
  // Note the NAMING is opposite between PLP (sp=up, sn=down) and
  // gtopt (sdn=up, sup=down).  What matters is the SIGN of the
  // coefficient on each slack in the fixing row.  The common
  // physical meaning is: the upward slack is bounded by how much
  // room there is to the column's upper bound; the downward
  // slack is bounded by how much room there is to the column's
  // lower bound.  `source_{low,upp}` are physical per 0a45e52b,
  // so we convert to LP units via `source / var_scale` before
  // subtracting the LP-space trial_value.
  //
  // Safety fallback: if `source_low == source_upp` (zero-width
  // box — typically when a fixture forgets to populate bounds)
  // OR `var_scale == 0`, the finite-bound math collapses.  In
  // that degenerate case revert to `DblMax`, matching the pre-D3
  // behaviour so unit-test scaffolds that haven't populated
  // bounds still exercise the filter.
  const bool have_finite_box =
      (link.var_scale > 0.0) && (link.source_upp > link.source_low);
  const double vs = have_finite_box ? link.var_scale : 1.0;
  const double src_upp_lp =
      have_finite_box ? link.source_upp / vs : LinearProblem::DblMax;
  const double src_low_lp =
      have_finite_box ? link.source_low / vs : -LinearProblem::DblMax;
  // sdn lifts dep UP (coeff −1 in the fixing row → dep = trial + sdn − sup)
  const double sdn_uppb = have_finite_box
      ? std::max(0.0, src_upp_lp - link.trial_value)
      : LinearProblem::DblMax;
  // sup pulls dep DOWN (coeff +1 in the fixing row)
  const double sup_uppb = have_finite_box
      ? std::max(0.0, link.trial_value - src_low_lp)
      : LinearProblem::DblMax;

  const auto sup = li.add_col(SparseCol {
      .uppb = sup_uppb,
      .cost = slack_cost,
  });

  const auto sdn = li.add_col(SparseCol {
      .uppb = sdn_uppb,
      .cost = slack_cost,
  });

  // dep + sup − sdn = trial_value
  auto elastic = SparseRow {
      .lowb = link.trial_value,
      .uppb = link.trial_value,
  };
  elastic[dep] = 1.0;
  elastic[sup] = 1.0;
  elastic[sdn] = -1.0;

  const auto fixing_row = li.add_row(elastic);

  SPDLOG_TRACE(
      "SDDP elastic: phase {} col {} relaxed to [{:.2f}, {:.2f}] "
      "(source bounds from phase {})",
      phase_index,
      dep,
      link.source_low,
      link.source_upp,
      link.source_phase_index);

  return RelaxedVarInfo {
      .relaxed = true,
      .sup_col = sup,
      .sdn_col = sdn,
      .fixing_row = fixing_row,
  };
}

auto elastic_filter_solve(const LinearInterface& li,
                          std::span<const StateVarLink> links,
                          double penalty,
                          const SolverOptions& opts)
    -> std::optional<ElasticSolveResult>
{
  // Clone the LP; modifications don't touch the original.
  auto cloned = li.clone();

  // Chinneck Phase-1 feasibility LP: zero every original objective
  // coefficient so the relaxed LP becomes a pure feasibility problem.
  // Its optimum equals Σ(s⁺ + s⁻) = minimum total slack activation to
  // restore feasibility — independent of dispatch cost structure.
  // Matches PLP `plp-bc.f`, Chinneck (2008) § 4, Ruszczyński (1997).
  // Keeping the original obj would leak state-dependent opex into
  // the Benders feasibility-cut RHS and drive α to diverge under
  // SDDP iteration (observed on juan/gtopt_iplp).
  for (const auto c : iota_range<ColIndex>(0, cloned.numcols_as_index())) {
    cloned.set_obj_coeff(c, 0.0);
  }

  // α is intentionally NOT pinned / modified in the clone: leave it in
  // whatever bound state the original LP has (bootstrap `lowb=uppb=0`
  // on first iter, or `[-DblMax, +DblMax]` after `free_alpha` has fired
  // from a prior optimality cut).  Pinning α at 0 here would make the
  // clone infeasible whenever the target phase has an installed
  // optimality cut `α + Σ rc·s ≥ Z` with positive Z at the current
  // state, hiding the true feasibility gap on the forward state
  // variables.  α is never added to `outgoing_links` (see
  // `collect_state_variable_links`), so no slack variables are ever
  // created for α — the filter treats α purely as a passive column
  // that keeps whatever value satisfies the installed cuts.

  ElasticSolveResult result;
  result.link_infos.reserve(links.size());

  bool modified = false;
  for (const auto& link : links) {
    auto info = relax_fixed_state_variable(
        cloned, link, link.target_phase_index, penalty);
    modified |= info.relaxed;
    result.link_infos.push_back(info);
  }

  if (!modified) {
    return std::nullopt;
  }

  // Solve the clone with elastic slack variables
  auto r = cloned.resolve(opts);
  result.solved = r.has_value() && cloned.is_optimal();
  if (result.solved) {
    SPDLOG_TRACE("elastic_filter_solve: solved clone (obj={:.4f})",
                 cloned.get_obj_value());
  } else {
    SPDLOG_DEBUG(
        "elastic_filter_solve: clone did NOT reach optimal — "
        "returning with solved=false so callers can persist the "
        "clone for post-mortem diagnostics");
  }
  result.clone = std::move(cloned);
  return result;
}

auto chinneck_filter_solve(const LinearInterface& li,
                           std::span<const StateVarLink> links,
                           double penalty,
                           const SolverOptions& opts,
                           double slack_tol)
    -> std::optional<ElasticSolveResult>
{
  // Phase 1 — full elastic relaxation, identical to elastic_filter_solve()
  auto initial = elastic_filter_solve(li, links, penalty, opts);
  if (!initial.has_value()) {
    return std::nullopt;
  }
  // When the elastic clone itself is infeasible, propagate the
  // result verbatim (with solved=false) so the caller can persist
  // the unsolved clone for diagnostics.  Continuing into the
  // slack-classification phase would read `get_col_sol_raw()` off
  // an unsolved LP and return garbage.
  if (!initial->solved) {
    return initial;
  }

  auto& clone = initial->clone;
  auto& infos = initial->link_infos;
  const auto sol = clone.get_col_sol_raw();

  // Phase 2 — classify links by slack activity
  std::vector<std::size_t> non_essential;
  non_essential.reserve(infos.size());
  std::size_t n_active = 0;
  for (std::size_t i = 0; i < infos.size(); ++i) {
    const auto& info = infos[i];
    if (!info.relaxed) {
      continue;
    }
    const double sup_val =
        (info.sup_col != ColIndex {unknown_index}) ? sol[info.sup_col] : 0.0;
    const double sdn_val =
        (info.sdn_col != ColIndex {unknown_index}) ? sol[info.sdn_col] : 0.0;
    if (sup_val <= slack_tol && sdn_val <= slack_tol) {
      non_essential.push_back(i);
    } else {
      ++n_active;
    }
  }

  // No essential links → nothing to filter; return the full elastic result.
  if (n_active == 0) {
    SPDLOG_TRACE(
        "chinneck_filter_solve: all {} relaxed bounds inactive — returning "
        "full elastic result unchanged",
        infos.size());
    return initial;
  }
  if (non_essential.empty()) {
    SPDLOG_TRACE(
        "chinneck_filter_solve: all {} relaxed bounds essential — IIS == "
        "full set, no filtering needed",
        n_active);
    return initial;
  }

  // Phase 3 — re-fix non-essential links by zeroing their slack uppers,
  // forcing the elastic equation `dep + sup − sdn = trial_value` to
  // hold strictly (sup = sdn = 0 ⇒ dep = trial_value).
  for (const auto i : non_essential) {
    auto& info = infos[i];
    if (info.sup_col != ColIndex {unknown_index}) {
      clone.set_col_upp_raw(info.sup_col, 0.0);
    }
    if (info.sdn_col != ColIndex {unknown_index}) {
      clone.set_col_upp_raw(info.sdn_col, 0.0);
    }
  }

  // Phase 4 — re-solve to confirm the IIS.
  auto r = clone.resolve(opts);
  const bool refixed_ok = r.has_value() && clone.is_optimal();

  if (!refixed_ok) {
    // The supposedly non-essential links were essential after all (penalty
    // competition obscured the true IIS).  Undo the re-fix and fall back
    // to the conservative full-elastic result.
    SPDLOG_DEBUG(
        "chinneck_filter_solve: re-solve infeasible after re-fixing {} "
        "non-essential link(s) — falling back to full elastic IIS "
        "(status {})",
        non_essential.size(),
        clone.get_status());
    for (const auto i : non_essential) {
      auto& info = infos[i];
      if (info.sup_col != ColIndex {unknown_index}) {
        clone.set_col_upp_raw(info.sup_col, LinearProblem::DblMax);
      }
      if (info.sdn_col != ColIndex {unknown_index}) {
        clone.set_col_upp_raw(info.sdn_col, LinearProblem::DblMax);
      }
    }
    // Re-solve once more to restore a consistent optimal basis for cut
    // construction.  If even this fails we propagate the failure.
    auto r2 = clone.resolve(opts);
    if (!r2.has_value() || !clone.is_optimal()) {
      return std::nullopt;
    }
    return initial;
  }

  // Phase 5 — IIS confirmed.  Mark non-essential links so downstream
  // `build_multi_cuts()` and any cut consumers treat them as inactive.
  for (const auto i : non_essential) {
    infos[i].sup_col = ColIndex {unknown_index};
    infos[i].sdn_col = ColIndex {unknown_index};
  }

  SPDLOG_INFO(
      "chinneck_filter_solve: IIS = {} essential / {} relaxed bounds "
      "(filtered {} non-essential, obj={:.4f})",
      n_active,
      n_active + non_essential.size(),
      non_essential.size(),
      clone.get_obj_value());
  return initial;
}

auto build_feasibility_cut(const LinearInterface& li,
                           ColIndex alpha_col,
                           std::span<const StateVarLink> links,
                           double penalty,
                           const SolverOptions& opts,
                           double /*scale_alpha*/,
                           double /*scale_objective*/)
    -> std::optional<FeasibilityCutResult>
{
  auto elastic = elastic_filter_solve(li, links, penalty, opts);
  if (!elastic.has_value()) {
    return std::nullopt;
  }

  // Physical-space cut: rc from elastic clone, trial from state_var.
  // The `scale_alpha` / `scale_objective` parameters are retained in
  // the signature for source compatibility but are now unused — the
  // physical builder lets `add_row` fold col_scales + row-max on the
  // caller's LP.
  auto cut =
      build_benders_cut_physical(alpha_col,
                                 links,
                                 elastic->clone,
                                 elastic->clone.get_obj_value_physical());

  return FeasibilityCutResult {
      .cut = std::move(cut),
      .elastic = std::move(*elastic),
  };
}

auto build_multi_cuts(ElasticSolveResult& elastic,
                      std::span<const StateVarLink> links,
                      const LpContext& context,
                      double cut_coeff_eps,
                      int niter) -> std::vector<SparseRow>
{
  std::vector<SparseRow> cuts;

  // PLP `plp-agrespd.f::AgrElastici` + `osicallsc.cpp::osi_lp_get_feasible_cut`
  // Birge-Louveaux sensitivity-cut convention — per-link signed
  // feasibility cut:
  //
  //   π · source_col ≥ π · (trial + sup_val − sdn_val)
  //                                       + FactEPS · |RHS|
  //
  //   π = −row_dual[fixing_row]   (signed sensitivity dual from the
  //                                elastic clone's optimal basis)
  //
  // `π` is the row dual on the fixing equation `dep + sup − sdn =
  // trial` at the elastic clone's optimum.  The name `ray` inherited
  // from the legacy PLP `osi_lp_get_feasible_cut` routine is a
  // misnomer here — this is NOT a Farkas infeasibility ray extracted
  // from an infeasible-status solver state, but rather a
  // sensitivity dual from an optimal solution of the relaxed clone.
  // (PLP's legacy code dates back to an attempt to extract a solver-
  // provided Farkas ray; the current form uses the optimal dual on
  // the fixing row, which plays an analogous role in the cut.)
  //
  // A single cut per relaxed link (D6).  The cut's DIRECTION is
  // determined by the sign of `π`:
  //   π > 0  ⟹  source_col ≥ trial_extended  (tightening lower)
  //   π < 0  ⟹  source_col ≤ trial_extended  (tightening upper)
  // We express the cut in the LP as `π · source_col ≥ rhs` with
  // signed coefficient; the solver interprets the direction from
  // the sign.  Replaces the previous two-cuts-per-link
  // `1.0 · source_col ≥ dep_val_phys` / `… ≤ dep_val_phys` form
  // (D1 + D2 + D6).
  //
  // `trial + sup − sdn` is the LP value of `dep` at the elastic
  // optimum (directly from the fixing equation).  All duals are
  // read after a crossover solve (`elastic_opts.crossover = true`
  // in `SDDPMethod::elastic_solve`) so they are vertex-quality.
  //
  // Outward epsilon perturbation (D8), scaled by `niter` — the number
  // of times this (scene, phase) has already emitted a feasibility cut
  // in the current iteration.  `kFactEps = 1e-10 · niter` starts at 0
  // on the first emission (raw RHS) and escalates on each repeat so
  // the master escapes a degenerate LP vertex that keeps regenerating
  // the same cut.  Pathological cases (juan/gtopt_iplp) used to emit
  // the same cut hundreds of times with a fixed small ε; the
  // niter-scaled form lets the perturbation grow until the master
  // moves.  Matches PLP's escalating retry strategy.
  //
  // A cut whose |π| falls below `cut_coeff_eps` is dropped — tiny
  // coefficients pollute the LP with near-zero rows.
  const double kFactEps = 1e-10 * static_cast<double>(niter);

  // Use the same conventions as `build_feasibility_cut_physical`
  // above: physical-space row duals (`get_row_dual()`) and
  // physical trial values (`state_var->col_sol_physical()`), so
  // `add_row` on the prev-phase LP can fold col_scales + row-max
  // equilibration uniformly.
  auto duals = elastic.clone.get_row_dual();
  const auto sol_raw = elastic.clone.get_col_sol_raw();
  const auto& link_infos = elastic.link_infos;

  for (const auto& [info, link] : std::views::zip(link_infos, links)) {
    if (!info.relaxed) {
      continue;
    }
    if (info.fixing_row == RowIndex {unknown_index}) {
      continue;
    }
    const double pi = -duals[info.fixing_row];
    if (std::abs(pi) < cut_coeff_eps) {
      continue;
    }

    // dx = sdn_val - sup_val in slack-col raw LP units (slacks are
    // added with col_scale = 1, so raw == physical for the slack
    // itself).  Our fixing equation is `dep + sup − sdn = v̂`, so
    // at the elastic optimum `dep_clone_lp = v̂_lp + dx` where
    // `dx = sdn − sup`.  To lift `dep_clone` into physical units
    // we multiply by the state variable's var_scale (source-col
    // scale, equal to dep-col scale for shared state variables):
    //     dep_clone_phys = v̂_phys + dx · var_scale
    // Prior to 2026-04-22 this code used `v̂_phys + dx` directly,
    // silently miscomputing the cut RHS when var_scale ≠ 1 (e.g.
    // RALCO / ELTORO at √10).
    const double sup_val = (info.sup_col != ColIndex {unknown_index})
        ? sol_raw[info.sup_col]
        : 0.0;
    const double sdn_val = (info.sdn_col != ColIndex {unknown_index})
        ? sol_raw[info.sdn_col]
        : 0.0;
    const double dx = sdn_val - sup_val;

    // Relative low-activation filter: drop cuts where the cut's shift
    // from the trial state is below `1e-12 × (|π · v̂| + 1e-8)`
    // (several orders above machine-ε relative to RHS magnitude).
    // The cut has the form `pi · source_col ≥ pi · (v̂ + dx · var_scale)`,
    // so the effective push of the cut (relative to its own RHS) is
    // `pi · dx / (pi · v̂ + eps) ≈ dx / v̂`.  When below the threshold
    // the cut shift is beneath meaningful precision and the cut is
    // effectively redundant.
    //
    // PLP's analogous filter (`osicallsc.cpp:727-730`) uses the
    // additive form `(|b|+1e-8) · FactEps > |dx|`.
    {
      const double rhs_scale =
          (std::abs(pi) * std::abs(link.trial_value)) + 1e-8;
      if (std::abs(pi * dx) < 1e-12 * rhs_scale) {
        continue;
      }
    }

    const double v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    const double dep_clone_phys = v_hat_phys + (dx * link.var_scale);

    // Cut: pi · source_col ≥ pi · dep_clone_phys + outward ε · |rhs|
    const double rhs_base = pi * dep_clone_phys;
    const double eps_term = kFactEps * std::abs(rhs_base);
    const double rhs = rhs_base + ((pi > 0.0) ? eps_term : -eps_term);

    // Defensive clamp to `[source_low, source_upp]` — keeps the cut
    // feasible when `dep_clone_phys` falls outside the source
    // column's physical box (happens when `relax_fixed_state_variable`
    // sets slack bounds to DblMax via the `source_low == source_upp`
    // or `var_scale == 0` fallbacks).  Without this clamp, cuts can
    // become hard-infeasible, breaking backtracking recovery.
    //
    // The clamp alone WAS the juan/gtopt_iplp emax-pinning bug when
    // combined with dx ≈ 0 — the cut collapsed to `source ≥ emax`.
    // The low-activation filter above (|π · dx| < 1e-16 · RHS) drops
    // those degenerate cases before they reach this clamp, so the
    // clamp now only bites when `dep_clone_phys` legitimately exceeds
    // the box — matching PLP's behaviour via its filter sequencing.
    const double implied_bound = rhs / pi;
    const double clamped_bound =
        std::clamp(implied_bound, link.source_low, link.source_upp);
    const double clamped_rhs = pi * clamped_bound;

    auto cut = SparseRow {
        .lowb = clamped_rhs,
        .uppb = LinearProblem::DblMax,
        .class_name = link.class_name,
        .constraint_name = "mcut",
        .variable_uid = link.uid,
        .context = context,
    };
    cut[link.source_col] = pi;
    cuts.push_back(std::move(cut));
  }

  return cuts;
}

// ─── Cut averaging ──────────────────────────────────────────────────────────

auto average_benders_cut(const std::vector<SparseRow>& cuts) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() == 1) {
    return cuts.front();
  }

  const auto n = static_cast<double>(cuts.size());

  // Collect all column indices that appear in any cut.
  // Use the first cut's column count as a lower-bound size hint —
  // all cuts typically share most columns.
  flat_map<ColIndex, double> avg_coeffs;
  map_reserve(avg_coeffs, cuts.front().cmap.size());
  double avg_rhs = 0.0;

  for (const auto& cut : cuts) {
    avg_rhs += cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      avg_coeffs[col] += coeff;
    }
  }

  auto result = SparseRow {
      .lowb = avg_rhs / n,
      .uppb = LinearProblem::DblMax,
      .scale = cuts.front().scale,
  };

  for (const auto& [col, total_coeff] : avg_coeffs) {
    result[col] = total_coeff / n;
  }

  return result;
}

auto weighted_average_benders_cut(const std::vector<SparseRow>& cuts,
                                  const std::vector<double>& weights)
    -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() != weights.size()) {
    SPDLOG_WARN(
        "weighted_average_benders_cut: cuts.size()={} != weights.size()={}, "
        "returning empty cut",
        cuts.size(),
        weights.size());
    return {};
  }

  // Compute the total weight for normalisation
  double total_weight = 0.0;
  for (const double w : weights) {
    total_weight += w;
  }

  if (total_weight <= 0.0) {
    return {};
  }

  // Single-cut shortcut (avoid unnecessary work)
  if (cuts.size() == 1) {
    return cuts.front();
  }

  flat_map<ColIndex, double> avg_coeffs;
  map_reserve(avg_coeffs, cuts.front().cmap.size());
  double avg_rhs = 0.0;

  for (const auto& [cut, weight] : std::views::zip(cuts, weights)) {
    const double w = weight / total_weight;
    avg_rhs += w * cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      avg_coeffs[col] += w * coeff;
    }
  }

  auto result = SparseRow {
      .lowb = avg_rhs,
      .uppb = LinearProblem::DblMax,
      .scale = cuts.front().scale,
  };

  for (const auto& [col, coeff] : avg_coeffs) {
    result[col] = coeff;
  }

  return result;
}

auto accumulate_benders_cuts(const std::vector<SparseRow>& cuts) -> SparseRow
{
  if (cuts.empty()) {
    return {};
  }
  if (cuts.size() == 1) {
    return cuts.front();
  }

  // Accumulate (sum) all cuts: no division by count or weight normalisation
  flat_map<ColIndex, double> sum_coeffs;
  map_reserve(sum_coeffs, cuts.front().cmap.size());
  double sum_rhs = 0.0;

  for (const auto& cut : cuts) {
    sum_rhs += cut.lowb;
    for (const auto& [col, coeff] : cut.cmap) {
      sum_coeffs[col] += coeff;
    }
  }

  auto result = SparseRow {
      .lowb = sum_rhs,
      .uppb = LinearProblem::DblMax,
      .scale = cuts.front().scale,
  };

  for (const auto& [col, coeff] : sum_coeffs) {
    result[col] = coeff;
  }

  return result;
}

// ─── BendersCut class ────────────────────────────────────────────────────────

auto BendersCut::elastic_filter_solve(const LinearInterface& li,
                                      std::span<const StateVarLink> links,
                                      double penalty,
                                      const SolverOptions& opts)
    -> std::optional<ElasticSolveResult>
{
  if (m_pool_ == nullptr) {
    // No pool available: delegate directly to the free function.
    auto result = gtopt::elastic_filter_solve(li, links, penalty, opts);
    if (result.has_value()) {
      m_infeasible_cut_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  // Clone the LP; modifications don't touch the original.
  auto cloned = li.clone();

  ElasticSolveResult result;
  result.link_infos.reserve(links.size());

  bool modified = false;
  for (const auto& link : links) {
    auto info = relax_fixed_state_variable(
        cloned, link, link.target_phase_index, penalty);
    modified |= info.relaxed;
    result.link_infos.push_back(info);
  }

  if (!modified) {
    return std::nullopt;
  }

  // Transfer ownership of the cloned LP to a shared_ptr captured by value
  // in the pool task lambda.  This guarantees the clone's lifetime extends
  // until after pool.get() returns, regardless of when the task is scheduled.
  auto cloned_sp = std::make_shared<LinearInterface>(std::move(cloned));

  // Submit the LP solve to the work pool so that the pool's scheduling and
  // monitoring infrastructure (CPU load, active workers) observe the solve.
  // The calling thread blocks on the future until the solve completes.
  auto fut = m_pool_->submit([cloned_sp, opts]() -> std::expected<int, Error>
                             { return cloned_sp->resolve(opts); });

  bool solved = false;
  if (fut.has_value()) {
    auto r = fut.value().get();
    solved = r.has_value() && cloned_sp->is_optimal();
  } else {
    // Pool submission failed (e.g. pool shut down): fall back to direct solve.
    SPDLOG_WARN(
        "BendersCut::elastic_filter_solve: pool submit failed, "
        "falling back to direct solve");
    auto r = cloned_sp->resolve(opts);
    solved = r.has_value() && cloned_sp->is_optimal();
  }

  if (solved) {
    m_infeasible_cut_count_.fetch_add(1, std::memory_order_relaxed);
    SPDLOG_TRACE(
        "BendersCut::elastic_filter_solve: solved clone via pool "
        "(obj={:.4f}), total_infeasible_cuts={}",
        cloned_sp->get_obj_value(),
        m_infeasible_cut_count_.load(std::memory_order_relaxed));
    result.clone = std::move(*cloned_sp);
    return result;
  }

  return std::nullopt;
}

auto BendersCut::build_feasibility_cut(const LinearInterface& li,
                                       ColIndex alpha_col,
                                       std::span<const StateVarLink> links,
                                       double penalty,
                                       const SolverOptions& opts,
                                       double /*scale_alpha*/,
                                       double /*scale_objective*/)
    -> std::optional<FeasibilityCutResult>
{
  auto elastic = this->elastic_filter_solve(li, links, penalty, opts);
  if (!elastic.has_value()) {
    return std::nullopt;
  }

  // See the free-function `build_feasibility_cut` above for rationale.
  auto cut =
      build_benders_cut_physical(alpha_col,
                                 links,
                                 elastic->clone,
                                 elastic->clone.get_obj_value_physical());

  return FeasibilityCutResult {
      .cut = std::move(cut),
      .elastic = std::move(*elastic),
  };
}

}  // namespace gtopt
