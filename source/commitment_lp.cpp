/**
 * @file      commitment_lp.cpp
 * @brief     LP formulation for unit commitment (three-bin u/v/w model)
 * @date      Tue Apr  8 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements CommitmentLP: creates binary status/startup/shutdown variables,
 * adds the three core UC constraints (logic, gen limits, exclusion),
 * emission cost adder on generation variables, and emission cap constraint.
 */

#include <gtopt/commitment_lp.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

CommitmentLP::CommitmentLP(const Commitment& commitment, const InputContext& ic)
    : Base(commitment, ic, Element::class_name)
    , generator_index_(ic.element_index(generator_sid()))
    , startup_cost_(
          ic, Element::class_name, id(), std::move(object().startup_cost))
    , shutdown_cost_(
          ic, Element::class_name, id(), std::move(object().shutdown_cost))
    , fixed_status_(
          ic, Element::class_name, id(), std::move(object().fixed_status))
    , pmin_(ic, Element::class_name, id(), std::move(object().pmin))
    , ramp_up_(ic, Element::class_name, id(), std::move(object().ramp_up))
    , ramp_down_(ic, Element::class_name, id(), std::move(object().ramp_down))
    , startup_ramp_(
          ic, Element::class_name, id(), std::move(object().startup_ramp))
    , shutdown_ramp_(
          ic, Element::class_name, id(), std::move(object().shutdown_ramp))
{
}

