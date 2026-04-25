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

void BoxEdgeStats::tally(double clamped, const StateVarLink& link)
{
  // Decile band width — anything within 10 % of either edge of the
  // source box ``[source_low, source_upp]`` counts as "at the edge".
  // The fallback ``std::max(1.0, …)`` keeps the band well-defined
  // even when the source bounds collapse to a single point (zero
  // box_width) — typical in test fixtures that haven't populated
  // physical bounds.
  const double box_width = std::max(1.0, link.source_upp - link.source_low);
  const double tol = 0.10 * box_width;
  if (clamped >= link.source_upp - tol) {
    ++upper;
    if (!link.name.empty()) {
      if (!upper_names.empty()) {
        upper_names += ", ";
      }
      upper_names += link.name;
    }
  } else if (clamped <= link.source_low + tol) {
    ++lower;
  } else {
    ++inside;
  }
}

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

  // Box-edge degeneracy diagnostic.  See `BoxEdgeStats` for the
  // tally semantics: every active link is bucketed into upper / lower
  // / inside relative to its source box ``[source_low, source_upp]``,
  // and a WARN fires when every active link clamps at the upper
  // edge (= the cascade-infeasibility signature).
  BoxEdgeStats box_stats;

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

    // PLP parity (osicallsc.cpp:727-730): drop the link when the slack
    // activation |dx| is below `(|trial| + 1e-8) × cut_coeff_eps`.
    if ((std::abs(link.trial_value) + 1e-8) * cut_coeff_eps > std::abs(dx)) {
      continue;
    }

    // dx is in slack-col raw LP units (slacks added with col_scale=1);
    // lift to physical via the dep column's *effective* LP-to-physical
    // scale.  We use `rc_source.get_col_scale(dependent_col)` rather
    // than `link.var_scale` because equilibration (ruiz in particular)
    // applies an additional column scale ON TOP of the user's var_scale
    // — using var_scale alone silently miscomputes the cut RHS whenever
    // the LP is ruiz-equilibrated.  Under `row_max` / `none` the col
    // scale equals var_scale, so this is a no-op there.
    const double v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    const double dep_scale_phys = rc_source.get_col_scale(link.dependent_col);
    const double dep_clone_phys = v_hat_phys + (dx * dep_scale_phys);

    // Per-link defensive clamp — same role as the box-edge guard in
    // `build_multi_cuts`.  Without it, an aggregated cut whose RHS
    // depends on a `dep_clone_phys` slightly outside `[source_low,
    // source_upp]` (typical when the elastic relaxation pushed a
    // bound to fix infeasibility) becomes hard-infeasible because no
    // master value of `source_col` can satisfy `pi · source ≥ pi ·
    // (out-of-box value)`.  Clamping per link keeps each contribution
    // within the achievable max LHS of the aggregate.
    const double clamped =
        std::clamp(dep_clone_phys, link.source_low, link.source_upp);

    box_stats.tally(clamped, link);

    row[link.source_col] = pi;
    row.lowb += pi * clamped;
  }

  if (box_stats.all_upper_degenerate()) {
    SPDLOG_WARN(
        "build_feasibility_cut_physical: degenerate cut — "
        "all {} contributing links clamp at source_upp; "
        "RHS floors every state at its physical max.  Likely "
        "cascade-infeasibility on next master retry. "
        "Reservoirs: [{}]",
        box_stats.total(),
        box_stats.upper_names);
  } else if (box_stats.total() > 0) {
    SPDLOG_DEBUG(
        "build_feasibility_cut_physical: cut box-edge stats — "
        "upper={} lower={} inside={} (of {} active links)",
        box_stats.upper,
        box_stats.lower,
        box_stats.inside,
        box_stats.total());
  }

  // PLP parity (plp-agrespd.f:722): flat FactEPS-relative outward
  // perturbation on the aggregated RHS.  PLP applies `rhs += FactEPS ·
  // |rhs|` unconditionally (same in multi-cut branch, :791).  The
  // niter-escalating form we used previously was a gtopt-local
  // heuristic for juan/gtopt_iplp degeneracy; PLP achieves the same
  // via the dx filter above catching repeat degenerate activations.
  // niter parameter retained for signature stability; ignored here.
  (void)niter;
  // PLP's FactEPS = 1e-8 (getopts.f:231) applied as `rhs += FactEPS·|rhs|`
  // (plp-agrespd.f:722, 791) is designed for PLP's unit ray magnitudes
  // where the resulting LP residual is ε·|rhs| = O(1e-6) — just within
  // typical solver tolerance.  gtopt's duals can be much larger (pi ≈
  // 210 on RALCO at juan/gtopt_iplp p11 because dep is coupled through
  // downstream constraints), so the same formula produces residuals
  // ≈ ε·pi·|dep| that trip CPLEX's FeasibilityTol=1e-6.
  //
  // Set to 0 here: the per-link clamp below keeps `dep_clone_phys`
  // inside `[source_low, source_upp]`, so the cut sits AT the box edge
  // rather than slightly OUTSIDE it — LP solver tolerance absorbs any
  // residual numerical drift on the basis side.  If cycling on the
  // same cut becomes an issue in practice, re-introduce an ABSOLUTE
  // (not relative) 1e-8 bump scaled by 1/pi.
  constexpr double kFactEps = 0.0;
  row.lowb += kFactEps * std::abs(row.lowb);

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
  // PLP parity (osicallsc.cpp:658): slack obj = 1.0 flat for every
  // relaxed state, regardless of per-variable business-cost hints.
  // `link.scost` carries the business `state_fail_cost` (default
  // 5000 in production cases) — NOT a Chinneck Phase-1 feasibility
  // price.  Using it here amplified the ray magnitude 5000× and
  // produced emax-pinning multi-cuts that violated LP tolerance by
  // `1e-8 × pi × rhs ≈ 0.03` on juan/gtopt_iplp p27.  The pure
  // `penalty` path matches PLP's AgrElastici call site
  // (plp-agrespd.f:673 passes `objs = 0` so every slack receives the
  // default 1.0).  `link.scost` retained on the struct for non-SDDP
  // consumers that may use it as a true business-cost hint.
  (void)link.scost;  // intentionally unused here
  const double slack_cost = penalty;
  // For the D3 slack-bound computation below we use the dep column's
  // *effective* LP-to-physical scale (reused in the ruiz case).  Under
  // `row_max` / `none` this equals `var_scale`; under `ruiz` it
  // additionally carries the ruiz-added factor.
  const double dep_scale_phys_raw = li.get_col_scale(dep);
  const double dep_scale_phys =
      (dep_scale_phys_raw > 0.0 && dep_scale_phys_raw != 1.0)
      ? dep_scale_phys_raw
      : link.var_scale;

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
  //
  // Use the dep column's *effective* LP-to-physical scale (reused
  // from the slack-cost computation above) when converting the
  // physical `source_{low,upp}` bounds into LP units.  Under
  // `row_max` / `none` this equals `var_scale`; under `ruiz` it
  // additionally carries the ruiz-added factor, without which the
  // slack bounds would be off by the ruiz factor and the elastic
  // clone would become spuriously infeasible.
  const bool have_finite_box =
      (dep_scale_phys > 0.0) && (link.source_upp > link.source_low);
  const double vs = have_finite_box ? dep_scale_phys : 1.0;
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

  // Symmetric slack cost — PLP convention (``osicallsc.cpp:658``):
  // both ``sup`` and ``sdn`` are priced at the same ``slack_cost``
  // (= 1.0 by default in PLP, ``penalty`` here).  Asymmetric biasing
  // was tested on juan/gtopt_iplp (``kSdnUpwardBias = 1e-3`` aiming
  // to break the emax-pinning vertex) but did NOT change CPLEX's
  // vertex selection because the chosen optimum is structurally
  // determined by p28's tightness, not a tiebreak.  Reverted to
  // PLP-equivalent symmetric pricing so the elastic clone behaves
  // identically to PLP's ``osi_lp_get_feasible_cut`` modulo
  // gtopt's stricter row writer.
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
  const double kFactEps = 0.01 * cut_coeff_eps * static_cast<double>(niter);

  // Use the same conventions as `build_feasibility_cut_physical`
  // above: physical-space row duals (`get_row_dual()`) and
  // physical trial values (`state_var->col_sol_physical()`), so
  // `add_row` on the prev-phase LP can fold col_scales + row-max
  // equilibration uniformly.
  auto duals = elastic.clone.get_row_dual();
  const auto sol_raw = elastic.clone.get_col_sol_raw();
  // Per-column raw objective coefficients on the elastic clone.
  // The slack columns ``info.sup_col`` / ``info.sdn_col`` were
  // priced at relaxation time by ``relax_fixed_state_variable``
  // (``slack_cost_sup = penalty`` and
  // ``slack_cost_sdn = penalty × (1 + kSdnUpwardBias)``), so reading
  // the cost back from the clone is the canonical source of truth —
  // it captures the actual per-direction asymmetric bias plus any
  // future per-link variation, with no plumbing through the call
  // chain.  Only the global ``penalty`` parameter is still passed
  // in (used as a sanity baseline); per-link costs override it
  // below.
  const auto col_costs = elastic.clone.get_col_cost_raw();
  const auto& link_infos = elastic.link_infos;

  // Box-edge degeneracy diagnostic + drop-degenerate guard.  See
  // `BoxEdgeStats` for the per-link tally semantics.  When every
  // emitted cut clamps at the upper edge (top 10 % of each
  // ``[source_low, source_upp]`` box), the family is degenerate —
  // it floors every state at emax, which the master cannot reach
  // in one phase if the predecessor's solved efin sits anywhere
  // meaningfully below.  We DROP that family at the end of this
  // function (see post-loop guard) so the caller can declare
  // infeasibility cleanly instead of installing rows that will
  // cascade-fail on the next forward attempt.
  BoxEdgeStats box_stats;

  for (const auto& [info, link] : std::views::zip(link_infos, links)) {
    if (!info.relaxed) {
      continue;
    }
    if (info.fixing_row == RowIndex {unknown_index}) {
      continue;
    }
    const double pi = -duals[info.fixing_row];

    // Per-link slack cost — read from the clone's slack columns so
    // the filter threshold automatically picks up whatever
    // ``relax_fixed_state_variable`` set, including the asymmetric
    // ``sdn`` upward bias (``slack_cost_sdn = penalty × (1 + 1e-3)``)
    // and any future ``var_scale``-aware pricing (e.g.
    // ``penalty × var_scale`` for per-physical-unit cost).
    //
    // **Unit alignment with ``pi``**: ``pi = -duals[fixing_row]`` is
    // returned by ``get_row_dual()`` which delivers *physical* duals
    // (raw_dual × row_scale × … — see LinearInterface).
    // ``get_col_cost_raw()`` returns raw-LP-space costs.  We promote
    // the raw cost to physical via ``cost_phys = raw_cost ×
    // get_col_scale(col)``.  For the slack columns this is a no-op
    // today (they were added with the default ``col_scale = 1``),
    // but writing the conversion explicitly keeps the comparison
    // dimensionally consistent and robust if a future change ever
    // assigns a non-unit scale to slack columns (e.g. ruiz
    // equilibration).
    //
    // The dual ``pi`` is bounded by ``[-slack_cost_sdn_phys,
    // +slack_cost_sup_phys]`` at the elastic optimum, so a
    // meaningful "non-trivial pi" threshold scales with the larger
    // of the two physical slack costs.  ``default_penalty = 1.0``
    // matches the PLP convention (``osicallsc.cpp:658``: ``obj=1.0``
    // flat) and is the fallback when either slack column is absent
    // (defensive — normally both slacks are added by
    // ``relax_fixed_state_variable``).
    constexpr double default_penalty = 1.0;
    const auto slack_cost_phys =
        [&col_costs, &elastic](ColIndex slack_col) noexcept -> double
    {
      if (slack_col == ColIndex {unknown_index}) {
        return default_penalty;
      }
      const double col_scale = elastic.clone.get_col_scale(slack_col);
      return std::abs(col_costs[slack_col]) * col_scale;
    };
    const double slack_cost_sup = slack_cost_phys(info.sup_col);
    const double slack_cost_sdn = slack_cost_phys(info.sdn_col);
    const double slack_cost_max = std::max(slack_cost_sup, slack_cost_sdn);

    if (std::abs(pi) < cut_coeff_eps * slack_cost_max) {
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

    // PLP-style additive low-activation filter
    // (``osicallsc.cpp:727-730``):
    //
    //   if |dx| < (|trial| + 1e-8) × cut_coeff_eps  ⇒  drop
    //
    // Filter form copied verbatim from PLP's
    // ``osi_lp_get_feasible_cut`` to preserve cut-emission parity
    // with PLP's reference implementation.  The threshold operates
    // on the slack activation ``|dx|`` directly (NOT on the cut RHS
    // contribution ``|π·dx|``), so its scale matches what PLP
    // measures and the same per-link cuts are dropped.  Replaces
    // the previous gtopt-local relative form
    // ``|π·dx| < 0.1 × cut_coeff_eps × (|π·v̂| + cut_coeff_eps)``
    // which over-filtered when ``|π|`` was small (the dual is
    // dimensioned with the slack cost; tiny ``|π|`` could survive
    // PLP's filter but be killed by the relative form).  ``+1e-8``
    // offset keeps the bound finite at ``trial = 0``.
    if (std::abs(dx) < (std::abs(link.trial_value) + 1e-8) * cut_coeff_eps) {
      continue;
    }

    // Lift dx (slack-col raw LP units) to physical via the dep column's
    // *effective* LP-to-physical scale, which includes any ruiz-added
    // factor on top of the user's `var_scale`.  Using `link.var_scale`
    // alone silently miscomputes the cut RHS under ruiz equilibration
    // (observed on juan/gtopt_iplp: elastic clone declared infeasible
    // because the lifted `dep_clone_phys` was off by the ruiz factor).
    const double v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    const double dep_scale_phys =
        elastic.clone.get_col_scale(link.dependent_col);
    const double dep_clone_phys = v_hat_phys + (dx * dep_scale_phys);

    // Cut: pi · source_col ≥ pi · dep_clone_phys + outward ε · |rhs|
    // (signed so the perturbation is always *outward* in the cut's
    // direction — for π > 0 the cut tightens lower so add +ε, for
    // π < 0 it tightens upper so subtract ε).
    const double rhs_base = pi * dep_clone_phys;
    const double eps_term = kFactEps * std::abs(rhs_base);
    const double rhs = rhs_base + ((pi > 0.0) ? eps_term : -eps_term);

    // Defensive clamp on the IMPLIED BOUND (`rhs / pi`), not on
    // `dep_clone_phys` directly — keeps the cut feasible against the
    // source column's physical box without collapsing the cut to the
    // box edge when the elastic clone hit the bound.  Concretely:
    //   - `clamped_bound = clamp(rhs/pi, source_low, source_upp)`
    //     bounds the implied source value to the achievable range.
    //   - `clamped_rhs = pi · clamped_bound` reconstructs the RHS
    //     after clamp.
    //
    // The previous form `clamp(dep_clone_phys, …)` collapsed the cut
    // to `source ≥ source_upp` (emax-pinning) when `dx ≈ 0` and
    // `dep_clone_phys ≈ source_upp`.  The relative low-activation
    // filter above drops those degenerate cases first; with that
    // filter in place the clamp here only bites when
    // `dep_clone_phys` *legitimately* exceeds the box (e.g. ULP
    // drift from LP roundoff on the elastic side), preserving cut
    // information instead of forcing emax equality.  PLP's analogous
    // clamp is commented out (osicallsc.cpp:744-751); gtopt keeps a
    // tighter form because the LP writer is stricter about row-LHS
    // vs column-box residuals at solver tolerance.
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

    box_stats.tally(clamped_bound, link);
  }

  if (box_stats.all_upper_degenerate()) {
    // Drop the entire family.  PLP's analogous path
    // (``osicallsc.cpp::osi_lp_get_feasible_cut``) silently emits
    // the same emax-pinning shape, but in gtopt the downstream
    // multi-cut install + retry chain cascades through every
    // upstream phase before the forward-pass attempt budget
    // exhausts (juan/gtopt_iplp: 28 backtracks p28→p1).  Clearing
    // ``cuts`` lets the caller in
    // ``sddp_forward_pass.cpp::forward_pass`` see "no feasibility
    // cut produced" and declare the scene infeasible immediately,
    // skipping the wasted retries.
    SPDLOG_WARN(
        "build_multi_cuts: degenerate cut family — all {} per-link "
        "cuts clamp at source_upp (top 10 %% of each box).  RHS "
        "would floor every state at its physical max; cascade-"
        "infeasibility on next master retry guaranteed.  DROPPING "
        "the family so the caller declares infeasibility cleanly. "
        "Reservoirs: [{}]",
        box_stats.total(),
        box_stats.upper_names);
    cuts.clear();
  } else if (box_stats.total() > 0 && box_stats.upper > 0) {
    SPDLOG_DEBUG(
        "build_multi_cuts: cut box-edge stats — upper={} lower={} "
        "inside={} (of {} active links) — partial degeneracy",
        box_stats.upper,
        box_stats.lower,
        box_stats.inside,
        box_stats.total());
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
