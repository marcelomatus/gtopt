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

// SPDLOG_ACTIVE_LEVEL must be set BEFORE any header that transitively
// includes <spdlog/spdlog.h> — otherwise the SPDLOG_TRACE macro is
// baked to `(void)0` for this whole translation unit and runtime
// `set_level(trace)` cannot recover the compiled-out calls.
//
// CAVEAT (PCH builds): `cmake_local/PrecompiledHeaders.cmake` includes
// <spdlog/spdlog.h> in the PCH, so this #define is INERT there and the
// SPDLOG_TRACE / SPDLOG_DEBUG macros stay compiled out at INFO.  Any
// diagnostic that must be recoverable at runtime has to use the
// function form (`spdlog::debug(...)` / `spdlog::trace(...)`), which
// checks the runtime level instead.
#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <cmath>
#include <ranges>

#include <gtopt/benders_cut.hpp>
#include <gtopt/constraint_names.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/work_pool.hpp>
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
  // Collapse the per-link `set_col_low_raw + set_col_upp_raw` pair into a
  // single bulk dispatch.  Same N → 1 backend-round-trip motivation as
  // `apply_post_load_replay` — matters on cases with many forward-pass
  // dep_col pins (production juan/IPLP, RALCO/sini chains).  The bulk
  // path applies the same validation, replay-buffer capture, and
  // cached-optimal invalidation as the per-element setters.
  const auto n = links.size();
  std::vector<ColIndex> idx;
  std::vector<char> lu;
  std::vector<double> values;
  idx.reserve(n);
  lu.reserve(n);
  values.reserve(n);
  for (auto& link : links) {
    link.trial_value = source_solution[link.source_col];
    idx.push_back(link.dependent_col);
    lu.push_back('B');  // pin both lower and upper to the trial value
    values.push_back(link.trial_value);
  }
  target_li.set_col_bounds_raw(idx, lu, values);
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
  //
  // Routed through the bulk `set_col_bounds` so the N per-link
  // (set_col_low + set_col_upp) pairs collapse to a single bulk
  // dispatch — same motivation as the raw overload above.  The phys
  // bulk path also passes ±DblMax / ±solver-infinity through
  // unchanged, which the singular `set_col_low/upp(phys)` could not.
  const auto n = links.size();
  std::vector<ColIndex> idx;
  std::vector<char> lu;
  std::vector<double> phys_values;
  idx.reserve(n);
  lu.reserve(n);
  phys_values.reserve(n);
  for (auto& link : links) {
    const double v_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    idx.push_back(link.dependent_col);
    lu.push_back('B');
    phys_values.push_back(v_phys);
    // Keep `trial_value` in dependent-column raw LP units so
    // downstream cut-builders (which compare against the bound
    // snapshot in `StateVarLink::source_{low,upp}` also captured in
    // raw) see a self-consistent value.  Hoist the descale once —
    // `set_col_bounds(phys)` below also divides by `col_scale` but
    // does it internally on the cached vector, so a second division
    // here is unavoidable yet computed only once per link.
    link.trial_value = v_phys / target_li.get_col_scale(link.dependent_col);
  }
  target_li.set_col_bounds(idx, lu, phys_values);
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
  // and `target_li.get_obj_value()` return at the call site.
  auto row = SparseRow {
      .lowb = objective_value_physical,
      .uppb = LinearProblem::DblMax,
      .pivot_col = alpha_col,  // ← α-pivot equilibration in add_row step 3
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

  // `pivot_col = alpha_col` tells `add_row`'s step-3 equilibration to
  // normalize by α's coefficient instead of row-max.  Keeps α at 1.0
  // in LP-space and pushes the dynamic range into the state-link
  // coefficients (which don't drive κ when α is the basic pivot).
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
      .pivot_col = alpha_col,  // ← α-pivot equilibration in add_row step 3
  };
  row[alpha_col] = 1.0;

  // Hoist the trace-level check outside the link loop.  At juan/IPLP
  // scale this loop iterates ~50 times per cut × ~8 apertures × ~50
  // phases × 16 scenes per backward pass — emitting a TRACE line per
  // link materialises ~320K `std::format` argument lists per iteration
  // even when trace is OFF (the common case).  spdlog's runtime gate
  // happens AFTER argument formatting, so guard explicitly.
  const bool trace_on = spdlog::should_log(spdlog::level::trace);
  double max_abs_coeff = 0.0;
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
    max_abs_coeff = std::max(max_abs_coeff, std::abs(rc_phys));

    if (trace_on) {
      // (DIAG 2026-04-30) Cut-math instrumentation: print just the
      // physical values that actually enter the cut row.  rc_phys came
      // from `state_var->reduced_cost_physical(scale_obj)`; v_hat_phys
      // came from `state_var->col_sol_physical()`.  See the per-cut
      // summary line below for the loop-final aggregate.
      spdlog::trace(
          "build_benders_cut_physical[ovld1]: src_col={} dep_col={} "
          "rc_phys={:.6e} v_hat_phys={:.6e}",
          static_cast<int>(link.source_col),
          static_cast<int>(link.dependent_col),
          rc_phys,
          v_hat_phys);
    }
  }

  // Single per-cut summary line — emitted even when the per-link
  // detail is silenced (cheap: O(1) format args, fires O(cuts/iter)).
  if (trace_on) {
    spdlog::trace(
        "build_benders_cut_physical[ovld1]: alpha_col={} obj_phys={:.6e} "
        "→ row.lowb={:.6e} ({} links, max|coeff|={:.6e})",
        static_cast<int>(alpha_col),
        objective_value_physical,
        row.lowb,
        links.size(),
        max_abs_coeff);
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
      .pivot_col = alpha_col,  // ← α-pivot equilibration in add_row step 3
  };
  row[alpha_col] = 1.0;

  // Hoist trace gate outside the per-link loop — see ovld1 for the
  // ~320K-line/iter rationale at juan/IPLP scale.
  const bool trace_on = spdlog::should_log(spdlog::level::trace);
  double max_abs_coeff = 0.0;
  const auto rc_view = rc_source.get_col_cost();
  for (const auto& link : links) {
    const auto rc_phys = rc_view[link.dependent_col];
    // Guard against a non-finite reduced cost (same rationale as the dual
    // guard in build_feasibility_cut_physical: `std::abs(NaN) < eps` is false,
    // so a NaN would otherwise flow into `row.lowb` and yield a poisoned cut).
    if (!std::isfinite(rc_phys) || std::abs(rc_phys) < cut_coeff_eps) {
      continue;
    }
    const auto v_hat_phys =
        (link.state_var != nullptr) ? link.state_var->col_sol_physical() : 0.0;
    row[link.source_col] = -rc_phys;
    row.lowb -= rc_phys * v_hat_phys;
    max_abs_coeff = std::max(max_abs_coeff, std::abs(rc_phys));

    if (trace_on) {
      // (DIAG 2026-04-30) rc_phys: from `rc_source.get_col_cost()[dep_col]`
      // (ScaledView, physical $/unit); v_hat_phys: state_var cache.
      spdlog::trace(
          "build_benders_cut_physical[ovld2]: src_col={} dep_col={} "
          "rc_phys={:.6e} v_hat_phys={:.6e}",
          static_cast<int>(link.source_col),
          static_cast<int>(link.dependent_col),
          rc_phys,
          v_hat_phys);
    }
  }

  if (trace_on) {
    spdlog::trace(
        "build_benders_cut_physical[ovld2]: alpha_col={} obj_phys={:.6e} "
        "→ row.lowb={:.6e} ({} links, max|coeff|={:.6e})",
        static_cast<int>(alpha_col),
        objective_value_physical,
        row.lowb,
        links.size(),
        max_abs_coeff);
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

  // `views::zip` clamps to the shorter range automatically (same
  // semantics as the previous `std::min(links.size(), link_infos.size())`
  // loop bound), so no explicit `n` is needed.
  for (const auto& [link, info] : std::views::zip(links, link_infos)) {
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
    // NaN guard mirrors build_multi_cuts: a degenerate elastic re-solve
    // can return NaN duals under an optimal status, and `abs(NaN) < eps`
    // is false — without this the NaN lands in the fcut row and poisons the
    // LP (a NaN RHS is infeasible for every master value).
    if (!std::isfinite(pi)) {
      spdlog::warn(
          "build_feasibility_cut_physical: non-finite dual pi={} at "
          "fixing row {} — link skipped",
          pi,
          static_cast<int>(info.fixing_row));
      continue;
    }
    if (std::abs(pi) < cut_coeff_eps) {
      continue;
    }

    const double sup_val =
        info.sup_col != ColIndex {unknown_index} ? sol_raw[info.sup_col] : 0.0;
    const double sdn_val =
        info.sdn_col != ColIndex {unknown_index} ? sol_raw[info.sdn_col] : 0.0;
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

// ─── Strengthened Benders cut ───────────────────────────────────────────────

auto build_strengthened_benders_cut(ColIndex alpha_col,
                                    std::span<const StateVarLink> links,
                                    const LinearInterface& target_li,
                                    double cut_coeff_eps,
                                    const SolverOptions& lp_opts,
                                    const SolverOptions& mip_opts)
    -> std::optional<StrengthenedCutResult>
{
  // Clone route — same parallel-safe manual+replay gate as
  // `elastic_filter_solve` (see the long rationale there): the manual
  // path needs a decompressed snapshot AND an active replay buffer;
  // otherwise the native `clone()` (serialised by the process-global
  // clone mutex) is the correctness-safe fallback.
  //
  // TWO independent clones, one per solve:
  //
  //   * `lp_clone`  — integrality relaxed, state pins kept: the LP
  //     relaxation whose pinned reduced costs are the multipliers λ.
  //   * `mip_clone` — integrality INTACT (a fresh clone, so no
  //     `relax_integers` → `restore_integers` round-trip is needed),
  //     state pins relaxed to the box, objective charged −λ: the
  //     Lagrangian MIP.
  //
  // The two-clone split is deliberate: it avoids the
  // `relax_integers` → `restore_integers` round-trip on a live
  // backend (a problem-type flip plus per-column ctype rewrites whose
  // interaction with resident solve state differs per plugin) — the
  // fresh clone starts the Lagrangian MIP from clean solver state at
  // the cost of one extra clone, negligible next to the MIP solve.
  if (!target_li.has_integer_cols()) {
    // Callers gate on `has_integer_cols()`; defensive no-op (checked
    // BEFORE any clone is built) so a pure-LP cell silently falls back
    // to the legacy cut path without paying a clone.
    return std::nullopt;
  }

  const bool can_use_manual = target_li.has_decompressed_snapshot()
      && target_li.low_memory_mode() != LowMemoryMode::off;
  const auto make_clone = [&]() -> LinearInterface
  {
    return can_use_manual
        ? target_li.clone_from_flat(LinearInterface::CloneKind::shallow)
        : target_li.clone(LinearInterface::CloneKind::shallow);
  };

  auto lp_clone = make_clone();

  // ── Step 1: LP relaxation at the pinned trial state v̂ ────────────
  //
  // This is where the sound duals come from: a MIP re-solve has no
  // reduced costs (the CPLEX backend's CPXgetdj error is silently
  // ignored, leaving zeros/stale values — investigation doc §1), so
  // the relaxation must be explicit.
  lp_clone.relax_integers();
  auto r_lp = lp_clone.resolve(lp_opts);
  if (!r_lp.has_value() || !lp_clone.is_optimal()) {
    SPDLOG_WARN(
        "build_strengthened_benders_cut: LP-relaxation solve failed "
        "(status {}) — caller falls back to the legacy cut path",
        lp_clone.get_status());
    return std::nullopt;
  }
  const double z_lp = lp_clone.get_obj_value();
  if (!std::isfinite(z_lp)) {
    SPDLOG_WARN(
        "build_strengthened_benders_cut: non-finite LP-relaxation "
        "objective ({}) — caller falls back to the legacy cut path",
        z_lp);
    return std::nullopt;
  }

  // ── Step 2: the certified Theorem-O1 cut off the relaxed clone ────
  //
  //   α + Σ(−λ_i)·x_i ≥ b_LP,   b_LP = z*_LP − ⟨λ, v̂⟩
  //
  // λ = pinned reduced costs read from the clone (ovld2 semantics);
  // the |λ| < cut_coeff_eps filter drops a link from BOTH the row and
  // the Lagrangian charge below, i.e. dropped links carry λ_i = 0
  // exactly — the strengthened intercept is exact for the emitted
  // slopes (no Theorem-O3 ε on the intercept itself).
  StrengthenedCutResult result;
  result.row = build_benders_cut_physical(
      alpha_col, links, lp_clone, z_lp, cut_coeff_eps);
  result.lp_rhs = result.row.lowb;
  result.mip_rhs = result.row.lowb;

  // The kept multipliers (keep-filter mirrors
  // build_benders_cut_physical[ovld2] exactly).
  struct Charge
  {
    ColIndex dep {};
    double rc_phys {};
  };
  std::vector<Charge> charges;
  charges.reserve(links.size());
  {
    const auto rc_view = lp_clone.get_col_cost();
    for (const auto& link : links) {
      const auto rc_phys = rc_view[link.dependent_col];
      if (!std::isfinite(rc_phys) || std::abs(rc_phys) < cut_coeff_eps) {
        continue;
      }
      charges.push_back(Charge {
          .dep = link.dependent_col,
          .rc_phys = rc_phys,
      });
    }
  }

  // ── Step 3: the Lagrangian MIP (fresh clone, integrality intact) ──
  //
  //   m* = min { c'u − ⟨λ, z⟩ : (u, z) ∈ X_MIP, z ∈ box }
  //      = L(λ) − ⟨λ, v̂⟩
  //
  // Every pinned dependent column is relaxed to its physical source
  // box (the same box `relax_fixed_state_variable` uses — the cut's
  // validity domain), including eps-dropped links (their λ_i = 0, so
  // the pin must not survive either: a surviving pin would make the
  // bound invalid away from v̂).  The kept links get the −λ objective
  // charge in physical units.
  auto mip_clone = make_clone();
  for (const auto& link : links) {
    const auto dep = link.dependent_col;
    const auto lo = mip_clone.get_col_low_raw()[dep];
    const auto hi = mip_clone.get_col_upp_raw()[dep];
    if (std::abs(lo - hi) >= kStatePinDetectEps) {
      continue;  // not pinned (mirror relax_fixed_state_variable)
    }
    // Physical setters descale by the dep column's own col_scale, so
    // cross-phase scale drift is absorbed (see the elastic path).
    mip_clone.set_col_low(dep, link.source_low);
    mip_clone.set_col_upp(dep, link.source_upp);
  }
  const auto scale_obj = mip_clone.scale_objective();
  for (const auto& [dep, rc_phys] : charges) {
    // obj_phys(dep) −= λ: read the current raw coefficient, lift to
    // physical (`raw × scale_obj / col_scale` — the inverse of the
    // flatten composition), subtract, write back via the physical
    // setter (which re-applies `× col_scale / scale_obj`).
    const double cs = mip_clone.get_col_scale(dep);
    const double old_raw = mip_clone.get_obj_coeff()[dep];
    const double old_phys = (cs != 0.0) ? old_raw * scale_obj / cs : 0.0;
    mip_clone.set_obj_coeff(dep, old_phys - rc_phys);
  }

  auto r_mip = mip_clone.resolve(mip_opts);
  if (r_mip.has_value() && mip_clone.is_optimal()) {
    const double m_star = mip_clone.get_obj_value();
    // `max(b_LP, m*)`: Corollary SB2 guarantees m* ≥ b_LP for exact
    // solves; the guard keeps solver noise / a positive MIP gap on a
    // minimization stopped early from ever loosening the cut below
    // the certified LP baseline.
    if (std::isfinite(m_star) && m_star > result.row.lowb) {
      result.mip_rhs = m_star;
      result.row.lowb = m_star;
      result.tightened = true;
    }
  } else {
    // Aperture-timeout-style silent fallback: the LP-relaxation cut
    // (already in `result.row`) is valid on its own; the MIP was only
    // ever a tightening.  Function-form spdlog: the SPDLOG_DEBUG macro
    // is compiled out under the INFO-baked PCH, and this fallback must
    // stay observable at --log-level=debug.
    spdlog::debug(
        "build_strengthened_benders_cut: Lagrangian MIP solve failed / "
        "timed out (status {}) — emitting the LP-relaxation cut",
        mip_clone.get_status());
  }

  spdlog::debug(
      "build_strengthened_benders_cut: b_LP={:.6e} m*={:.6e} "
      "tightened={} ({} charged link(s))",
      result.lp_rhs,
      result.mip_rhs,
      result.tightened,
      charges.size());

  return result;
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

  if (std::abs(lo - hi) >= kStatePinDetectEps) {
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
  // `link.scost` carries the business `state_violation_cost` (default
  // 5000 in production cases) — NOT a Chinneck Phase-1 feasibility
  // price.  Using it here amplified the ray magnitude 5000× and
  // produced emax-pinning multi-cuts that violated LP tolerance by
  // `1e-8 × pi × rhs ≈ 0.03` on juan/gtopt_iplp p27.  The pure
  // `penalty` path matches PLP's AgrElastici call site
  // (plp-agrespd.f:673 passes `objs = 0` so every slack receives the
  // default 1.0).  `link.scost` retained on the struct for non-SDDP
  // consumers that may use it as a true business-cost hint.
  (void)link.scost;  // intentionally unused here
  // For the D3 slack-bound computation below we use the dep column's
  // *effective* LP-to-physical scale (reused in the ruiz case).  Under
  // `row_max` / `none` this equals `var_scale`; under `ruiz` it
  // additionally carries the ruiz-added factor.
  const double dep_scale_phys_raw = li.get_col_scale(dep);
  const double dep_scale_phys =
      (dep_scale_phys_raw > 0.0 && dep_scale_phys_raw != 1.0)
      ? dep_scale_phys_raw
      : link.var_scale;
  // Slack cost is ``penalty × dep_scale_phys``.  ``penalty`` is the
  // PLP Phase-1 unit cost (1.0 by default — see PLP's
  // ``osicallsc.cpp:658``); the ``× dep_scale_phys`` factor makes the
  // cost-per-PHYSICAL-unit-of-dep-relaxed invariant under col_scale.
  // Without it the elastic clone over-relaxes by a factor of
  // ``col_scale`` whenever ``col_scale > 1`` (1 LP-unit of slack
  // moves dep by 1 LP-raw, which is ``col_scale`` physical units —
  // so a unit slack cost prices each LP unit but each LP unit equals
  // ``col_scale`` physical units, making the per-physical price
  // ``1/col_scale`` of the unit price).  Pricing slacks at
  // ``penalty × col_scale`` restores per-physical invariance — the
  // elastic optimum activates the same physical-unit gap regardless
  // of LP scaling.
  const double slack_cost = penalty * dep_scale_phys;

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
  // Slack columns are LP-space variables with col_scale = 1 (PLP /
  // Chinneck Phase-1 convention).  The fixing row goes through
  // ``add_row_raw`` (below) which skips col_scale composition, so
  // there is no scaling mismatch between dep and the slacks even
  // when ``col_scale(dep) != 1``.  Crucially,
  // ``build_feasibility_cut_physical`` reads ``sol_raw[sup/sdn]``
  // (slack-col LP-raw) and lifts ``dx = sdn − sup`` to physical via
  // ``dx × dep_scale_phys`` — that lift is correct ONLY when slack
  // col_scale = 1 (giving slack-LP-raw values that equal dep-LP-raw
  // values, since the fixing row sums them with unit coefficients).
  // Setting ``slack.scale = dep_scale_phys`` here would double-lift
  // and produce ``col_scale × col_scale``-too-large cut RHSs.
  //
  // Slack uppb / cost are expressed in dep's LP-raw basis:
  // ``sup_uppb`` / ``sdn_uppb`` are computed from
  // ``link.trial_value`` (already in dep-LP-raw units) and
  // ``src_{low,upp}_lp = source_{low,upp} / dep_scale_phys``
  // (also in dep-LP-raw).  ``slack_cost`` follows PLP's flat unit
  // cost regardless of scaling.
  //
  // Per-link ``variable_uid`` uses the dep column index (unique
  // within the clone) so multiple slacks don't trip the duplicate-
  // metadata detector when the link's element uid is shared (or
  // unknown_uid in test fixtures).  ColIndex is implicitly
  // convertible to Index (== Uid's underlying int32_t), so no cast.
  const Uid slack_uid {dep};
  // Disposable adds: the elastic clone is throw-away, so the slack
  // cols / fixing row don't need to enter the shared metadata
  // (`m_labels_.col_labels_meta`).  Their label metadata
  // is captured into a per-clone-local extras vector + dedup map and
  // synthesised on demand by `generate_labels_from_maps` if a bad-LP
  // dump is ever requested — gtopt-formatted output identical to the
  // production path's.  See `LinearInterface::add_col_disposable`.
  //
  // The `variable_name` constants (`sddp_elastic_sup_col_name` /
  // `sddp_elastic_sdn_col_name`) live in `sddp_types.hpp`; centralising
  // them avoids the no-magic-strings rule and lets a future tooling
  // pass grep for the exact label used by `CoinLpIO`.
  const auto sup = li.add_col_disposable(SparseCol {
      .uppb = sup_uppb,
      .cost = slack_cost,
      .class_name = sddp_alpha_class_name,
      .variable_name = sddp_elastic_sup_col_name,
      .variable_uid = slack_uid,
  });

  const auto sdn = li.add_col_disposable(SparseCol {
      .uppb = sdn_uppb,
      .cost = slack_cost,
      .class_name = sddp_alpha_class_name,
      .variable_name = sddp_elastic_sdn_col_name,
      .variable_uid = slack_uid,
  });

  // dep + sup − sdn = trial_value
  //
  // The fixing row is inherently in **LP-raw space**: ``trial_value``
  // is the post-flatten LP-raw value of dep, and the unit coefficients
  // refer directly to dep / sup / sdn LP-raw values (not physical).
  // Inserting via ``add_row`` would route through the post-flatten
  // physical-space transformation (col_scale × element, ÷scale_obj on
  // RHS, then row-max equilibration), producing a row equivalent to
  // ``col_scale × dep + col_scale × sup − col_scale × sdn =
  // trial_value / scale_obj`` — silently inconsistent with the slack
  // uppb computations (in dep-LP-raw units) whenever
  // ``col_scale(dep) != 1`` or ``scale_objective != 1``.  Using
  // ``add_row_raw`` skips those transforms so the LP solver sees the
  // intended unit-coefficient fixing equation verbatim.
  //
  // Fixing row gets labelable metadata so ``write_lp`` can synthesise
  // a name on the elastic clone (post-mortem dumps).  ``variable_uid``
  // matches the slack's ``variable_uid`` (= dep col index) so the row
  // is colocated with its slack pair in label output.
  //
  // NOTE on dual scaling: this row is intentionally LP-raw with unit
  // coefficients on dep / sup / sdn and LP-raw RHS = trial_value.
  // ``get_row_dual()[fixing_row]`` returns ``raw_dual × scale_obj``
  // (since ``row_scale = 1``), which is ``∂obj_phys/∂trial_LP`` — the
  // physical objective marginal w.r.t. an LP-raw change in dep.  To
  // obtain the marginal w.r.t. a PHYSICAL change in dep (needed by
  // ``build_feasibility_cut_physical`` since its ``dep_clone_phys``
  // is in physical units), the consumer divides by
  // ``col_scale(dep)``.  Setting ``row.scale = col_scale(dep)`` here
  // would NOT help — the resulting LP form ``(1/col_scale) × dep + …
  // = trial_LP/col_scale`` produces a solver dual ``col_scale`` times
  // larger, and ``get_row_dual()``'s ``× scale_obj / row_scale``
  // composition cancels exactly back to the same value.  The
  // col_scale division must happen in the cut consumer.
  auto elastic = SparseRow {
      .lowb = link.trial_value,
      .uppb = link.trial_value,
      .class_name = sddp_alpha_class_name,
      .constraint_name = elastic_fix_constraint_name,
      .variable_uid = slack_uid,
  };
  elastic[dep] = 1.0;
  elastic[sup] = 1.0;
  elastic[sdn] = -1.0;

  const auto fixing_row = li.add_row_disposable(elastic);

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

namespace
{
// Per-link relaxation spec computed up-front so the cols / rows can be
// inserted in batches via `add_cols_disposable` / `add_rows_disposable`.
struct RelaxationSpec
{
  std::size_t link_idx {};  ///< Position in the original links span.
  ColIndex dep {};  ///< Dependent column (already re-bounded).
  Uid slack_uid {};  ///< Per-link unique id for slack metadata.
  /// Objective cost on the down-pull slack (sup).  Under the legacy
  /// `penalty_scaled` policy both directions carry
  /// `penalty × dep_scale_phys`; the PLP policy prices them
  /// per-direction (`1 − min(tilt, 0)` here).
  double sup_cost {};
  /// Objective cost on the up-lift slack (sdn) — PLP: `1 + max(tilt, 0)`.
  double sdn_cost {};
  double sup_uppb {};  ///< Upper bound on the down-pull slack.
  double sdn_uppb {};  ///< Upper bound on the up-lift slack.
  double trial_value {};  ///< LP-raw RHS for the fixing row.
};

// Pre-pass: walk every link, run the singular relaxability check
// (`abs(lo - hi) >= kStatePinDetectEps`) plus the bound-pinning + slack-bound
// math from `relax_fixed_state_variable`, and accumulate relaxation specs for
// the relaxable subset.  No col / row mutations happen here — only
// `set_col_low` / `set_col_upp` on the dependent column, which is a backend
// bound write that doesn't touch shared metadata.
//
// Returns the per-link specs in iteration order; non-relaxable links
// are silently skipped (the caller knows their link_idx because the
// spec carries it).
[[nodiscard]] std::vector<RelaxationSpec> compute_relaxation_specs(
    LinearInterface& li,
    std::span<const StateVarLink> links,
    double penalty,
    const ElasticCostPolicy& cost_policy)
{
  std::vector<RelaxationSpec> specs;
  specs.reserve(links.size());

  for (const auto& [i, link] : enumerate(links)) {
    const auto dep = link.dependent_col;
    const auto lo = li.get_col_low_raw()[dep];
    const auto hi = li.get_col_upp_raw()[dep];
    if (std::abs(lo - hi) >= kStatePinDetectEps) {
      continue;
    }

    // Mirror `relax_fixed_state_variable`: pin the dependent column
    // to the source-physical box first.  Physical setter is
    // intentional — it descales by `col_scale(dep)` so cross-phase
    // scale drift is absorbed.
    li.set_col_low(dep, link.source_low);
    li.set_col_upp(dep, link.source_upp);

    const double dep_scale_phys_raw = li.get_col_scale(dep);
    const double dep_scale_phys =
        (dep_scale_phys_raw > 0.0 && dep_scale_phys_raw != 1.0)
        ? dep_scale_phys_raw
        : link.var_scale;

    // Per-direction slack costs.  Legacy policy: symmetric
    // `penalty × dep_scale_phys`.  PLP policy: unit costs with the
    // `0.01 × rc_prev` tilt (see `ElasticCostPolicy` docstring for the
    // sign mapping — gtopt sdn ≡ PLP sp raises the state and carries
    // `1 + max(tilt, 0)`; gtopt sup ≡ PLP sn lowers it and carries
    // `1 − min(tilt, 0)`; both are ≥ 1 by construction).
    //
    // ALL policies price per PHYSICAL unit (`× dep_scale_phys`).  One
    // raw slack unit moves `dep` by one LP-raw unit = `col_scale(dep)`
    // physical units, so a flat per-RAW price under-charges scaled
    // state columns by exactly `col_scale`.  PLP's LP is UNSCALED —
    // its flat unit costs ARE per-physical-unit prices — so the
    // `× dep_scale_phys` fold is what preserves PLP parity under
    // gtopt's auto scale_reservoir.  Without it (pre-2026-07-11) the
    // per-element energy scales (×10 on LMAULE/ELTORO/RALCO in the
    // 2-yr case) made scaled reservoirs 10× cheaper to repair: the
    // elastic optimum routed the whole repair through them, the
    // fixing-row duals came out at ±1 per RAW unit (⇒ σ_phys = 0.1
    // vs 1.0 across elements — the col_scale leaking into the cut
    // coefficients), and the Phase-1 objective V summed raw units of
    // mixed physical meaning (hm3/10 + hm3).
    double sup_cost = penalty * dep_scale_phys;
    double sdn_cost = sup_cost;
    // Guard degenerate fixtures that explicitly zero var_scale — the
    // two unit-based policies must never emit a zero (free) slack.
    const double phys_per_raw = dep_scale_phys > 0.0 ? dep_scale_phys : 1.0;
    if (cost_policy.model == ElasticCostPolicy::Model::plp_unit_rc_tilt) {
      double tilt = 0.0;
      if (link.state_var != nullptr) {
        const double vs = link.state_var->var_scale();
        tilt = cost_policy.rc_tilt_factor
            * link.state_var->source_reduced_cost()
            * cost_policy.scale_objective / (vs != 0.0 ? vs : 1.0);
      }
      sdn_cost = (1.0 + std::max(tilt, 0.0)) * phys_per_raw;
      sup_cost = (1.0 - std::min(tilt, 0.0)) * phys_per_raw;
    } else if (cost_policy.model == ElasticCostPolicy::Model::unit) {
      // Füllner–Rebennack §17.2 Phase-1 objective eᵀy⁺ + eᵀy⁻: flat
      // unit costs per PHYSICAL unit, no rc tilt, no penalty folding.
      // The physical pricing makes V (the §17.3 intercept) measure
      // physical repair and lands σ_phys = σ_view / col_scale(dep)
      // back at ±1 for every active link regardless of per-element
      // scaling.
      sup_cost = phys_per_raw;
      sdn_cost = phys_per_raw;
    }

    const bool have_finite_box =
        (dep_scale_phys > 0.0) && (link.source_upp > link.source_low);
    const double vs = have_finite_box ? dep_scale_phys : 1.0;
    const double src_upp_lp =
        have_finite_box ? link.source_upp / vs : LinearProblem::DblMax;
    const double src_low_lp =
        have_finite_box ? link.source_low / vs : -LinearProblem::DblMax;
    const double sdn_uppb = have_finite_box
        ? std::max(0.0, src_upp_lp - link.trial_value)
        : LinearProblem::DblMax;
    const double sup_uppb = have_finite_box
        ? std::max(0.0, link.trial_value - src_low_lp)
        : LinearProblem::DblMax;

    specs.push_back(RelaxationSpec {
        .link_idx = static_cast<std::size_t>(i),
        .dep = dep,
        .slack_uid = Uid {dep},
        .sup_cost = sup_cost,
        .sdn_cost = sdn_cost,
        .sup_uppb = sup_uppb,
        .sdn_uppb = sdn_uppb,
        .trial_value = link.trial_value,
    });
  }

  return specs;
}

// Bulk equivalent of the per-link `relax_fixed_state_variable` loop:
// pre-compute specs, then issue exactly one `add_cols_disposable`
// (slacks) and one `add_rows_disposable` (fixing rows) call,
// regardless of the number of relaxable links.  On juan-scale
// (~300 cuts × 8 links) this collapses 4800 backend round-trips per
// elastic solve into 2.
//
// Returns one `RelaxedVarInfo` per input link, in order; non-
// relaxable links get a default-constructed (`relaxed = false`)
// entry so callers can index by link position.
[[nodiscard]] std::vector<RelaxedVarInfo> apply_relaxations_bulk(
    LinearInterface& li,
    std::span<const StateVarLink> links,
    double penalty,
    const ElasticCostPolicy& cost_policy = {})
{
  std::vector<RelaxedVarInfo> infos(links.size());

  const auto specs = compute_relaxation_specs(li, links, penalty, cost_policy);
  if (specs.empty()) {
    return infos;
  }

  // Build slack column specs in stride-2 layout: [sup_0, sdn_0, sup_1,
  // sdn_1, ...].  Bulk insert returns indices in the same order, so
  // `cols_idx[2k]` is `sup` and `cols_idx[2k + 1]` is `sdn` for
  // spec k.
  std::vector<SparseCol> slack_cols;
  slack_cols.reserve(specs.size() * 2);
  for (const auto& s : specs) {
    slack_cols.push_back(SparseCol {
        .uppb = s.sup_uppb,
        .cost = s.sup_cost,
        .class_name = sddp_alpha_class_name,
        .variable_name = "elastic_sup",
        .variable_uid = s.slack_uid,
    });
    slack_cols.push_back(SparseCol {
        .uppb = s.sdn_uppb,
        .cost = s.sdn_cost,
        .class_name = sddp_alpha_class_name,
        .variable_name = "elastic_sdn",
        .variable_uid = s.slack_uid,
    });
  }
  const auto slack_indices = li.add_cols_disposable(slack_cols);

  // Build fixing rows now that slack indices are known.
  std::vector<SparseRow> fixing_rows;
  fixing_rows.reserve(specs.size());
  for (const auto& [k, s] : enumerate(specs)) {
    const auto sup = slack_indices[static_cast<std::size_t>(k) * 2];
    const auto sdn = slack_indices[(static_cast<std::size_t>(k) * 2) + 1];
    auto elastic = SparseRow {
        .lowb = s.trial_value,
        .uppb = s.trial_value,
        .class_name = sddp_alpha_class_name,
        .constraint_name = elastic_fix_constraint_name,
        .variable_uid = s.slack_uid,
    };
    elastic[s.dep] = 1.0;
    elastic[sup] = 1.0;
    elastic[sdn] = -1.0;
    fixing_rows.push_back(std::move(elastic));
  }
  const auto row_indices = li.add_rows_disposable(fixing_rows);

  // Fan out into per-link RelaxedVarInfo by the recorded link_idx.
  for (const auto& [k, s] : enumerate(specs)) {
    const auto kk = static_cast<std::size_t>(k);
    infos[s.link_idx] = RelaxedVarInfo {
        .relaxed = true,
        .sup_col = slack_indices[kk * 2],
        .sdn_col = slack_indices[(kk * 2) + 1],
        .fixing_row = row_indices[kk],
    };
  }
  return infos;
}

// Elasticize the installed feasibility-cut rows of the clone
// (`elastic_filter_mode = farkas_recursive`): for each row r with
// sense `a_rᵀx ≥ b_r`, add one slack column z_r ≥ 0 (unbounded above,
// unit cost — same physical-cost convention as the sup/sdn pair: the
// cost passes through `add_cols_disposable` which divides by
// `scale_objective`, and `get_row_dual()` multiplies it back) and
// insert the raw coefficient +1 into the existing row via
// `set_coeff_raw` — CLP's `modifyCoefficient` and CPLEX's `CPXchgcoef`
// both INSERT a new nonzero when the element is absent.  This is the
// Füllner–Rebennack §17.2 `+ I z` term: the relaxed clone can always
// satisfy a poisoned cut row by paying z_r, so "relaxed clone
// infeasible" can no longer be caused by an installed cut.
//
// Rows outside the clone's current row range are skipped with a WARN —
// a desynced StoredCut::row would otherwise corrupt an unrelated row.
[[nodiscard]] std::vector<FcutSlackInfo> apply_fcut_relaxations(
    LinearInterface& li, std::span<const RowIndex> rows)
{
  std::vector<FcutSlackInfo> infos;
  if (rows.empty()) {
    return infos;
  }

  const auto numrows = li.numrows_as_index();
  std::vector<RowIndex> valid_rows;
  valid_rows.reserve(rows.size());
  std::vector<SparseCol> z_cols;
  z_cols.reserve(rows.size());
  for (const auto row : rows) {
    if (row == RowIndex {unknown_index} || row >= numrows) {
      SPDLOG_WARN(
          "apply_fcut_relaxations: fcut row {} outside clone row range "
          "[0, {}) — skipped (stale cut-store row index?)",
          static_cast<int>(row),
          static_cast<int>(numrows));
      continue;
    }
    valid_rows.push_back(row);
    // z is priced per PHYSICAL unit of cut violation: the installed
    // cut row was composed by `add_row` (col_scale × row-max ×
    // scale_objective), so one raw z unit relaxes the PHYSICAL cut
    // LHS by `row_scale` (the stored composite).  Pricing z at
    // `row_scale` keeps the ω·z* part of the clone objective — the
    // §17.3 fold the farkas intercept reads through V — in the same
    // physical units as the state-slack costs (which are priced at
    // `col_scale(dep)` per raw unit).  Unit-scale rows (row_scale = 1,
    // every fixture) are unchanged.
    z_cols.push_back(SparseCol {
        .cost = li.get_row_scale(row),
        .class_name = sddp_alpha_class_name,
        .variable_name = sddp_elastic_zfc_col_name,
        .variable_uid = Uid {row},
    });
  }
  if (valid_rows.empty()) {
    return infos;
  }

  const auto z_indices = li.add_cols_disposable(z_cols);
  infos.reserve(valid_rows.size());
  for (const auto& [k, row] : enumerate(valid_rows)) {
    const auto z_col = z_indices[static_cast<std::size_t>(k)];
    li.set_coeff_raw(row, z_col, 1.0);
    infos.push_back(FcutSlackInfo {
        .row = row,
        .z_col = z_col,
    });
  }
  return infos;
}
}  // namespace

auto elastic_filter_solve(const LinearInterface& li,
                          std::span<const StateVarLink> links,
                          double penalty,
                          const SolverOptions& opts,
                          const ElasticCostPolicy& cost_policy,
                          std::span<const RowIndex> elastic_fcut_rows)
    -> std::optional<ElasticSolveResult>
{
  // Shallow clone: the elastic path adds slacks and a fixing row via
  // the disposable APIs (`add_col_disposable`, `add_row_disposable`),
  // which write only per-clone-local extras and never touch the
  // shared metadata.  Bound mutations and obj-coeff zeroing happen
  // through the backend and don't touch shared state either.  COW
  // detach therefore stays dormant.
  //
  // Clone route — pick the parallel-safe manual+replay path when
  // both pre-conditions are met:
  //
  //   1. `has_snapshot_data()`: an uncompressed FlatLinearProblem
  //      snapshot is available for `load_flat` to rebuild from.
  //
  //   2. `low_memory_mode() != off`: the replay buffer
  //      (m_dynamic_cols_, m_dynamic_rows_, m_active_cuts_,
  //      m_pending_col_bounds_) has been populated by add_col /
  //      add_row / set_col_low_raw / set_col_upp_raw.  Those
  //      auto-record gates are CLOSED in off mode (see
  //      `linear_interface.cpp` line 1456 and the bound setters at
  //      2630/2653 — `m_low_memory_mode_ != LowMemoryMode::off` is
  //      the precondition for recording).  Off mode still saves a
  //      snapshot at build time (`system_lp.cpp:560`) for
  //      fingerprinting / future use, so `has_snapshot_data()` is
  //      true even there — but `m_replay_` is empty, and the
  //      manual+replay clone would silently miss α + cuts +
  //      forward-pass-pinned bounds (verified 2026-05-08 against the
  //      ElasticFilterMode comparison fixture: missing α col,
  //      missing α=0 bootstrap pin, missing reservoir trial-pin).
  //
  // When either gate fails we fall back to the native `clone()`,
  // which is itself serialised by a process-global mutex inside the
  // LinearInterface — correctness-safe even though it serialises N
  // concurrent elastic clones.
  // `has_decompressed_snapshot()` is the right gate (not just
  // `has_snapshot_data()`): under `LowMemoryMode::compress` the
  // snapshot is held compressed between iterations.  After
  // `reconstruct_backend` runs (lazy on first solve), the flat_lp
  // vectors are cleared and only the compressed buffer remains —
  // `clone_from_flat` would throw "source flat LP is not decompressed".
  // Falling back to native `clone()` here is correct: it reads from the
  // live backend, which has the full LP regardless of snapshot
  // compression state.
  const bool can_use_manual = li.has_decompressed_snapshot()
      && li.low_memory_mode() != LowMemoryMode::off;
  auto cloned = can_use_manual
      ? li.clone_from_flat(LinearInterface::CloneKind::shallow)
      : li.clone(LinearInterface::CloneKind::shallow);

  // Chinneck Phase-1 feasibility LP: zero every original objective
  // coefficient so the relaxed LP becomes a pure feasibility problem.
  // Its optimum equals Σ(s⁺ + s⁻) = minimum total slack activation to
  // restore feasibility — independent of dispatch cost structure.
  // Matches PLP `plp-bc.f`, Chinneck (2008) § 4, Ruszczyński (1997).
  // Keeping the original obj would leak state-dependent opex into
  // the Benders feasibility-cut RHS and drive α to diverge under
  // SDDP iteration (observed on juan/gtopt_iplp).
  //
  // Bulk dispatch via `set_obj_coeffs_raw`: a single backend call
  // replaces the per-column loop.  CPLEX / HiGHS / OSI / MindOpt /
  // Gurobi all override the bulk virtual with their native batched
  // attribute-array setter, so the elastic clone's Phase-1 reset
  // becomes one C-API round-trip per infeasible cell instead of N.
  const auto ncols = static_cast<size_t>(cloned.get_numcols());
  const std::vector<double> zeros(ncols, 0.0);
  cloned.set_obj_coeffs_raw(zeros);

  // The Phase-1 reset must ALSO clear the LP's objective CONSTANT —
  // the solver-native offset accumulated by `add_obj_constant` (the
  // demand-failure Option-A baseline `+fail_cost·ecost·lmax`, the
  // boundary-cut α-rebase c̄, DecisionVariable obj_constant, …).
  // `set_obj_coeffs_raw(zeros)` only touches column coefficients; the
  // offset would otherwise leak verbatim into the clone's
  // `get_obj_value()`, which every feasibility-gap consumer reads
  // (the farkas_recursive intercept V, the α-bearing fcut RHS).
  // Observed on the 2-yr case (farkas_recursive, i1): each phase's
  // ~1.7e8 demand-fail baseline inflated the frcut RHS, and the §17.2
  // z-fold then re-folded the poisoned RHS plus the NEXT phase's
  // constant at every backtrack — an additive runaway to a 6.2e9 RHS
  // at p1 (unsatisfiable-by-construction cuts that turned one
  // recoverable p52 event into a full p52→p0 cascade).
  if (const double obj_c = cloned.get_obj_constant(); obj_c != 0.0) {
    cloned.add_obj_constant(-obj_c);
  }

  // α is intentionally NOT pinned / modified in the clone: leave it in
  // whatever bound state the original LP has (bootstrap `lowb=uppb=0`
  // on first iter, or `[-DblMax, +DblMax]` after `bound_alpha` has fired
  // from a prior optimality cut).  Pinning α at 0 here would make the
  // clone infeasible whenever the target phase has an installed
  // optimality cut `α + Σ rc·s ≥ Z` with positive Z at the current
  // state, hiding the true feasibility gap on the forward state
  // variables.  α is never added to `outgoing_links` (see
  // `collect_state_variable_links`), so no slack variables are ever
  // created for α — the filter treats α purely as a passive column
  // that keeps whatever value satisfies the installed cuts.

  ElasticSolveResult result;

  // One bulk pass: slack cols + fixing rows are inserted in two
  // backend calls total, instead of `2N + N` per-link calls.
  result.link_infos =
      apply_relaxations_bulk(cloned, links, penalty, cost_policy);

  const bool modified = std::ranges::any_of(
      result.link_infos, [](const auto& info) { return info.relaxed; });
  if (!modified) {
    return std::nullopt;
  }

  // §17.2 `+ I z` term (farkas_recursive only; default empty span is
  // a no-op): relax every installed feasibility-cut row with a z
  // slack so a poisoned cut can never wedge the clone.
  result.fcut_infos = apply_fcut_relaxations(cloned, elastic_fcut_rows);

  // Solve the clone with elastic slack variables
  auto r = cloned.resolve(opts);
  result.solved = r.has_value() && cloned.is_optimal();
  if (result.solved) {
    spdlog::trace("elastic_filter_solve: solved clone (obj={:.4f})",
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
  for (const auto& [i, info] : std::views::enumerate(infos)) {
    if (!info.relaxed) {
      continue;
    }
    const double sup_val =
        info.sup_col != ColIndex {unknown_index} ? sol[info.sup_col] : 0.0;
    const double sdn_val =
        info.sdn_col != ColIndex {unknown_index} ? sol[info.sdn_col] : 0.0;
    if (sup_val <= slack_tol && sdn_val <= slack_tol) {
      non_essential.push_back(static_cast<std::size_t>(i));
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
  auto cut = build_benders_cut_physical(
      alpha_col, links, elastic->clone, elastic->clone.get_obj_value());

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

  // Per-cut counter used to make each multi-cut sibling's `extra`
  // unique within this (scene, phase, iter) call.  All siblings share
  // the same 3-tuple (scene, phase, iter) from ``context``; without a
  // per-cut discriminator the StoredCut dedup ``CutKey`` would
  // collapse them.  The caller's original ``extra = infeas_count``
  // (carried in the ``IterationContext`` 4th slot when ``context``
  // holds one) is folded into the high bits so this still
  // distinguishes cuts emitted on different attempts of the same iter.
  // ``context`` may also be a non-iteration variant (e.g. monostate
  // for unit tests that build cuts directly without setting it) — in
  // that case fall back to ``niter`` as the base and a fresh
  // ``IterationContext`` cannot be assembled; we leave ``context``
  // unchanged for those callers and only stamp ``extra`` when an
  // IterationContext is present.
  const bool has_iter_ctx = std::holds_alternative<IterationContext>(context);
  const int base_extra =
      has_iter_ctx ? std::get<3>(std::get<IterationContext>(context)) : niter;
  constexpr int kExtraStride = 4096;  // > max plausible links per cell
  int link_idx = 0;
  for (const auto& [info, link] : std::views::zip(link_infos, links)) {
    if (!info.relaxed) {
      continue;
    }
    if (info.fixing_row == RowIndex {unknown_index}) {
      continue;
    }
    const double pi = -duals[info.fixing_row];

    // A degenerate basis at the elastic feasibility boundary can make
    // the Phase-4 re-solve report optimal while returning NaN duals
    // (observed 2026-07-02, ~1-in-3 on the ElasticFilterMode fixture).
    // The magnitude filter below does NOT exclude NaN — IEEE 754 makes
    // `abs(NaN) < eps` false — so guard explicitly or the NaN lands in
    // the fcut row and wedges the solver.  Skipping the link only
    // weakens this fcut, exactly like the eps filter.
    if (!std::isfinite(pi)) {
      spdlog::warn(
          "build_multi_cuts: non-finite dual pi={} at fixing row {} — "
          "link {} skipped",
          pi,
          static_cast<int>(info.fixing_row),
          link_idx);
      continue;
    }

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
    const double sup_val =
        info.sup_col != ColIndex {unknown_index} ? sol_raw[info.sup_col] : 0.0;
    const double sdn_val =
        info.sdn_col != ColIndex {unknown_index} ? sol_raw[info.sdn_col] : 0.0;
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

    // Per-cut saturation drop.  When the clamp had to bite — i.e.
    // ``implied_bound`` exited the source column's physical
    // ``[source_low, source_upp]`` box — the resulting cut is
    // mathematically equivalent to ``state >= source_upp`` (or
    // ``state <= source_low``).  Cascading that bound through the
    // master LP at the previous phase usually pins storage at emax,
    // which the upstream forward pass cannot reach with a single
    // stage of inflows + the predecessor trial — guaranteed
    // cascade-infeasibility on the next master retry.
    //
    // Drop the individual saturated cut here (not just the whole
    // family at the end of the loop).  The family-level guard
    // ``box_stats.all_upper_degenerate()`` below remains as a
    // diagnostic + cleanup for the rare case where *every* link
    // saturates; this per-cut filter handles the *partial* degeneracy
    // that currently leaks through (e.g. juan/IPLP single-bus run
    // with multi_cut elastic mode: 5 of 8 links saturate at emax,
    // 3 at inside-box, family kept, 5 emax-pins force infeasibility).
    //
    // Tolerance is relative to the box width to absorb LP solver
    // ULP drift on the elastic side without false-dropping legitimate
    // cuts that happen to sit close to (but inside) the box edge.
    //
    // **Unbounded side handling**: ``source_upp`` and ``source_low``
    // can carry sentinel values for "no bound" — typically
    // ``+DblMax`` / ``-DblMax`` (= 1e308 in this build's solver
    // infinity) or smaller-but-still-huge numbers like ``1e30`` that
    // bypass the LP scaler.  A naive ``box_width = source_upp -
    // source_low`` then evaluates to ~2e308, and ``1e-9 × box_width``
    // dwarfs every realistic ``implied_bound``, so the saturation
    // check would fire on *every* cut and silently disable the entire
    // multi-cut path.  Two guards:
    //
    //   1. ``kInfThreshold = 0.5 × DblMax`` — anything past this on
    //      either side is effectively unbounded; skip the saturation
    //      check on that side (a cut with ``implied_bound`` headed to
    //      ±inf is genuinely degenerate, but the clamp-then-rebuild
    //      path already produces nonsense for it; the family-level
    //      diagnostic still catches the all-degenerate case).
    //   2. ``box_width`` is capped at 1e15 before scaling so the
    //      tolerance stays sensible even when one side is bounded
    //      and the other is at ±DblMax (one-sided box).
    constexpr double kInfThreshold = 0.5 * LinearProblem::DblMax;
    const bool upper_bounded = link.source_upp < kInfThreshold;
    const bool lower_bounded = link.source_low > -kInfThreshold;
    const double raw_width = link.source_upp - link.source_low;
    const double bounded_width = std::min(raw_width, 1e15);
    const double box_width = std::max(1.0, bounded_width);
    // Tolerance kept very tight — first-backtrack cuts (niter=1, the
    // common case) carry FactEps perturbation
    // ``eps_factor = 1e-10`` so ``implied_bound`` exceeds
    // ``source_upp`` by ``eps_factor × source_upp ≈ 1e-7`` for
    // typical LP-space values (~1e3).  A 1e-9 relative tolerance was
    // too lax and missed those near-saturation cases — observed on
    // juan/IPLP single-bus where the multi_cut path emitted 7/8 cuts
    // pinning ELTORO/RAPEL/CIPRESES/PEHUENCHE/PANGUE/RALCO/COLBUN at
    // their LP-space ``source_upp`` exactly post-clamp, but pre-clamp
    // ``implied_bound`` only edged past the bound by 1e-7 — below
    // ``box_eps = 1.766e-6`` for a 1766-wide ELTORO box.  1e-12 keeps
    // the filter firmly above LP solver ULP drift (~1e-15) without
    // letting saturated cuts through.
    const double box_eps = 1e-12 * box_width;
    // Only drop when the clamp *actually bit* — i.e.
    // ``implied_bound`` was STRICTLY outside the box.  An
    // ``implied_bound`` that lands exactly on (or within ``box_eps``
    // of) ``source_upp`` is a legitimate signal that the elastic
    // optimum walked the source up to its physical max without
    // pushing past it; that cut still contributes useful information
    // (regression test
    // ``test_sddp_benders_cut.cpp::"cut RHS is not clamped to
    // source_upp"`` enforces this).  The cascade-infeasibility we
    // want to suppress is the case where ``rhs/pi`` was, say,
    // ``2 × source_upp`` pre-clamp and got pulled in by ``std::clamp``
    // — the resulting cut is mathematically equivalent to fixing
    // ``state = source_upp`` even though the elastic clone only said
    // "the LP would have liked state to grow further".  Strict ``>``
    // (not ``>=``) preserves the regression test while still cutting
    // off saturated cuts on juan/IPLP single-bus.
    if ((upper_bounded && implied_bound > link.source_upp + box_eps)
        || (lower_bounded && implied_bound < link.source_low - box_eps))
    {
      box_stats.tally(clamped_bound, link);
      continue;
    }

    // Per-cut unique `extra`: combine the caller's base_extra
    // (typically infeas_count) with a per-link counter so each sibling
    // multi-cut row carries a distinct ``extra`` in its context.
    // Without this, every mcut sibling shares the same CutKey and the
    // StoredCut dedup view collapses them — observed as
    // ``test_sddp_fcut_audit`` failures after the name-removal switch
    // to CutKey-based identity.
    auto sibling_context = context;
    if (has_iter_ctx) {
      auto& it_ctx = std::get<IterationContext>(sibling_context);
      std::get<3>(it_ctx) = (base_extra * kExtraStride) + link_idx;
    }

    auto cut = SparseRow {
        .lowb = clamped_rhs,
        .uppb = LinearProblem::DblMax,
        .class_name = link.class_name,
        .constraint_name = sddp_mcut_constraint_name,
        .variable_uid = link.uid,
        .context = sibling_context,
    };
    cut[link.source_col] = pi;
    cuts.push_back(std::move(cut));
    ++link_idx;

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

// ─── PLP-exact feasibility cuts (`elastic_filter_mode = state_repair`) ──────

auto build_plp_feasibility_cuts(ElasticSolveResult& elastic,
                                std::span<const StateVarLink> links,
                                const LpContext& context,
                                double fact_eps) -> PlpFeasibilityCuts
{
  // PLP `plp-agrespd.f::AgrElastici` (FOneFeasRay = FALSE branch, the
  // production default) reproduced step-for-step.  See the header
  // docstring for the formula; the numbered comments below cite the
  // matching PLP source lines.
  PlpFeasibilityCuts out;

  // (0) plp-fasedual.f:698-780 / plp-faseprim.f:656-714: a clone not
  // proven optimal (infeasible / unbounded / limit) means the
  // infeasibility is rooted in rows the elastic filter cannot relax —
  // no cut exists, the caller declares infeasibility.
  if (!elastic.solved) {
    out.status = PlpCutStatus::fail;
    return out;
  }

  // PLP's absolute floor 2^-36 added to every FactEPS-derived
  // tolerance (plp-agrespd.f: `eps = FactEPS + 2.0**(-36)`).
  constexpr double kTwoPowM36 = 0x1p-36;
  const double ray_eps = fact_eps + kTwoPowM36;

  auto duals = elastic.clone.get_row_dual();
  const auto sol_raw = elastic.clone.get_col_sol_raw();
  const auto& link_infos = elastic.link_infos;

  // (1) Extraction pass — one (ray, rhsi) pair per relaxed link.
  struct PlpExtract
  {
    std::size_t link_idx {};
    double ray {};
    double rhsi {};
  };
  std::vector<PlpExtract> extracts;
  extracts.reserve(links.size());
  double rhs_total = 0.0;

  const std::size_t n_links = std::min(link_infos.size(), links.size());
  for (std::size_t idx = 0; idx < n_links; ++idx) {
    const auto& info = link_infos[idx];
    const auto& link = links[idx];
    if (!info.relaxed || info.fixing_row == RowIndex {unknown_index}) {
      continue;
    }

    // ray_i = −dual(fixing_row_i).  PLP never sees NaN (its LP is
    // always solved to a simplex vertex); gtopt guards anyway — a NaN
    // dual would poison the emitted row (same rationale as the
    // build_multi_cuts guard).  Treated as ray = 0 (link dropped).
    double ray = -duals[info.fixing_row];
    if (!std::isfinite(ray)) {
      spdlog::warn(
          "build_plp_feasibility_cuts: non-finite dual {} at fixing "
          "row {} — ray zeroed",
          ray,
          static_cast<int>(info.fixing_row));
      ray = 0.0;
    }
    // Ray-zero threshold (osicallsc.cpp:723): |ray| < FactEPS + 2^-36.
    if (std::abs(ray) < ray_eps) {
      ray = 0.0;
    }

    // dx = sdn − sup: the minimal RHS repair in dep-LP-raw units
    // (gtopt fixing row `dep + sup − sdn = trial` ⇒ dep_clone =
    // trial + dx; equals PLP's `sp − sn` under its row
    // `dep − sp + sn = vini` — see the sign-mapping note in the
    // ElasticCostPolicy docstring).
    const double sup_val =
        info.sup_col != ColIndex {unknown_index} ? sol_raw[info.sup_col] : 0.0;
    const double sdn_val =
        info.sdn_col != ColIndex {unknown_index} ? sol_raw[info.sdn_col] : 0.0;
    double dx = sdn_val - sup_val;

    // dx activation filter (osicallsc.cpp:727-730): a sub-tolerance
    // slack activation zeroes BOTH dx and ray — a fixing-row dual can
    // be nonzero at a degenerate vertex even when the elastic did not
    // move this state at all (the demand-row dual leaks through the
    // basic dep column), so the dx filter is what keeps untouched
    // links out of the cut set.
    if (std::abs(dx) < fact_eps * (std::abs(link.trial_value) + 1e-8)) {
      dx = 0.0;
      ray = 0.0;
    }
    if (ray == 0.0) {
      continue;
    }

    // nx_i = the minimally repaired initial state, physical space —
    // the `dep_clone_phys` convention shared with build_multi_cuts.
    // dep_scale lifts the LP-raw dx through any ruiz factor on top of
    // var_scale.  Fixture fallback (null state_var): the trial value
    // itself lifted to physical — exact for the fixtures' unit-scale
    // LPs (production always carries state_var).
    const double dep_scale = elastic.clone.get_col_scale(link.dependent_col);
    const double v_hat_phys = (link.state_var != nullptr)
        ? link.state_var->col_sol_physical()
        : link.trial_value * dep_scale;
    const double nx = v_hat_phys + (dx * dep_scale);
    const double rhsi = ray * nx;
    rhs_total += rhsi;

    extracts.push_back(PlpExtract {
        .link_idx = idx,
        .ray = ray,
        .rhsi = rhsi,
    });
  }

  // (2) Emission pass (plp-agrespd.f:770-815, FOneFeasRay = FALSE):
  // one single-variable row per link whose ray survives the
  // rhs_total-relative skip.  NO clamping to the state box, NO
  // saturation / degenerate-family drops, NO aggregated fallback —
  // the RHS is bound-consistent BY CONSTRUCTION (slack caps = the
  // previous phase's state box, so nx ∈ [source_low, source_upp]).
  const double deps = (1e-3 * fact_eps * std::abs(rhs_total)) + kTwoPowM36;

  // Per-sibling `extra` stamping — same CutKey-dedup scheme as
  // build_multi_cuts (all siblings share (scene, phase, iter); the
  // 4th IterationContext slot disambiguates them).
  const bool has_iter_ctx = std::holds_alternative<IterationContext>(context);
  const int base_extra =
      has_iter_ctx ? std::get<3>(std::get<IterationContext>(context)) : 0;
  constexpr int kExtraStride = 4096;  // > max plausible links per cell
  int sibling_idx = 0;

  for (const auto& ex : extracts) {
    if (std::abs(ex.ray) <= deps) {
      continue;
    }
    const auto& link = links[ex.link_idx];

    // Outward FactEPS margin (plp-agrespd.f:791): rhs += FactEPS·|rhs|
    // — unconditional (not signed by ray), exactly as PLP applies it.
    // Then clamp to the bound-level satisfiability limit: PLP's cut is
    // satisfiable at bound level BY CONSTRUCTION (slack caps = the
    // previous phase's state box ⇒ nx ∈ [source_low, source_upp]), but
    // when the repair lands exactly ON the box edge the margin would
    // push the RHS an epsilon PAST it (ray>0: above ray·source_upp;
    // ray<0: above ray·source_low) — presolve then declares the row
    // bound-infeasible (observed on the 2y case: repaired state at the
    // box top). min() restores the invariant without weakening any
    // interior cut.
    const double bound_limit =
        ex.ray > 0.0 ? ex.ray * link.source_upp : ex.ray * link.source_low;
    const double rhs =
        std::min(ex.rhsi + (fact_eps * std::abs(ex.rhsi)), bound_limit);

    auto sibling_context = context;
    if (has_iter_ctx) {
      auto& it_ctx = std::get<IterationContext>(sibling_context);
      std::get<3>(it_ctx) = (base_extra * kExtraStride) + sibling_idx;
    }

    auto cut = SparseRow {
        .lowb = rhs,
        .uppb = LinearProblem::DblMax,
        .class_name = link.class_name,
        .constraint_name = sddp_plpcut_constraint_name,
        .variable_uid = link.uid,
        .context = sibling_context,
    };
    cut[link.source_col] = ex.ray;
    out.cuts.push_back(std::move(cut));
    ++sibling_idx;
  }

  // (3) Status: clone optimal + zero rows ⇒ HOLGURAS (PLP IStat = −1
  // — the elastic repaired feasibility without moving any state).
  out.status =
      out.cuts.empty() ? PlpCutStatus::holguras : PlpCutStatus::cuts_added;

  SPDLOG_DEBUG(
      "build_plp_feasibility_cuts: {} link(s) extracted, {} cut(s) "
      "emitted (rhs_total={:.6e}, deps={:.3e}, fact_eps={:.1e}) — {}",
      extracts.size(),
      out.cuts.size(),
      rhs_total,
      deps,
      fact_eps,
      out.status == PlpCutStatus::cuts_added ? "CUTS_ADDED" : "HOLGURAS");

  return out;
}

// ─── Recursive feasibility cut (`elastic_filter_mode = farkas_recursive`) ───

auto build_farkas_recursive_cut(ElasticSolveResult& elastic,
                                std::span<const StateVarLink> links,
                                const LpContext& context,
                                double fact_eps) -> PlpFeasibilityCuts
{
  // Füllner & Rebennack (SIAM Review 2023) §17.2–17.3 — see the
  // header docstring for the full mapping (h_t ↔ trial RHS of the
  // fixing rows, α^f_r ↔ b_r of the elasticized cut rows) and the
  // strong-duality argument that the eq. (17.3) intercept
  // `σᵀh_t + ωᵀα^f` (+ gtopt's column-box folds) equals the clone
  // optimum V(v̂) rebased at the trial:
  //
  //     Σ_i σ_i·x_i ≥ Σ_i σ_i·v̂_i + V(v̂)
  //
  // Solver-independent by construction: ordinary optimal duals +
  // objective value only, never a Farkas/ray API.
  PlpFeasibilityCuts out;

  // (0) Clone not proven optimal ⇒ the infeasibility is rooted in
  // rows the filter cannot relax (a HARD non-cut row) — no cut
  // exists; the caller declares the scene infeasible.  With the
  // §17.2 `+ I z` term in place this can no longer be caused by an
  // installed feasibility-cut row.
  if (!elastic.solved) {
    out.status = PlpCutStatus::fail;
    return out;
  }

  // Same absolute floor as the PLP-exact builder: 2⁻³⁶ added to the
  // FactEPS-derived zero-guard.
  constexpr double kTwoPowM36 = 0x1p-36;
  const double dual_eps = fact_eps + kTwoPowM36;

  auto duals = elastic.clone.get_row_dual();
  const auto sol_raw = elastic.clone.get_col_sol_raw();
  const auto& link_infos = elastic.link_infos;

  // V(v̂): the Phase-1 optimum (original objective zeroed; physical
  // sup/sdn/z costs) — the minimum total PHYSICAL violation needed to
  // restore feasibility at the trial.  `get_obj_value()` returns the
  // same scale_objective-folded convention as the `get_row_dual()`
  // view, so σ·v̂ and V compose consistently.
  //
  // `− get_obj_constant()`: defense-in-depth against the objective
  // CONSTANT leaking into the intercept.  `elastic_filter_solve`
  // already zeroes the clone's offset alongside the column
  // coefficients (so this subtraction is a no-op on every production
  // path), but the intercept formula is only correct for the
  // pure-slack objective — a clone handed in with a live offset
  // (demand-fail Option-A baseline ~1.7e8/phase on the 2-yr case)
  // previously inflated every frcut RHS by that constant and drove
  // the additive p52→p0 RHS runaway (6.2e9 at p1).
  const double v_gap =
      elastic.clone.get_obj_value() - elastic.clone.get_obj_constant();

  auto cut = SparseRow {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .class_name = sddp_alpha_class_name,
      .constraint_name = sddp_frcut_constraint_name,
      .context = context,
  };

  double rhs = v_gap;
  int n_coeffs = 0;
  // Maximum achievable LHS of the emitted cut over the previous
  // phase's PHYSICAL state box — accumulated per emitted link for the
  // post-margin satisfiability clamp below.  A link unbounded in the
  // cut's tightening direction disables the clamp (the box max is
  // +inf, so any finite RHS is satisfiable).
  double box_top_lhs = 0.0;
  bool box_top_bounded = true;
  constexpr double kInfThreshold = 0.5 * LinearProblem::DblMax;
  const std::size_t n_links = std::min(link_infos.size(), links.size());
  for (std::size_t idx = 0; idx < n_links; ++idx) {
    const auto& info = link_infos[idx];
    const auto& link = links[idx];
    if (!info.relaxed || info.fixing_row == RowIndex {unknown_index}) {
      continue;
    }

    // σ_i = −dual(fixing_row_i) — same sign convention as
    // `build_feasibility_cut_physical` (σ > 0 ⇔ the clone wants the
    // state to INCREASE).  NaN guard mirrors the other builders: a
    // degenerate re-solve can report optimal with NaN duals, and
    // `abs(NaN) < eps` is false.
    const double sigma_view = -duals[info.fixing_row];
    if (!std::isfinite(sigma_view)) {
      spdlog::warn(
          "build_farkas_recursive_cut: non-finite dual {} at fixing "
          "row {} — link skipped",
          sigma_view,
          static_cast<int>(info.fixing_row));
      continue;
    }
    if (std::abs(sigma_view) < dual_eps) {
      continue;
    }

    // σ_view is ∂obj/∂trial_LP (the fixing row is LP-raw).  Emit the
    // per-PHYSICAL-unit coefficient σ_view / col_scale(dep) so that
    // `σ_phys·x_phys` chains exactly (σ_view·Δtrial_LP ≡
    // σ_phys·Δtrial_phys); the paired RHS term uses
    // σ_phys·v̂_phys = σ_view·trial_LP.  Under unit scale (every
    // fixture) this equals the incumbent view-dual convention.
    const double dep_scale = elastic.clone.get_col_scale(link.dependent_col);
    const double sigma_phys =
        (dep_scale != 0.0) ? sigma_view / dep_scale : sigma_view;
    const double v_hat_phys = (link.state_var != nullptr)
        ? link.state_var->col_sol_physical()
        : link.trial_value * dep_scale;

    cut[link.source_col] = sigma_phys;
    rhs += sigma_phys * v_hat_phys;
    ++n_coeffs;

    // Track the box-top LHS of the growing cut (physical units): for
    // σ > 0 the maximising state sits at source_upp, for σ < 0 at
    // source_low.
    const double edge = sigma_phys > 0.0 ? link.source_upp : link.source_low;
    if (std::abs(edge) >= kInfThreshold) {
      box_top_bounded = false;
    } else {
      box_top_lhs += sigma_phys * edge;
    }
  }

  // ω_r diagnostics on the elasticized cut rows.  The ω_r·b_r fold is
  // already CONTAINED in V (strong duality — the poisoned rows'
  // residual violation Σ ω_r·z*_r is the z-part of the objective), so
  // ω is NOT added to the RHS again; it is surfaced here so operators
  // can see how much of the gap came from relaxing installed cuts.
  double omega_fold = 0.0;
  int n_omega = 0;
  for (const auto& fi : elastic.fcut_infos) {
    if (fi.row == RowIndex {unknown_index}
        || fi.z_col == ColIndex {unknown_index})
    {
      continue;
    }
    const double omega = duals[fi.row];
    if (!std::isfinite(omega)) {
      spdlog::warn(
          "build_farkas_recursive_cut: non-finite dual {} at "
          "elasticized fcut row {} — ignored (fold is carried by V)",
          omega,
          static_cast<int>(fi.row));
      continue;
    }
    const double z_val = sol_raw[fi.z_col];
    if (std::abs(omega) >= dual_eps && z_val > 0.0) {
      omega_fold += omega * z_val;
      ++n_omega;
    }
  }

  // Clone optimal but no state sensitivity survives the zero-guard ⇒
  // no valid state cut exists (PLP "holguras" semantics — the elastic
  // repaired feasibility without implicating any state variable).
  if (n_coeffs == 0) {
    out.status = PlpCutStatus::holguras;
    return out;
  }

  // Outward FactEPS-relative margin — same convention as the
  // state_repair builder (plp-agrespd.f:791) — followed by the
  // aggregated mirror of state_repair's `bound_limit` min(): when the
  // UNmargined intercept was satisfiable at the box top, the margin
  // must not push it past the edge (presolve would then declare the
  // master row bound-infeasible and force a guaranteed deeper
  // backtrack).  The clamp deliberately does NOT touch an intercept
  // that already exceeds the box-top LHS BEFORE the margin — that is
  // the exact §17.3 unreachability proof (e.g. a folded downstream
  // cut whose requirement no box state can meet; see the kill-chain
  // pin in test_farkas_recursive_cuts.cpp) and weakening it would
  // discard the recursion's information.  Diagnosed as a WARN so
  // operators can see genuinely box-unreachable events.
  const double rhs_margin = rhs + (fact_eps * std::abs(rhs));
  if (box_top_bounded && rhs <= box_top_lhs) {
    rhs = std::min(rhs_margin, box_top_lhs);
  } else {
    if (box_top_bounded) {
      spdlog::warn(
          "build_farkas_recursive_cut: intercept {:.6e} exceeds the "
          "box-top LHS {:.6e} ({} coeff(s), V={:.6e}) — downstream "
          "unreachable from every previous-phase state; the master "
          "re-solve will backtrack another level",
          rhs,
          box_top_lhs,
          n_coeffs,
          v_gap);
    }
    rhs = rhs_margin;
  }
  cut.lowb = rhs;

  out.cuts.push_back(std::move(cut));
  out.status = PlpCutStatus::cuts_added;

  // Function-form spdlog: the SPDLOG_DEBUG macro is compiled out under
  // the INFO-baked PCH (see the file-top caveat), and the ω-fold
  // diagnostic must stay observable at --log-level=debug.
  spdlog::debug(
      "build_farkas_recursive_cut: 1 aggregated cut ({} sigma coeff(s), "
      "rhs={:.6e}, V={:.6e}, omega-fold={:.6e} over {} active z row(s), "
      "fact_eps={:.1e})",
      n_coeffs,
      rhs,
      v_gap,
      omega_fold,
      n_omega,
      fact_eps);

  return out;
}

// ─── Cut averaging ──────────────────────────────────────────────────────────
//
// `average_benders_cut` and `accumulate_benders_cuts` were removed
// 2026-07-08 together with the invalid `broadcast_mean` / `accumulate`
// cut-sharing modes (their only production callers).  The aperture
// expected-cut path keeps `weighted_average_benders_cut` below.

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

// ─── BendersCut class ────────────────────────────────────────────────────────

auto BendersCut::elastic_filter_solve(
    const LinearInterface& li,
    std::span<const StateVarLink> links,
    double penalty,
    const SolverOptions& opts,
    const ElasticCostPolicy& cost_policy,
    std::span<const RowIndex> elastic_fcut_rows)
    -> std::optional<ElasticSolveResult>
{
  if (m_pool_ == nullptr) {
    // No pool available: delegate directly to the free function.
    auto result = gtopt::elastic_filter_solve(
        li, links, penalty, opts, cost_policy, elastic_fcut_rows);
    if (result.has_value()) {
      m_infeasible_cut_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  // Clone route — mirror the free `elastic_filter_solve` gating so
  // both elastic paths (single/multi via this method, chinneck via
  // the free function) pick up the parallel-safe manual+replay route
  // under the same conditions.  See the free function for the full
  // rationale; in short: when a snapshot is hydrated AND the replay
  // buffer is being populated (low_memory != off), `clone_from_flat`
  // builds the clone via `CPXcreateprob` + `CPXaddrows` on a freshly-
  // opened solver env (no `CPXcloneprob`, no global mutex, parallel-
  // safe across N forward-pass workers), then replays α + cuts +
  // pinned bounds onto the clone.  Falls back to native `clone()` —
  // itself process-globally serialised since `fe2ed42d` — when
  // either pre-condition fails.
  // `has_decompressed_snapshot()` is the right gate (not just
  // `has_snapshot_data()`): under `LowMemoryMode::compress` the
  // snapshot is held compressed between iterations.  After
  // `reconstruct_backend` runs (lazy on first solve), the flat_lp
  // vectors are cleared and only the compressed buffer remains —
  // `clone_from_flat` would throw "source flat LP is not decompressed".
  // Falling back to native `clone()` here is correct: it reads from the
  // live backend, which has the full LP regardless of snapshot
  // compression state.
  const bool can_use_manual = li.has_decompressed_snapshot()
      && li.low_memory_mode() != LowMemoryMode::off;
  auto cloned = can_use_manual
      ? li.clone_from_flat(LinearInterface::CloneKind::shallow)
      : li.clone(LinearInterface::CloneKind::shallow);

  // Chinneck Phase-1 feasibility LP — zero every original objective
  // coefficient, exactly like the free `elastic_filter_solve` (see the
  // long rationale there).  This pooled path historically SKIPPED the
  // zeroing, silently leaking dispatch opex into the clone objective
  // and the fixing-row duals whenever a work pool was set (production
  // SDDP always sets one) — divergent from both the free function and
  // every consumer's documented contract ("the clone carries a
  // Chinneck Phase-1 objective", sddp_forward_pass.cpp).  The
  // farkas_recursive intercept additionally reads the clone OPTIMUM as
  // the feasibility gap V, which requires the pure-slack objective.
  const auto ncols = static_cast<size_t>(cloned.get_numcols());
  const std::vector<double> zeros(ncols, 0.0);
  cloned.set_obj_coeffs_raw(zeros);

  // Clear the objective CONSTANT too — mirrors the free
  // `elastic_filter_solve` (see the long rationale there): the
  // solver-native offset (demand-fail Option-A baseline, boundary-cut
  // c̄, …) is part of the original objective and would otherwise leak
  // into `get_obj_value()`, inflating the farkas_recursive intercept V
  // by ~1.7e8/phase on the 2-yr case (the p52→p0 RHS runaway).
  if (const double obj_c = cloned.get_obj_constant(); obj_c != 0.0) {
    cloned.add_obj_constant(-obj_c);
  }

  ElasticSolveResult result;

  // One bulk pass: see the free `elastic_filter_solve` for the
  // backend-call-count rationale.
  result.link_infos =
      apply_relaxations_bulk(cloned, links, penalty, cost_policy);

  const bool modified = std::ranges::any_of(
      result.link_infos, [](const auto& info) { return info.relaxed; });
  if (!modified) {
    return std::nullopt;
  }

  // §17.2 `+ I z` term — mirrors the free function (no-op on the
  // default empty span).
  result.fcut_infos = apply_fcut_relaxations(cloned, elastic_fcut_rows);

  // Resolve the clone IN-THREAD — this method is itself called from a
  // forward-pass pool task (`sddp_forward_pass.cpp` → `elastic_solve` →
  // here), so submitting the clone-resolve back into the same pool would
  // create a recursive-submit deadlock: the parent forward-pass worker
  // would block on `fut.get()` for a child the pool's memory-throttle
  // gate cannot dispatch, so all parent workers wedge on their nested
  // futures, no `tasks_active_` decrement ever happens, and the pool
  // never recovers (juan/iplp_plain hang fingerprint, gdb thread dump
  // 2026-05-08: 13/96 workers parked in `__atomic_futex_wait_until` two
  // frames below `BendersCut::elastic_filter_solve`).
  //
  // Running the resolve in-thread costs nothing in parallelism — the
  // forward-pass worker would have been blocked on `fut.get()` for the
  // duration of the solve anyway.  It removes the deadlock class
  // entirely: the parent task simply executes the LP solve as part of
  // its own body and returns normally, decrementing `tasks_active_` as
  // expected, freeing the slot for other work, releasing memory.  Pool-
  // level CPU/active-worker monitoring no longer sees the elastic solve
  // as a discrete task — that's an acceptable trade-off (the elastic
  // path is rare, ~1 in 100+ forward solves on convergent runs).
  auto r = cloned.resolve(opts);
  const bool solved = r.has_value() && cloned.is_optimal();

  if (solved) {
    m_infeasible_cut_count_.fetch_add(1, std::memory_order_relaxed);
    SPDLOG_TRACE(
        "BendersCut::elastic_filter_solve: solved clone in-thread "
        "(obj={:.4f}), total_infeasible_cuts={}",
        cloned.get_obj_value(),
        m_infeasible_cut_count_.load(std::memory_order_relaxed));
    result.clone = std::move(cloned);
    return result;
  }

  // Preserve the unsolved clone so the caller can persist it for
  // post-mortem diagnostics — mirrors the free-function path which
  // always returns ``result`` (with ``solved=false``) on failure.
  // The earlier ``return std::nullopt`` here masked the elastic LP
  // from the saved-error-LP machinery in
  // ``sddp_forward_pass.cpp:641``, which made debugging
  // ``relaxed clone infeasible`` warnings impossible.
  result.solved = false;
  result.clone = std::move(cloned);
  return result;
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
  auto cut = build_benders_cut_physical(
      alpha_col, links, elastic->clone, elastic->clone.get_obj_value());

  return FeasibilityCutResult {
      .cut = std::move(cut),
      .elastic = std::move(*elastic),
  };
}

}  // namespace gtopt
