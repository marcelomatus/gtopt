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
                                    double cut_coeff_eps) -> SparseRow
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

    const double v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    row[link.source_col] = pi;
    row.lowb += pi * (v_hat_phys + dx);
  }

  return row;
}

// ─── Elastic filter ─────────────────────────────────────────────────────────

RelaxedVarInfo relax_fixed_state_variable(
    LinearInterface& li,
    const StateVarLink& link,
    [[maybe_unused]] PhaseIndex phase_index,
    [[maybe_unused]] double penalty)
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

  // Chinneck Phase-1 feasibility LP: unit-cost slack variables.  The
  // caller (`elastic_filter_solve`) has zeroed every original
  // objective coefficient on the clone, so the relaxed LP's optimum
  // equals Σ(s⁺ + s⁻) — the pure feasibility gap.  Matches PLP
  // `plp-bc.f` and Chinneck (2008) § 4.  `penalty` retained for
  // signature stability; no longer consulted here.
  constexpr double slack_cost = 1.0;
  const auto sup = li.add_col(SparseCol {
      .uppb = DblMax,
      .cost = slack_cost,
  });

  const auto sdn = li.add_col(SparseCol {
      .uppb = DblMax,
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

auto build_multi_cuts(const ElasticSolveResult& elastic,
                      std::span<const StateVarLink> links,
                      const LpContext& context,
                      double slack_tol) -> std::vector<SparseRow>
{
  std::vector<SparseRow> cuts;

  // Physical-space bound cuts.  `dep_sol_phys = LP × col_scale` puts
  // dependent-column values in physical units on the elastic clone's
  // LP; the resulting cut `source_col ≤ dep_val_phys` is in the same
  // units and `add_row` on the src LP folds the source column's
  // col_scale + row-max equilibration automatically.
  //
  // Slack column values (`sup`/`sdn`) are read in *raw LP* via
  // `get_col_sol_raw` because the slack tolerance threshold
  // `slack_tol` is defined against the LP units the elastic solve
  // emits; they are only used to decide whether to emit the cut, not
  // for its coefficients.
  const auto dep_sol_phys = elastic.clone.get_col_sol();  // ScaledView
  const auto& dep_sol_raw = elastic.clone.get_col_sol_raw();
  const auto& link_infos = elastic.link_infos;

  // Each multi-cut row bounds a specific state-variable column, so
  // the per-element identity (class_name + uid) disambiguates row
  // labels across iterations and across element classes.  Uids are
  // unique only within a class, so using `link.uid` alone would let
  // e.g. Reservoir uid=1 and LngTerminal uid=1 collide.  We pair uid
  // with `link.class_name` (captured at link collection time from
  // the state-variable registry Key) so the composed label is
  // globally unique.  Both `class_name` and `uid` are stable for the
  // full solver lifetime — the class_name string_view references the
  // registry Key's storage, which outlives every cut produced here.
  for (const auto& [info, link] : std::views::zip(link_infos, links)) {
    if (!info.relaxed) {
      continue;
    }
    const double dep_val_phys_raw = dep_sol_phys[link.dependent_col];

    // Clamp the bound-cut RHS to the source column's PHYSICAL feasible
    // box `[source_low, source_upp]` captured at link-collection time
    // (see `SDDPMethod::collect_state_variable_links` in
    // sddp_method.cpp and `StateVarLink::source_{low,upp}` in
    // benders_cut.hpp — both in physical units after the scale-bug fix
    // in commit 0a45e52b).
    //
    // Without this clamp, the elastic clone's unbounded slack solution
    // can push the relaxed dependent variable outside the source
    // column's physical box and emit an mcut of the form
    //     source_col ≥ dep_val_phys  with dep_val_phys > source_upp_phys
    // — impossible to satisfy because the column is physically bounded
    // at `source_upp_phys` (= emax).  Diagnosed on plp_2_years phase
    // 27/28 where the elastic filter produced
    //     reservoir_mcut_lb_65_*: reservoir_energy_65_* ≥ 917.47 LP
    // against a column capped at emax = 371.03 LP, causing the next
    // forward-pass attempt on phase 27 to be strictly infeasible.
    //
    // Sanitization rules:
    //   - `mcut_ub` (source ≤ dep_val_phys): take dep_val_phys =
    //     max(dep_val_phys, source_low).  If dep_val_phys ≥ source_upp
    //     the cut is trivially non-binding (source's upper bound is
    //     already tighter) → skip emission.
    //   - `mcut_lb` (source ≥ dep_val_phys): take dep_val_phys =
    //     min(dep_val_phys, source_upp).  If dep_val_phys ≤ source_low
    //     the cut is trivially non-binding → skip emission.
    //
    // A cut that collapses to the column's own bound is still valid
    // but redundant with the existing column bound; skipping it keeps
    // the LP lean and avoids the row-name collision diagnostic noise
    // from "Duplicate LP row metadata" detectors.
    const double src_low_phys = link.source_low;
    const double src_upp_phys = link.source_upp;

    // sup > 0 ⟹ solution < trial_value ⟹ source ≤ dep_val
    if (info.sup_col != ColIndex {unknown_index}) {
      const double sup_val = dep_sol_raw[info.sup_col];
      if (sup_val > slack_tol) {
        // Clamp below by source_low.  `ub == src_upp` is redundant
        // with the column's own upper bound, so skip emission there.
        const double ub = std::max(dep_val_phys_raw, src_low_phys);
        if (ub < src_upp_phys) {
          auto ub_cut = SparseRow {
              .lowb = -LinearProblem::DblMax,
              .uppb = ub,
              .class_name = link.class_name,
              .constraint_name = "mcut_ub",
              .variable_uid = link.uid,
              .context = context,
          };
          ub_cut[link.source_col] = 1.0;
          cuts.push_back(std::move(ub_cut));
        }
      }
    }

    // sdn > 0 ⟹ solution > trial_value ⟹ source ≥ dep_val
    if (info.sdn_col != ColIndex {unknown_index}) {
      const double sdn_val = dep_sol_raw[info.sdn_col];
      if (sdn_val > slack_tol) {
        // Clamp above by source_upp.  `lb == src_low` is redundant
        // with the column's own lower bound, so skip emission there.
        const double lb = std::min(dep_val_phys_raw, src_upp_phys);
        if (lb > src_low_phys) {
          auto lb_cut = SparseRow {
              .lowb = lb,
              .uppb = LinearProblem::DblMax,
              .class_name = link.class_name,
              .constraint_name = "mcut_lb",
              .variable_uid = link.uid,
              .context = context,
          };
          lb_cut[link.source_col] = 1.0;
          cuts.push_back(std::move(lb_cut));
        }
      }
    }
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