bool CommitmentLP::add_to_lp(SystemContext& sc,
                             const ScenarioLP& scenario,
                             const StageLP& stage,
                             LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // UC constraints only apply to chronological stages
  if (!stage.is_chronological()) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index_);
  if (!generator_lp.is_active(stage)) {
    return true;
  }

  // Tolerant lookup: same rationale as the patch at
  // ``source/reserve_provision_lp.cpp:264`` — when every block of
  // ``(scenario, stage)`` was elided by the P1 zero-pmax optimization
  // (all-zero pmax_profile, e.g. PLEXOS-must-OFF units forced via
  // ``--use-plexos-gen-cap``), the outer ``generation_cols`` key is
  // absent and ``generation_cols_at`` would throw.
  // ``lookup_generation_cols`` returns an empty inner holder; the
  // per-block ``generation_cols.find(buid)`` checks below safely
  // iterate zero times and no commitment row is emitted for blocks
  // without a dispatchable gen column.
  const auto& generation_cols =
      generator_lp.lookup_generation_cols(scenario, stage);
  const auto& blocks = stage.blocks();

  if (blocks.empty()) {
    return true;
  }

  // ── LP-size: skip the whole commitment when the generator elided
  // every generation column for this (scenario, stage) ───────────────
  // ``lookup_generation_cols`` is empty precisely when the generator
  // emitted no dispatch column at all — every block was P1-skipped
  // (all-zero pmax) or ``--use-plexos-gen-cap`` forced the unit OFF.
  // In that case the status/startup/shutdown binaries (u/v/w) and all
  // the C1/C3/min-up/min-down/max-starts rows would couple to nothing
  // physical: Phase B already `continue`s on every block (no gcol to
  // bind), leaving the binaries orphaned.  Returning here drops those
  // orphan columns and rows entirely, consistent with the elided
  // generation column.
  //
  // Write-out rule: a generator with no dispatch column is OFF for the
  // whole (scenario, stage), so its commitment is identically
  // ``status = startup = shutdown = 0``.  The output holders stay
  // empty, which the writer renders as "no rows" (long) / "no uid
  // column" (wide) — the natural zero, matching the absent generation.
  if (generation_cols.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto cuid = uid();

  // Resolve commitment parameters
  const auto noload = commitment().noload_cost.value_or(0.0);
  // Startup / shutdown costs are now per-(stage, block) schedules
  // (``OptTBRealSched``), resolved at the period's representative block
  // inside the loop below (see ``v_cost`` / ``w_cost``).  A scalar or
  // per-stage JSON input broadcasts to every block, so this reproduces
  // the legacy per-stage cost byte-for-byte while letting a single-stage
  // PLEXOS conversion carry a per-period Gen_StartCost profile.
  const auto initial_u = commitment().initial_status.value_or(0.0);
  // ``initial_power`` (≡ ``p_prev`` at the first block of the
  // planning horizon) defaults to 0.  When ``initial_u = 1`` and
  // pmin > 0, this default forces the LP's first-block ramp-up to
  // be infeasible whenever the ramp limit < pmin (the case that
  // exposed the bug: RTS-GMLC ``216_STEAM_1`` with pmin = 62,
  // ramp_up = 60 → ``p[0] ≤ 0 + 60·1 = 60`` clashed with the pmin
  // floor of 62).  Hot-start callers (e.g. UC.jl's
  // ``Initial power (MW)`` field) must set ``Commitment.initial_power``
  // explicitly for the first-block constraint to honor the
  // previous-block dispatch.
  const auto initial_p = commitment().initial_power.value_or(0.0);
  const auto is_relax = commitment().relax.value_or(false)
      || sc.simulation().phases()[stage.phase_index()].is_continuous();
  const auto is_must_run = commitment().must_run.value_or(false);
  // Ramp limits / startup-shutdown envelopes are now per-(stage, block)
  // schedules (``OptTBRealSched``), resolved block-by-block below exactly
  // like ``pmin_``.  ``has_value()`` here reports whether the field was
  // supplied at all (scalar or per-block); a scalar input resolves to the
  // same constant on every block, preserving the legacy ``OptReal``
  // behaviour byte-for-byte.  The per-block ``value_or(gen_pmax)``
  // "unset ⇒ pmax (no limit)" fallback is applied inside the loop.
  const auto has_ramp_up = ramp_up_.has_value();
  const auto has_ramp_down = ramp_down_.has_value();
  const auto has_startup_ramp = startup_ramp_.has_value();
  const auto has_shutdown_ramp = shutdown_ramp_.has_value();

  // Resolve piecewise heat rate curve.
  //
  // DEPRECATED on Commitment.  Piecewise heat-rate-derived fuel cost
  // Piecewise heat-rate + fuel-cost handling is OWNED by ``GeneratorLP``
  // (source/generator_lp.cpp:272+).  ``Generator.pmax_segments`` +
  // ``Generator.heat_rate_segments`` + ``Generator.fuel`` are wired
  // there as a pure-LP convex-slack formulation that works with or
  // without a Commitment binary.  CommitmentLP intentionally
  // *DOES NOT* read those fields anymore — they were removed from
  // the Commitment struct on 2026-05-20 (commitment.hpp).

  // Emission cost is no longer wired here — the per-pollutant tax
  // and cap live on `EmissionZone` (see `emission_zone_lp.cpp`),
  // which adds its `price · production` term to the objective via
  // a dedicated bridge column.  Generator dispatch + segment cost
  // accounting here is fuel-only.

  // Resolve startup cost tiers
  const auto opt_hot_cost = commitment().hot_start_cost;
  const auto opt_warm_cost = commitment().warm_start_cost;
  const auto opt_cold_cost = commitment().cold_start_cost;
  const auto opt_hot_time = commitment().hot_start_time;
  const auto opt_cold_time = commitment().cold_start_time;
  const bool has_startup_tiers = opt_hot_cost.has_value()
      && opt_warm_cost.has_value() && opt_cold_cost.has_value()
      && opt_hot_time.has_value() && opt_cold_time.has_value();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  // ── Compute commitment periods ──
  // When commitment_period is set, binary variables (u/v/w) are created at
  // a coarser resolution.  Each period groups consecutive blocks whose
  // cumulative duration fits within commitment_period hours.
  // Default: one period per block (commitment_period not set).
  const auto opt_period = commitment().commitment_period;
  const auto period_hours = opt_period.value_or(0.0);  // 0 = per-block

  // Period grouping: each entry is the index of the first block in a period.
  // period_starts[k] = index into blocks[] where period k begins.
  // A final sentinel equal to blocks.size() marks the end.
  std::vector<size_t> period_starts;
  period_starts.push_back(0);
  if (period_hours > 0.0) {
    double accum = 0.0;
    for (size_t i = 0; i < blocks.size(); ++i) {
      accum += blocks[i].duration();
      if (accum >= period_hours - 1e-9 && i + 1 < blocks.size()) {
        period_starts.push_back(i + 1);
        accum = 0.0;
      }
    }
  } else {
    // One period per block
    for (size_t i = 1; i < blocks.size(); ++i) {
      period_starts.push_back(i);
    }
  }
  const auto nperiods = period_starts.size();

  // Map every block index → its period index, AND materialise per-period
  // block-uid vectors used by the IntegerVariable registry fan-out
  // (consumed by both the u/v/w loop below and the startup-tier loop).
  std::vector<size_t> block_period(blocks.size());
  std::vector<std::vector<BlockUid>> period_block_uids(nperiods);
  for (size_t p = 0; p < nperiods; ++p) {
    const auto start = period_starts[p];
    const auto end = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
    period_block_uids[p].reserve(end - start);
    for (size_t i = start; i < end; ++i) {
      block_period[i] = p;
      period_block_uids[p].push_back(blocks[i].uid());
    }
  }
  const auto period_scope =
      (period_hours > 0.0) ? IntegerScope::Group : IntegerScope::Block;
  const auto period_domain =
      is_relax ? IntegerDomain::Relaxed : IntegerDomain::Binary;

  // ── Phase A: Create u/v/w per commitment period, C1 and C3 ──
  BIndexHolder<ColIndex> ucols;  // period representative buid → u col
  BIndexHolder<ColIndex> vcols;
  BIndexHolder<ColIndex> wcols;
  BIndexHolder<RowIndex> lrows;
  BIndexHolder<RowIndex> erows;
  map_reserve(ucols, nperiods);
  map_reserve(vcols, nperiods);
  map_reserve(wcols, nperiods);
  map_reserve(lrows, nperiods);
  map_reserve(erows, nperiods);

  // Period-indexed v/w columns: avoid repeated map lookups per block in the
  // phase-B loop (each period is visited once per block in that period).
  std::vector<ColIndex> period_vcol(nperiods);
  std::vector<ColIndex> period_wcol(nperiods);

  // block_ucol / block_vcol / block_wcol map EVERY block uid → its
  // period's u / v / w column. The per-block maps feed both the
  // ramp / logic / gen-bound constraint rows and the AMPL variable
  // registry (see end of this function), so user constraints can
  // reference ``commitment("X").status`` / ``.startup`` / ``.shutdown``.
  BIndexHolder<ColIndex> block_ucol;
  BIndexHolder<ColIndex> block_vcol;
  BIndexHolder<ColIndex> block_wcol;
  map_reserve(block_ucol, blocks.size());
  map_reserve(block_vcol, blocks.size());
  map_reserve(block_wcol, blocks.size());

  ColIndex prev_ucol {};
  bool first_period = true;

  for (size_t p = 0; p < nperiods; ++p) {
    const auto pstart = period_starts[p];
    const auto pend = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();

    // Period representative block (first block in the period)
    const auto& rep_block = blocks[pstart];
    const auto rep_buid = rep_block.uid();
    const auto ctx = make_block_context(scenario.uid(), stage.uid(), rep_buid);

    // Accumulate period duration for noload cost
    double period_duration = 0.0;
    for (size_t i = pstart; i < pend; ++i) {
      period_duration += blocks[i].duration();
    }

    // ── Create u (status) variable ──
    // Noload cost is proportional to the period duration, not block duration.
    // Use the representative block for probability/discount factors.
    const auto u_cost =
        CostHelper::block_ecost(scenario, stage, rep_block, noload)
        * (period_duration / rep_block.duration());

    // Resolve commitment bounds.  Precedence (highest → lowest):
    //   1. ``fixed_status`` schedule entry for this (stage, block) — if
    //      set to a value in ``[0, 1]``, pins ``u`` to that value
    //      (rounded at 0.5 to 0 or 1).  Values *outside* ``[0, 1]``
    //      are reserved as the "no-pin" sentinel so the schedule can
    //      pin some blocks and leave others free without juggling a
    //      proper nullable cell type (UC.jl's ``Commitment status``
    //      arrays can contain ``null`` for un-pinned blocks; the
    //      converter emits ``-1.0`` there).
    //   2. ``must_run`` flag — forces ``u >= 1`` for the whole stage.
    //   3. Default — ``u`` in ``[0, 1]``.
    auto u_lowb = is_must_run ? 1.0 : 0.0;
    auto u_uppb = 1.0;
    if (const auto pin = fixed_status_.optval(stage.uid(), rep_buid)) {
      const auto val = pin.value();
      if (val >= 0.0 && val <= 1.0) {
        const auto pinned = (val >= 0.5) ? 1.0 : 0.0;
        u_lowb = pinned;
        u_uppb = pinned;
      }
    }

    auto ucol =
        sc.add_integer_col(lp,
                           IntegerVariable::key(scenario,
                                                stage,
                                                Element::class_name,
                                                cuid,
                                                StatusName,
                                                period_scope,
                                                rep_buid),
                           SparseCol {
                               .lowb = u_lowb,
                               .uppb = u_uppb,
                               .cost = u_cost,
                               // pin scale on all three commitment vars (status
                               // u + startup v + shutdown w), not just the
                               // integer one — u is semantically binary even
                               // when LP-relaxed (--no-mip drops is_integer).
                               .pin_scale = true,
                               .class_name = cname,
                               .variable_name = StatusName,
                               .variable_uid = cuid,
                               .context = ctx,
                           },
                           period_domain,
                           period_scope,
                           rep_buid,
                           std::span<const BlockUid> {period_block_uids[p]});
    ucols[rep_buid] = ucol;

    // ── Create v (startup) and w (shutdown) variables ──
    //
    // These are deliberately CONTINUOUS in [0,1], NOT integer, even on a
    // non-relaxed (MIP) build.  In this tight 3-binary formulation the
    // logic equality C1 (`u[p] - u[p-1] - v[p] + w[p] = 0`) together with
    // the exclusion C3 (`v[p] + w[p] <= 1`) and the nonnegative startup /
    // shutdown costs force v and w to the integer up/down transition of an
    // integer u — so they take binary values at every optimal vertex
    // without being declared integer.  Declaring only u integer cuts the
    // branching-variable count by ~2/3 with an identical feasible set.
    // See Knueven, Ostrowski & Watson (2020), "On MIP Formulations for the
    // Unit Commitment Problem", INFORMS J. Comput. 32(4); and
    // Morales-España, Latorre & Ramos (2013), "Tight and Compact MILP
    // Formulation ...", IEEE Trans. Power Syst.
    // Startup is a once-per-EVENT cost ($/start), NOT a per-hour power cost,
    // so it must NOT be multiplied by the block duration (Δt).  Use the
    // no-duration ``cost_factor`` (probability · discount) — present-valued
    // but face-value in magnitude.  Multiplying by ``rep_block.duration()``
    // (as ``block_ecost`` does) would inflate it by the block length.
    // Resolve startup / shutdown cost at this period's representative
    // block (a scalar/per-stage input broadcasts to the same value on
    // every block — full back-compat with the legacy per-stage cost).
    const auto block_startup_cost =
        startup_cost_.optval(stage.uid(), rep_buid).value_or(0.0);
    const auto block_shutdown_cost =
        shutdown_cost_.optval(stage.uid(), rep_buid).value_or(0.0);
    const auto v_cost = has_startup_tiers ? 0.0
                                          : block_startup_cost
            * CostHelper::cost_factor(scenario.probability_factor(),
                                      stage.discount_factor());
    // Startup ``v`` is continuous-in-[0,1] always (not integer-declared)
    // but the C1 logic + nonnegative cost force it to {0, 1} at the
    // optimum.  Pin ``scale = 1.0`` so the C1 coefficient invariants
    // survive Ruiz / VariableScaleMap — task #50.
    auto vcol = lp.add_col({
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = v_cost,
        .is_integer = false,
        .pin_scale = true,  // semantically binary even when LP-relaxed
        .class_name = cname,
        .variable_name = StartupName,
        .variable_uid = cuid,
        .context = ctx,
    });
    vcols[rep_buid] = vcol;
    period_vcol[p] = vcol;

    // Shutdown, like startup, is a once-per-EVENT cost ($/stop), NOT a
    // per-hour power cost — use the no-duration ``cost_factor`` so it is
    // not inflated by the block length.
    const auto w_cost = block_shutdown_cost
        * CostHelper::cost_factor(scenario.probability_factor(),
                                  stage.discount_factor());
    // Shutdown ``w`` — same shape as startup; pin scale = 1.0.
    auto wcol = lp.add_col({
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = w_cost,
        .is_integer = false,
        .pin_scale = true,  // semantically binary even when LP-relaxed
        .class_name = cname,
        .variable_name = ShutdownName,
        .variable_uid = cuid,
        .context = ctx,
    });
    wcols[rep_buid] = wcol;
    period_wcol[p] = wcol;

    // Fan the u / v / w columns out across every block in this period
    // so per-block constraint rows and AMPL accessors can resolve them.
    for (size_t i = pstart; i < pend; ++i) {
      const auto buid = blocks[i].uid();
      block_ucol[buid] = ucol;
      block_vcol[buid] = vcol;
      block_wcol[buid] = wcol;
    }

    // ── C1: Logic transition (per period) ──
    // u[p] - u[p-1] - v[p] + w[p] = 0
    // For first period: u[0] - v[0] + w[0] = initial_status
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = LogicName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .equal(first_period ? initial_u : 0.0);
      row[ucol] = 1.0;
      if (!first_period) {
        row[prev_ucol] = -1.0;
      }
      row[vcol] = -1.0;
      row[wcol] = 1.0;
      lrows[rep_buid] = lp.add_row(std::move(row));
    }

    // ── C3: Exclusion (per period): v[p] + w[p] <= 1 ──
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = ExclusionName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(1.0);
      row[vcol] = 1.0;
      row[wcol] = 1.0;
      erows[rep_buid] = lp.add_row(std::move(row));
    }

    prev_ucol = ucol;
    first_period = false;
  }

  // ── Phase B: Per-block constraints (C2, ramp, emission, segments) ──
  // Each block references its period's u column via block_ucol.
  BIndexHolder<RowIndex> gurows;
  BIndexHolder<RowIndex> glrows;
  BIndexHolder<RowIndex> rurows;
  BIndexHolder<RowIndex> rdrows;
  map_reserve(gurows, blocks.size());
  map_reserve(glrows, blocks.size());
  map_reserve(rurows, blocks.size());
  map_reserve(rdrows, blocks.size());

  ColIndex prev_gcol {};
  ColIndex prev_block_ucol {};
  bool first_block = true;

  for (size_t bidx = 0; bidx < blocks.size(); ++bidx) {
    const auto& block = blocks[bidx];
    const auto buid = block.uid();
    const auto ctx = make_block_context(scenario.uid(), stage.uid(), buid);

    const auto gcol_it = generation_cols.find(buid);
    if (gcol_it == generation_cols.end()) {
      continue;
    }
    const auto gcol = gcol_it->second;
    const auto ucol = block_ucol.at(buid);

    // Look up period's v/w for ramp constraints via period-indexed cache
    // (avoids two map .at() lookups per block).
    const auto pidx = block_period[bidx];
    const auto vcol = period_vcol[pidx];
    const auto wcol = period_wcol[pidx];

    const auto gen_pmax = lp.get_col_uppb(gcol);
    // ── Resolve commitment-conditional pmin ──
    // ``Commitment.pmin`` is the per-unit min stable level when
    // committed (PLEXOS ``Generator.Min Stable Level`` semantics).
    // It is INDEPENDENT from ``Generator.pmin``, the always-on hard
    // floor that lives on the gen column's ``lowb``.  Both bounds
    // remain in force; the LP enforces:
    //   gen ≥ Generator.pmin                  (always, col bound)
    //   gen ≥ Commitment.pmin × u             (conditional, row below)
    // CommitmentLP must NOT modify ``Generator.pmin`` (the col
    // ``lowb``) — that's the user's territory; only adds its own
    // u-linked row.
    //
    // When ``Commitment.pmin`` is unset (legacy JSON written before
    // the field existed), fall back on reading the col ``lowb`` so
    // round-trip compatibility is preserved.  Legal as long as
    // ``Generator.pmin ≤ Commitment.pmin``; values that violate
    // that order are a user error (the LP still solves but the
    // conditional pmin is irrelevant — dominated by the always-on
    // floor).
    const auto explicit_pmin = pmin_.optval(stage.uid(), buid);
    const auto gen_pmin =
        explicit_pmin.has_value() ? *explicit_pmin : lp.get_col_lowb(gcol);

    // ── C2: Generation upper bound: p - Pmax*u <= 0 ──
    //
    // ``Generator.pmin`` (the gen col ``lowb``) is left untouched —
    // it is an independent always-on floor, distinct from the
    // commitment-conditional ``Commitment.pmin`` applied via the
    // C2 row below.  Users wanting a purely-conditional pmin set
    // ``Generator.pmin = 0`` and ``Commitment.pmin = desired floor``.

    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = GenUpperName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(0.0);
      row[gcol] = 1.0;
      row[ucol] = -gen_pmax;
      gurows[buid] = lp.add_row(std::move(row));
    }

    // ── C2: Generation lower bound: p - Pmin*u >= 0 ──
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = GenLowerName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .greater_equal(0.0);
      row[gcol] = 1.0;
      row[ucol] = -gen_pmin;
      glrows[buid] = lp.add_row(std::move(row));
    }

    // ── C4: Ramp up: p[t] - p[t-1] ≤ RU·u[t-1] + SU·v[t] ──
    if (has_ramp_up || has_startup_ramp) {
      // Per-block resolution mirrors ``pmin_`` above: ``optval`` returns
      // the scheduled rate at this (stage, block), or nullopt for unset
      // blocks → ``value_or(gen_pmax)`` keeps the legacy "no limit ⇒
      // pmax" semantics.  A scalar JSON input resolves to the same value
      // on every block.
      const auto ru = ramp_up_.optval(stage.uid(), buid).value_or(gen_pmax)
          * block.duration();
      const auto su =
          startup_ramp_.optval(stage.uid(), buid).value_or(gen_pmax);

      // First block uses ``initial_p`` as ``p_prev`` and ``initial_u``
      // as ``u_prev``; subsequent blocks reach back to ``prev_gcol``
      // / ``prev_block_ucol`` for the same quantities.  Without
      // ``initial_p`` on the RHS, hot-start gens with ``ramp_up <
      // pmin`` (e.g. RTS-GMLC ``216_STEAM_1``) go infeasible on the
      // first block.
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = RampUpName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(first_block ? initial_p + (ru * initial_u)
                                  + (su * (1.0 - initial_u))
                                      : 0.0);
      row[gcol] = 1.0;
      if (!first_block) {
        row[prev_gcol] = -1.0;
        row[prev_block_ucol] = -ru;
        row[vcol] = -su;
      }
      rurows[buid] = lp.add_row(std::move(row));
    }

    // ── C5: Ramp down: p[t-1] - p[t] ≤ RD·u[t] + SD·w[t] ──
    if (has_ramp_down || has_shutdown_ramp) {
      // Per-block resolution, mirroring the ramp-up branch / ``pmin_``.
      const auto rd = ramp_down_.optval(stage.uid(), buid).value_or(gen_pmax)
          * block.duration();
      const auto sd =
          shutdown_ramp_.optval(stage.uid(), buid).value_or(gen_pmax);

      // Mirror the ramp-up first-block convention: ``p_prev`` on the
      // LHS becomes ``initial_p`` on the RHS for the first block.
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = RampDownName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(first_block ? (rd * initial_u)
                                  + (sd * (1.0 - initial_u)) - initial_p
                                      : 0.0);
      row[gcol] = -1.0;
      if (!first_block) {
        row[prev_gcol] = 1.0;
        row[ucol] = -rd;
        row[wcol] = -sd;
      }
      rdrows[buid] = lp.add_row(std::move(row));
    }

    // Piecewise heat rate curve — NO LONGER built here.  ``GeneratorLP``
    // owns the piecewise dispatch-cost formulation via convex slack
    // columns (source/generator_lp.cpp:272+).  See the deprecation
    // warning at the top of this function for migration guidance.
    // The legacy Commitment.pmax_segments / heat_rate_segments fields
    // are still parsed for back-compat but no longer wired into the
    // LP from CommitmentLP — preventing double-counting if both the
    // legacy Commitment-side and new Generator-side fields are
    // populated.

    prev_block_ucol = ucol;
    prev_gcol = gcol;
    first_block = false;
  }

  // ── u-gate piecewise heat-rate kink rows ──
  //
  // ``GeneratorLP::add_to_lp`` emits the per-segment piecewise kink
  // rows as
  //
  //     p − δ_k ≤ pmax_segs[k-1]               (constant RHS)
  //
  // independent of any Commitment.  For a renewable-only Generator
  // (no Commitment) that is correct: the breakpoint is a physical
  // capacity threshold.  For a Generator gated by a Commitment, the
  // constant breakpoint over-relaxes the LP — with fractional ``u``
  // the dispatch ``p`` can still climb to ``pmax × u`` (from the
  // C2 upper row) and pay only marginal-segment cost, even though
  // physically the gen is "half on" and shouldn't have access to
  // the cheap segments at full width.  The pre-``refactor(commitment)``
  // code put piecewise on Commitment and explicitly gated each
  // segment by ``u``; the refactor moved piecewise to Generator and
  // dropped that gating, producing a looser LP-relax that drives
  // fractional ``u`` in cases where the integer optimum has ``u=1``
  // (22 UC.jl golden / real-case tests broke).
  //
  // Retro-fit: change every kink row from
  //
  //     p − δ_k ≤ pmax_segs[k-1]
  //
  // to
  //
  //     p − δ_k − pmax_segs[k-1] · u ≤ 0
  //
  // i.e. add the ``-pmax_segs[k-1]`` coefficient on ``ucol`` and
  // reset RHS to 0.  At ``u=1`` the row is algebraically identical
  // (renewable-only case unaffected because no Commitment runs
  // this block).  At ``u<1`` the segment becomes proportionally
  // tighter, restoring the pre-refactor LP-relax behaviour.
  if (!block_ucol.empty()) {
    const auto& kink_rows = generator_lp.heat_rate_kink_rows();
    const auto& breakpoints = generator_lp.heat_rate_kink_breakpoints();
    if (!kink_rows.empty() && kink_rows.size() == breakpoints.size()) {
      for (size_t k = 0; k < kink_rows.size(); ++k) {
        const auto kit = kink_rows[k].find(st_key);
        if (kit == kink_rows[k].end()) {
          continue;
        }
        const double breakpoint_k = breakpoints[k];
        for (const auto& [buid, krow] : kit->second) {
          const auto ucol_it = block_ucol.find(buid);
          if (ucol_it == block_ucol.end()) {
            continue;
          }
          auto& row = lp.row_at(krow);
          row[ucol_it->second] = -breakpoint_k;
          row.uppb = 0.0;
        }
      }
    }
  }

  // ── Reserve-UC integration ──
  // Modify existing reserve provision headroom rows to be conditional on u.
  // Up-provision:   g + r_up ≤ Pmax      →  g + r_up - Pmax·u ≤ 0
  // Down-provision: g - r_dn ≥ Pmin      →  g - r_dn - Pmin·u ≥ 0
  if (!block_ucol.empty()) {
    for (const auto& rprov : sc.elements<ReserveProvisionLP>()) {
      if (rprov.generator_sid() != generator_sid()) {
        continue;
      }
      for (const auto& block : blocks) {
        const auto buid = block.uid();
        const auto ucol_it = block_ucol.find(buid);
        if (ucol_it == block_ucol.end()) {
          continue;
        }
        const auto ucol = ucol_it->second;

        // Modify up-provision row: add -Pmax on u, change RHS to 0
        if (const auto urow =
                rprov.lookup_up_provision_row(scenario, stage, buid))
        {
          auto& row = lp.row_at(*urow);
          const auto pmax = row.uppb;  // original Pmax was stored as uppb
          lp.set_coeff(*urow, ucol, -pmax);
          row.uppb = 0.0;
        }

        // Modify down-provision row: add -Pmin on u, change RHS to 0
        if (const auto drow =
                rprov.lookup_dn_provision_row(scenario, stage, buid))
        {
          auto& row = lp.row_at(*drow);
          const auto pmin = row.lowb;  // original Pmin was stored as lowb
          lp.set_coeff(*drow, ucol, -pmin);
          row.lowb = 0.0;
        }

        // ── PLEXOS Min Provision linkage (urmin/drmin) ──
        //
        // ARCHITECTURAL DEBT (queued for refactor): per the
        // "consumer pulls from owner" principle, this transform
        // should live in ``ReserveProvisionLP::add_to_lp``, looking
        // up the gen's CommitmentLP via ``sc.elements<CommitmentLP>()``
        // and constructing the linkage row at provision creation time.
        // Implementing that requires (a) reordering ``collections_t``
        // so CommitmentLP precedes ReserveProvisionLP and (b) saving
        // the original gen-col bounds before CommitmentLP resets
        // ``gcol.lowb = 0`` (consumers like inertia_provision_lp.cpp
        // and the dprov_row helper in reserve_provision_lp.cpp:298
        // currently read ``lp.get_col_lowb(gcol)`` expecting the
        // original pmin).  This block keeps the legacy "CommitmentLP
        // transforms ReserveProvisionLP-owned rows" pattern that's
        // already pre-existing (see the urmax/drmax × u transform
        // a few lines above).
        //
        // ReserveProvisionLP::add_to_lp creates the provision column
        // with ``lowb = urmin`` as a hard floor (when PLEXOS
        // ``ReserveGenerators.Min Provision`` is set on the
        // membership).  Per PLEXOS docs (ReserveGenerators.MinProvision)
        // the floor is gated by ``Available Units`` — i.e. should
        // collapse to 0 when the gen is not committed.  Here, since
        // this gen has a commitment, transform the hard col-lowb
        // into a conditional linkage row:
        //   provision_col - urmin·u >= 0
        // and reset the col lowb to 0 so the LP only enforces the
        // floor when u = 1.
        if (const auto up_col_opt =
                rprov.lookup_up_provision_col(scenario, stage, buid))
        {
          const auto up_col = *up_col_opt;
          auto& col = lp.col_at(up_col);
          if (col.lowb > 0.0) {
            const auto urmin = col.lowb;
            auto urow =
                SparseRow {
                    .class_name = ReserveProvisionLP::Element::class_name,
                    .constraint_name = "min_up_provision",
                    .variable_uid = rprov.uid(),
                    .context = make_block_context(
                        scenario.uid(), stage.uid(), block.uid()),
                }
                    .greater_equal(0);
            urow[up_col] = 1.0;
            urow[ucol] = -urmin;
            std::ignore = lp.add_row(std::move(urow));
            col.lowb = 0.0;
          }
        }
        if (const auto dn_col_opt =
                rprov.lookup_dn_provision_col(scenario, stage, buid))
        {
          const auto dn_col = *dn_col_opt;
          auto& col = lp.col_at(dn_col);
          if (col.lowb > 0.0) {
            const auto drmin = col.lowb;
            auto drow =
                SparseRow {
                    .class_name = ReserveProvisionLP::Element::class_name,
                    .constraint_name = "min_dn_provision",
                    .variable_uid = rprov.uid(),
                    .context = make_block_context(
                        scenario.uid(), stage.uid(), block.uid()),
                }
                    .greater_equal(0);
            drow[dn_col] = 1.0;
            drow[ucol] = -drmin;
            std::ignore = lp.add_row(std::move(drow));
            col.lowb = 0.0;
          }
        }
      }
    }
  }

  // Inertia/Reserve u_commit linkage: NOT done here.  Architectural
  // principle (consumer pulls from owner): each provision LP class
  // (ReserveProvisionLP, InertiaProvisionLP) should look up its
  // generator's CommitmentLP and integrate the u_commit linkage in
  // its OWN add_to_lp — keeps the responsibility close to the
  // provision construction and avoids concentrating cross-class
  // knowledge inside CommitmentLP.  This requires reordering
  // ``collections_t`` so CommitmentLP precedes the provision LPs
  // (so u_commit columns exist when consumers look them up).
  //
  // The legacy reserve-headroom block above (urmax × u, drmax × u,
  // and the urmin/drmin x u floors) still lives here for back-compat
  // and is on the deferred-refactor list.  Same for the Battery
  // pmin_charge/discharge gating in ConverterLP — Battery LP should
  // pull from Converter LP rather than have Converter own the
  // binaries.

  // ── C6: Min up time (at period level) ──
  // Σ_{q=p}^{min(p+UT_periods-1,P)} u[q] ≥ UT_periods · v[p]
  // Accumulates period durations to find how many periods cover min_up_hours.
  const auto min_up_hours = commitment().min_up_time.value_or(0.0);
  if (min_up_hours > 0.0 && !ucols.empty() && !vcols.empty()) {
    BIndexHolder<RowIndex> mut_rows;
    map_reserve(mut_rows, nperiods);

    // Compute period durations
    std::vector<double> period_dur(nperiods);
    for (size_t p = 0; p < nperiods; ++p) {
      const auto pstart = period_starts[p];
      const auto pend =
          (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double dur = 0.0;
      for (size_t i = pstart; i < pend; ++i) {
        dur += blocks[i].duration();
      }
      period_dur[p] = dur;
    }

    for (size_t p = 0; p < nperiods; ++p) {
      const auto rep_buid = blocks[period_starts[p]].uid();
      const auto vcol_it = vcols.find(rep_buid);
      if (vcol_it == vcols.end()) {
        continue;
      }

      // Count how many periods from p onward cover min_up_hours
      double accum_hours = 0.0;
      size_t ut_periods = 0;
      for (size_t q = p; q < nperiods && accum_hours < min_up_hours; ++q) {
        accum_hours += period_dur[q];
        ++ut_periods;
      }
      if (ut_periods <= 1) {
        continue;  // trivially satisfied
      }

      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = MinUpTimeName,
              .variable_uid = cuid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), rep_buid),
          }
              .greater_equal(0.0);
      // Σ u[q] - UT_periods · v[p] ≥ 0
      for (size_t q = p; q < p + ut_periods && q < nperiods; ++q) {
        const auto q_buid = blocks[period_starts[q]].uid();
        const auto ucol_it = ucols.find(q_buid);
        if (ucol_it != ucols.end()) {
          row[ucol_it->second] = 1.0;
        }
      }
      row[vcol_it->second] = -static_cast<double>(ut_periods);
      mut_rows[rep_buid] = lp.add_row(std::move(row));
    }
    if (!mut_rows.empty()) {
      min_up_time_rows_[st_key] = std::move(mut_rows);
    }
  }

  // ── C7: Min down time (at period level) ──
  // Σ_{q=p}^{min(p+DT_periods-1,P)} (1 - u[q]) ≥ DT_periods · w[p]
  // Rearranged: Σ u[q] + DT_periods · w[p] ≤ span
  const auto min_down_hours = commitment().min_down_time.value_or(0.0);
  if (min_down_hours > 0.0 && !ucols.empty() && !wcols.empty()) {
    BIndexHolder<RowIndex> mdt_rows;
    map_reserve(mdt_rows, nperiods);

    // Reuse or recompute period_dur if not already computed
    std::vector<double> pd(nperiods);
    for (size_t p = 0; p < nperiods; ++p) {
      const auto ps = period_starts[p];
      const auto pe = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double dur = 0.0;
      for (size_t i = ps; i < pe; ++i) {
        dur += blocks[i].duration();
      }
      pd[p] = dur;
    }

    for (size_t p = 0; p < nperiods; ++p) {
      const auto rep_buid = blocks[period_starts[p]].uid();
      const auto wcol_it = wcols.find(rep_buid);
      if (wcol_it == wcols.end()) {
        continue;
      }

      double accum_hours = 0.0;
      size_t dt_periods = 0;
      for (size_t q = p; q < nperiods && accum_hours < min_down_hours; ++q) {
        accum_hours += pd[q];
        ++dt_periods;
      }
      if (dt_periods <= 1) {
        continue;
      }

      const auto span = std::min(p + dt_periods, nperiods) - p;
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = MinDownTimeName,
              .variable_uid = cuid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), rep_buid),
          }
              .less_equal(static_cast<double>(span));
      // Σ u[q] + DT_periods · w[p] ≤ span
      for (size_t q = p; q < p + dt_periods && q < nperiods; ++q) {
        const auto q_buid = blocks[period_starts[q]].uid();
        const auto ucol_it = ucols.find(q_buid);
        if (ucol_it != ucols.end()) {
          row[ucol_it->second] = 1.0;
        }
      }
      row[wcol_it->second] = static_cast<double>(dt_periods);
      mdt_rows[rep_buid] = lp.add_row(std::move(row));
    }
    if (!mdt_rows.empty()) {
      min_down_time_rows_[st_key] = std::move(mdt_rows);
    }
  }

  // ── C8/C9/C10: Hot/warm/cold startup cost tiers (per period) ──
  // When all tier parameters are defined, create per-period tier variables
  // y_hot, y_warm, y_cold with constraints:
  //   C8: v[p] = y_hot[p] + y_warm[p] + y_cold[p]  (type selection)
  //   C9: y_hot[p] ≤ Σ w[q] for q in hot window    (recent shutdown)
  //   C10: y_warm[p] ≤ Σ w[q] for q in warm window  (medium offline)
  //   cold start is the residual via C8.
  if (has_startup_tiers && !vcols.empty() && !wcols.empty()) {
    const auto hot_cost = *opt_hot_cost;
    const auto warm_cost = *opt_warm_cost;
    const auto cold_cost = *opt_cold_cost;
    const auto hot_time = *opt_hot_time;
    const auto cold_time = *opt_cold_time;

    // Validate: cold_time must be >= hot_time (cold = longer offline)
    if (cold_time < hot_time) {
      spdlog::warn(
          "Commitment {}: cold_start_time ({}) < hot_start_time ({}), "
          "skipping startup tiers",
          commitment().name,
          cold_time,
          hot_time);
      return true;
    }
    const auto initial_hours_offline =
        (initial_u < 0.5) ? commitment().initial_hours.value_or(1e6) : 0.0;

    // Compute period durations for window counting
    std::vector<double> pdur(nperiods);
    for (size_t p = 0; p < nperiods; ++p) {
      const auto ps = period_starts[p];
      const auto pe = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double dur = 0.0;
      for (size_t i = ps; i < pe; ++i) {
        dur += blocks[i].duration();
      }
      pdur[p] = dur;
    }

    BIndexHolder<ColIndex> hcols;
    BIndexHolder<ColIndex> wmcols;
    BIndexHolder<ColIndex> ccols;
    BIndexHolder<RowIndex> st_rows;
    BIndexHolder<RowIndex> hr_rows;
    BIndexHolder<RowIndex> wr_rows;
    map_reserve(hcols, nperiods);
    map_reserve(wmcols, nperiods);
    map_reserve(ccols, nperiods);
    map_reserve(st_rows, nperiods);
    map_reserve(hr_rows, nperiods);
    map_reserve(wr_rows, nperiods);

    for (size_t p = 0; p < nperiods; ++p) {
      const auto& rep_block = blocks[period_starts[p]];
      const auto rep_buid = rep_block.uid();
      const auto vcol_it = vcols.find(rep_buid);
      if (vcol_it == vcols.end()) {
        continue;
      }

      const auto bctx =
          make_block_context(scenario.uid(), stage.uid(), rep_buid);
      // Start-up tier costs (hot/warm/cold) are once-per-EVENT costs
      // ($/start), NOT per-hour power costs — exactly like the plain
      // startup ``v`` above, they must NOT be multiplied by the block
      // duration (Δt).  Use the no-duration ``cost_factor`` (probability ·
      // discount).  When ``has_startup_tiers`` is set these tiers carry the
      // ENTIRE startup cost (``v_cost`` is forced to 0), so a
      // ``block_ecost`` here would re-introduce the exact Δt inflation the
      // ``v``/``w`` fix removed — guarded by the
      // "Startup TIERS are per-EVENT costs" regression test in
      // ``test/source/test_commitment.cpp``.
      const auto tier_factor = CostHelper::cost_factor(
          scenario.probability_factor(), stage.discount_factor());

      // Tier-column scope / domain / blocks reuse the values computed
      // once for the u/v/w loop above (`period_scope`, `period_domain`,
      // `period_block_uids[p]`).
      const auto tier_blocks_span =
          std::span<const BlockUid> {period_block_uids[p]};

      // Create tier variables with their respective costs.  The
      // master-side ``IntegerVariable`` registry takes ownership of
      // integer-vs-LP-relax and per-period dispatch — feature branch's
      // legacy ``pin_scale`` workaround is subsumed by the registry's
      // bookkeeping.  Per-event cost stays no-Δt (see comment above).
      const auto h_cost = hot_cost * tier_factor;
      auto hcol = sc.add_integer_col(lp,
                                     IntegerVariable::key(scenario,
                                                          stage,
                                                          Element::class_name,
                                                          cuid,
                                                          HotStartName,
                                                          period_scope,
                                                          rep_buid),
                                     SparseCol {
                                         .lowb = 0.0,
                                         .uppb = 1.0,
                                         .cost = h_cost,
                                         .class_name = cname,
                                         .variable_name = HotStartName,
                                         .variable_uid = cuid,
                                         .context = bctx,
                                     },
                                     period_domain,
                                     period_scope,
                                     rep_buid,
                                     tier_blocks_span);
      hcols[rep_buid] = hcol;

      const auto wm_cost = warm_cost * tier_factor;
      auto wmcol = sc.add_integer_col(lp,
                                      IntegerVariable::key(scenario,
                                                           stage,
                                                           Element::class_name,
                                                           cuid,
                                                           WarmStartName,
                                                           period_scope,
                                                           rep_buid),
                                      SparseCol {
                                          .lowb = 0.0,
                                          .uppb = 1.0,
                                          .cost = wm_cost,
                                          .class_name = cname,
                                          .variable_name = WarmStartName,
                                          .variable_uid = cuid,
                                          .context = bctx,
                                      },
                                      period_domain,
                                      period_scope,
                                      rep_buid,
                                      tier_blocks_span);
      wmcols[rep_buid] = wmcol;

      const auto c_cost = cold_cost * tier_factor;
      auto ccol = sc.add_integer_col(lp,
                                     IntegerVariable::key(scenario,
                                                          stage,
                                                          Element::class_name,
                                                          cuid,
                                                          ColdStartName,
                                                          period_scope,
                                                          rep_buid),
                                     SparseCol {
                                         .lowb = 0.0,
                                         .uppb = 1.0,
                                         .cost = c_cost,
                                         .class_name = cname,
                                         .variable_name = ColdStartName,
                                         .variable_uid = cuid,
                                         .context = bctx,
                                     },
                                     period_domain,
                                     period_scope,
                                     rep_buid,
                                     tier_blocks_span);
      ccols[rep_buid] = ccol;

      // C8: v[p] - y_hot[p] - y_warm[p] - y_cold[p] = 0
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = StartupTypeName,
                .variable_uid = cuid,
                .context = bctx,
            }
                .equal(0.0);
        row[vcol_it->second] = 1.0;
        row[hcol] = -1.0;
        row[wmcol] = -1.0;
        row[ccol] = -1.0;
        st_rows[rep_buid] = lp.add_row(std::move(row));
      }

      // Count periods back from p that cover hot_time / cold_time hours
      double accum = 0.0;
      size_t hot_periods = 0;
      for (size_t q = p; q > 0 && accum < hot_time; --q) {
        accum += pdur[q - 1];
        ++hot_periods;
      }

      accum = 0.0;
      size_t cold_periods = 0;
      for (size_t q = p; q > 0 && accum < cold_time; --q) {
        accum += pdur[q - 1];
        ++cold_periods;
      }

      // C9: y_hot[p] ≤ Σ w[q] for q in [p-hot_periods, p-1]
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = HotStartName,
                .variable_uid = cuid,
                .context = bctx,
            }
                .less_equal(0.0);
        row[hcol] = 1.0;
        for (size_t q = (p > hot_periods ? p - hot_periods : 0); q < p; ++q) {
          const auto q_buid = blocks[period_starts[q]].uid();
          const auto wcol_it = wcols.find(q_buid);
          if (wcol_it != wcols.end()) {
            row[wcol_it->second] = -1.0;
          }
        }
        if (p < hot_periods && initial_hours_offline < hot_time) {
          row.uppb = 1.0;
        }
        hr_rows[rep_buid] = lp.add_row(std::move(row));
      }

      // C10: y_warm[p] ≤ Σ w[q] for q in [p-cold_periods, p-hot_periods-1]
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = WarmStartName,
                .variable_uid = cuid,
                .context = bctx,
            }
                .less_equal(0.0);
        row[wmcol] = 1.0;
        const auto warm_start =
            p > cold_periods ? p - cold_periods : static_cast<size_t>(0);
        const auto warm_end =
            p > hot_periods ? p - hot_periods : static_cast<size_t>(0);
        for (size_t q = warm_start; q < warm_end; ++q) {
          const auto q_buid = blocks[period_starts[q]].uid();
          const auto wcol_it = wcols.find(q_buid);
          if (wcol_it != wcols.end()) {
            row[wcol_it->second] = -1.0;
          }
        }
        if (p < cold_periods && initial_hours_offline >= hot_time
            && initial_hours_offline < cold_time)
        {
          row.uppb = 1.0;
        }
        wr_rows[rep_buid] = lp.add_row(std::move(row));
      }
    }

    if (!hcols.empty()) {
      hot_start_cols_[st_key] = std::move(hcols);
    }
    if (!wmcols.empty()) {
      warm_start_cols_[st_key] = std::move(wmcols);
    }
    if (!ccols.empty()) {
      cold_start_cols_[st_key] = std::move(ccols);
    }
    if (!st_rows.empty()) {
      startup_type_rows_[st_key] = std::move(st_rows);
    }
    if (!hr_rows.empty()) {
      hot_start_rows_[st_key] = std::move(hr_rows);
    }
    if (!wr_rows.empty()) {
      warm_start_rows_[st_key] = std::move(wr_rows);
    }
  }

  // ── C9: Startup-count bounds per window (PLEXOS Max Starts + symmetric
  // Min Starts floor) ──
  //
  // Two-sided bound on the cumulative startup count over a rolling time
  // window:
  //
  //     min_starts  ≤  Σ_{p ∈ window} v[p]  ≤  max_starts
  //
  // Defaults: ``min_starts`` unset → 0 (no lower row); ``max_starts``
  // unset → +∞ (no upper row).  Each side emits ONE row per window when
  // the corresponding bound is set; both rows share the same accumulated
  // LHS so the per-window walk runs once even when both bounds apply.
  //
  // Window boundaries are determined by the SHARED ``starts_scope``:
  //   "hour"    →   1.0    (one row per period)
  //   "day"     →  24.0    (cumulative-duration boundary flush, mirror
  //                         of ``UserConstraint.daily_sum``)
  //   "week"    → 168.0    (or stage length if smaller — PLEXOS Max
  //                         Starts Week is the CEN PCP target use case)
  //   "horizon" → +∞       (one row per stage)
  //   N (int)   → N hours  (arbitrary window length)
  //
  // Scopes longer than the stage collapse to ``"horizon"`` (one row per
  // stage).  See ``Commitment::starts_window_hours`` for the resolved
  // mapping rules.
  const bool has_max_starts = commitment().max_starts.has_value();
  const bool has_min_starts = commitment().min_starts.has_value();
  if ((has_max_starts || has_min_starts) && !vcols.empty()) {
    const double window_hours = commitment().starts_window_hours();
    const auto max_starts_v =
        has_max_starts ? static_cast<double>(*commitment().max_starts) : 0.0;
    const auto min_starts_v =
        has_min_starts ? static_cast<double>(*commitment().min_starts) : 0.0;

    BIndexHolder<RowIndex> ms_rows;
    auto fresh_row = [&]() -> SparseRow
    {
      SparseRow r;
      r.class_name = cname;
      r.constraint_name = MaxStartsName;
      r.variable_uid = cuid;
      return r;
    };
    SparseRow window_row = fresh_row();
    bool row_open = false;
    double acc_hours = 0.0;
    BlockUid window_end_block {};

    auto flush_window = [&]()
    {
      if (!row_open) {
        return;
      }
      // Emit upper-bound row when ``max_starts`` is set, lower-bound
      // row when ``min_starts`` is set.  Both share the same LHS
      // (window_row).  Clone the row for the second emission so each
      // gets its own copy with the appropriate sense.
      if (has_max_starts) {
        SparseRow upper_row {window_row};
        auto bound = std::move(upper_row).less_equal(max_starts_v);
        const auto row_idx = lp.add_row(std::move(bound));
        ms_rows[window_end_block] = row_idx;
      }
      // LP-size: a ``min_starts`` of 0 makes the lower row ``Σ v ≥ 0``,
      // which is trivially satisfied (every ``v`` has lowb ≥ 0) and can
      // never bind — skip it.  The row index is never stored, so no
      // dual output is lost.  Write-out rule: none — a constraint that
      // is always slack contributes no primal/dual to any stream.
      if (has_min_starts && min_starts_v > 0.0) {
        SparseRow lower_row {window_row};
        auto bound = std::move(lower_row).greater_equal(min_starts_v);
        // Capture the row index but don't store it in a dedicated
        // holder — sensitivity work on forced-commitment can read the
        // dual from the LP via the row name + window block.
        [[maybe_unused]] const auto lower_idx = lp.add_row(std::move(bound));
      }
      window_row = fresh_row();
      row_open = false;
      acc_hours = 0.0;
    };

    for (size_t p = 0; p < nperiods; ++p) {
      const auto pstart = period_starts[p];
      const auto pend =
          (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double period_hours = 0.0;
      for (size_t i = pstart; i < pend; ++i) {
        period_hours += blocks[i].duration();
      }
      const auto& rep_blk = blocks[pstart];
      if (!row_open) {
        window_row.context =
            make_block_context(scenario.uid(), stage.uid(), rep_blk.uid());
        row_open = true;
      }
      window_row[period_vcol[p]] = 1.0;
      acc_hours += period_hours;
      window_end_block = rep_blk.uid();
      if (window_hours > 0.0 && acc_hours >= window_hours) {
        flush_window();
      }
    }
    flush_window();  // close any open window at stage-end

    if (!ms_rows.empty()) {
      max_starts_rows_[st_key] = std::move(ms_rows);
    }
  }

  // Store index holders
  if (!ucols.empty()) {
    status_cols_[st_key] = std::move(ucols);
  }
  if (!vcols.empty()) {
    startup_cols_[st_key] = std::move(vcols);
  }
  if (!wcols.empty()) {
    shutdown_cols_[st_key] = std::move(wcols);
  }
  if (!lrows.empty()) {
    logic_rows_[st_key] = std::move(lrows);
  }
  if (!gurows.empty()) {
    gen_upper_rows_[st_key] = std::move(gurows);
  }
  if (!glrows.empty()) {
    gen_lower_rows_[st_key] = std::move(glrows);
  }
  if (!erows.empty()) {
    exclusion_rows_[st_key] = std::move(erows);
  }
  if (!rurows.empty()) {
    ramp_up_rows_[st_key] = std::move(rurows);
  }
  if (!rdrows.empty()) {
    ramp_down_rows_[st_key] = std::move(rdrows);
  }

  // Register PAMPL-visible commitment columns so user constraints can
  // reference ``commitment("X").status`` / ``.startup`` / ``.shutdown``.
  // The block-keyed maps were populated above, fanning the period
  // representative column across every block in the period.
  static constexpr auto ampl_name = Element::class_name.snake_case();
  if (!block_ucol.empty()) {
    sc.add_ampl_variable(
        ampl_name, cuid, StatusName, scenario, stage, block_ucol);
  }
  if (!block_vcol.empty()) {
    sc.add_ampl_variable(
        ampl_name, cuid, StartupName, scenario, stage, block_vcol);
  }
  if (!block_wcol.empty()) {
    sc.add_ampl_variable(
        ampl_name, cuid, ShutdownName, scenario, stage, block_wcol);
  }

  return true;
}

bool CommitmentLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // ``status`` / ``startup`` / ``shutdown`` are binary LP
  // variables — emit via the integer-snapping overload so the
  // output parquet carries exact 0/1 values.  See
  // ``OutputContext::add_col_sol_integer`` for the rationale.
  out.add_col_sol_integer(cname, StatusName, pid, status_cols_);
  out.add_col_cost(cname, StatusName, pid, status_cols_);
  out.add_col_sol_integer(cname, StartupName, pid, startup_cols_);
  out.add_col_cost(cname, StartupName, pid, startup_cols_);
  out.add_col_sol_integer(cname, ShutdownName, pid, shutdown_cols_);
  out.add_col_cost(cname, ShutdownName, pid, shutdown_cols_);

  // Max-starts cap row duals (one per scope window, indexed by the
  // window-ending block).  Empty when ``commitment.max_starts`` is
  // unset; the dual surfaces the shadow $-per-extra-startup the LP
  // would pay if the cap were relaxed by 1 event, useful for
  // sensitivity work on cycling-limited units.
  out.add_row_dual(cname, MaxStartsName, pid, max_starts_rows_);

  out.add_row_dual(cname, LogicName, pid, logic_rows_);
  out.add_row_dual(cname, GenUpperName, pid, gen_upper_rows_);
  out.add_row_dual(cname, GenLowerName, pid, gen_lower_rows_);
  out.add_row_dual(cname, ExclusionName, pid, exclusion_rows_);
  out.add_row_dual(cname, RampUpName, pid, ramp_up_rows_);
  out.add_row_dual(cname, RampDownName, pid, ramp_down_rows_);

  // Segment col/row outputs moved to GeneratorLP::add_to_output —
  // piecewise heat-rate slacks now live there alongside the gen
  // columns they augment.

  out.add_row_dual(cname, MinUpTimeName, pid, min_up_time_rows_);
  out.add_row_dual(cname, MinDownTimeName, pid, min_down_time_rows_);

  out.add_col_sol(cname, HotStartName, pid, hot_start_cols_);
  out.add_col_cost(cname, HotStartName, pid, hot_start_cols_);
  out.add_col_sol(cname, WarmStartName, pid, warm_start_cols_);
  out.add_col_cost(cname, WarmStartName, pid, warm_start_cols_);
  out.add_col_sol(cname, ColdStartName, pid, cold_start_cols_);
  out.add_col_cost(cname, ColdStartName, pid, cold_start_cols_);
  out.add_row_dual(cname, StartupTypeName, pid, startup_type_rows_);
  out.add_row_dual(cname, HotStartName, pid, hot_start_rows_);
  out.add_row_dual(cname, WarmStartName, pid, warm_start_rows_);

  return true;
}

}  // namespace gtopt
