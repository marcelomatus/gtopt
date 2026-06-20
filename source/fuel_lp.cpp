/**
 * @file      fuel_lp.cpp
 * @brief     Implementation of FuelLP — parameter carrier + offtake cap/floor
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * FuelLP resolves the Fuel schedules (price, heat content, emission
 * factors, offtake bounds) at construction.  Generators consume the
 * resolved schedules via the `param_*` accessors.
 *
 * ## LP-active constraints — only when explicitly configured
 *
 * When `Fuel.max_offtake` is set for a (scenario, stage), `add_to_lp`
 * walks every active `GeneratorLP` whose `Generator.fuel == this_fuel`
 * and creates a cap row whose LHS is `Σ heat_rate · dur · gen_g`,
 * stamped DIRECTLY on the consuming generators' dispatch columns:
 *
 *   * default (per-stage SUM):
 *       Σ_b Σ_g (heat_rate · gen · dur_b)  ≤  max_offtake(s)
 *
 *   * `Fuel.max_offtake_per_block = true` (per-block, mirrors PLEXOS):
 *       Σ_g (heat_rate · gen · dur_b)  ≤  max_offtake(s) · dur_b / Σ dur
 *
 * The symmetric `Fuel.min_offtake` path emits a `≥` floor row with
 * the same LHS shape and an optional `+1` slack column.
 *
 * ## PAMPL `fuel("X").offtake` accessor — weighted sum, no LP column
 *
 * The aggregate `Σ_g heat_rate_g · dur_b · generation_g[b]` is also
 * exposed to PAMPL as `fuel("X").offtake` via the AMPL
 * `block_cols_weighted_sum` registry — no aggregator LP column, no
 * binding equality row.  Mirror of the EmissionZone.production
 * substitution: the previous design carried a per-(scenario, stage,
 * block) `Y_f[b]` LP column plus an equality row `Y_f[b] − Σ hr·dur·gen
 * = 0`, both of which were pure bookkeeping (zero objective cost,
 * unique-determined-by-gen value).  Removing them shrinks the LP by
 * ~N_fuels × N_blocks columns AND the matching equality rows on every
 * (scene, phase) cell.
 *
 * Optional slack column priced at `*_offtake_cost(s)` is added with
 * coefficient −1 (cap) or +1 (floor) so the bound can be softened at
 * a per-unit price.  In per-block mode each block has its own slack
 * column.
 */

#include <tuple>

#include <gtopt/cost_helper.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

FuelLP::FuelLP(const Fuel& fuel, const InputContext& ic)
    : Base(fuel, ic, Element::class_name)
    , price_(ic, Element::class_name, id(), std::move(object().price))
    , heat_content_(
          ic, Element::class_name, id(), std::move(object().heat_content))
    , combustion_ef_(ic,
                     Element::class_name,
                     id(),
                     std::move(object().combustion_emission_factor))
    , upstream_ef_(ic,
                   Element::class_name,
                   id(),
                   std::move(object().upstream_emission_factor))
    , max_offtake_(
          ic, Element::class_name, id(), std::move(object().max_offtake))
    , max_offtake_cost_(
          ic, Element::class_name, id(), std::move(object().max_offtake_cost))
    , min_offtake_(
          ic, Element::class_name, id(), std::move(object().min_offtake))
    , min_offtake_cost_(
          ic, Element::class_name, id(), std::move(object().min_offtake_cost))
{
}

namespace
{

/// Per-generator (gen_col → coefficient) map indexed by block, built
/// in one walk over `sc.elements<GeneratorLP>()`.  Used by both the
/// per-stage SUM path and the per-block path.
[[nodiscard]] BIndexHolder<std::vector<std::pair<ColIndex, double>>>
collect_gen_coefficients(const SystemContext& sc,
                         const FuelLP& this_fuel,
                         const ScenarioLP& scenario,
                         const StageLP& stage)
{
  BIndexHolder<std::vector<std::pair<ColIndex, double>>> per_block;
  const auto stage_uid = stage.uid();

  const auto this_fuel_uid = this_fuel.uid();
  for (const auto& gen : sc.elements<GeneratorLP>()) {
    if (!gen.is_active(stage)) {
      continue;
    }

    // ── Static-fuel match (legacy path) ────────────────────────────
    // ``static_matches`` is true when ``Generator.fuel`` resolves to
    // ``this_fuel``.  Falls back to ``has_fuel_per_block`` for the
    // Issue #510 per-block-override case where the static fuel may
    // be unset or point to a DIFFERENT default.
    const auto& fuel_ref = gen.generator().fuel;
    const bool static_matches = fuel_ref.has_value()
        && sc.element<FuelLP>(FuelLPSId {fuel_ref.value()}).uid()
            == this_fuel_uid;
    const bool has_pb = gen.has_fuel_per_block();
    if (!static_matches && !has_pb) {
      continue;
    }

    // Tolerant accessor — see `lookup_generation_cols` in
    // GeneratorLP for why this is preferred over `generation_cols_at`.
    const auto& gcols = gen.lookup_generation_cols(scenario, stage);
    for (const auto& block : stage.blocks()) {
      const auto buid = block.uid();
      const auto hr = gen.param_heat_rate(stage_uid, buid).value_or(0.0);
      if (hr <= 0.0) {
        continue;
      }
      const auto it = gcols.find(buid);
      if (it == gcols.end()) {
        continue;
      }

      // ── Per-block fuel-uid bucket (Issue #510 Phase 1) ──────────
      // When the generator has a per-block fuel schedule, gate the
      // contribution: this (gen, block) only counts toward ``this_fuel``
      // when the resolved uid for that cell matches.  Cells whose
      // resolved uid is the sentinel 0 fall back to ``static_matches``
      // — the static-fuel branch above.
      if (has_pb) {
        const auto opt_uid = gen.param_fuel_per_block(stage_uid, buid);
        const auto resolved = opt_uid.value_or(Uid {0});
        const bool block_matches = (resolved != Uid {0})
            ? (resolved == this_fuel_uid)
            : static_matches;
        if (!block_matches) {
          continue;
        }
      }

      // coefficient = heat_rate × block_duration so the LHS
      // (sum of coef × gen_col) evaluates to fuel-units when gen is
      // in MW.  Matches the unit basis of `max_offtake`.
      per_block[buid].emplace_back(it->second, hr * block.duration());
    }
  }

  return per_block;
}

}  // namespace

bool FuelLP::add_to_lp(const SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto stage_uid = stage.uid();
  const auto stage_cap = param_max_offtake(stage_uid);

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto scen_uid = scenario.uid();
  const auto stage_ctx = make_stage_context(scen_uid, stage_uid);

  // Walk generators once; reuse the per-block (col → coefficient)
  // map for the cap/floor row LHS stamps AND the PAMPL weighted-sum
  // registration.  GeneratorLP runs BEFORE FuelLP in
  // `lp_element_types_t`, so generation_cols_at() is already
  // populated.
  const auto gen_coefs = collect_gen_coefficients(sc, *this, scenario, stage);
  if (gen_coefs.empty()) {
    // No active generators reference this fuel at this stage — neither
    // the cap row nor the floor row would contribute, and the PAMPL
    // weighted sum is identically zero.  Skip to keep the LP sparse.
    return true;
  }

  // ── PAMPL `fuel("X").offtake` registration ────────────────────────
  // Expose the weighted sum `Σ_g heat_rate_g · dur_b · generation_g[b]`
  // as an AMPL attribute via the `block_cols_weighted_sum` registry —
  // no aggregator LP column, no binding equality row.  PLEXOS
  // `Offtake Coefficient` UCs (`Gas_MaxOpDay*`, `FueMaxOff*`) thus
  // resolve as a single LHS term `α × fuel("X").offtake` whose legs
  // expand directly onto generator dispatch cols at constraint-build
  // time.  Mirror of the EmissionZone.production substitute-out.
  static constexpr auto ampl_name = Element::class_name.snake_case();
  sc.add_ampl_variable(
      ampl_name, uid(), OfftakeName, scenario, stage, gen_coefs);

  const auto stage_floor = param_min_offtake(stage_uid);

  if (!stage_cap && !stage_floor) {
    // No cap nor floor set — PAMPL `fuel("X").offtake` stays exposed
    // (UCs can still reference it via the weighted-sum registration
    // above), but no cap / floor / slack row is emitted.  This is the
    // common case for fuels carrying only a price + emission factors.
    return true;
  }

  // Slack cost (used in both modes if max_offtake_cost is set).
  // Multiply by `cost_factor(prob, disc)` — the 2-arg overload with
  // duration defaulting to 1.0 — so the slack-cost coefficient stays
  // in $/fuel-unit terms.  Using `scenario_stage_ecost` here would
  // additionally multiply by `stage.duration()`, over-penalising
  // multi-hour stages because the slack is a stage-total quantity
  // (not a per-time rate).
  const auto stage_cost = param_max_offtake_cost(stage_uid);
  const double slack_cost_per_unit = (stage_cost && *stage_cost > 0.0)
      ? *stage_cost
          * CostHelper::cost_factor(scenario.probability_factor(),
                                    stage.discount_factor())
      : 0.0;

  const auto stage_floor_cost = param_min_offtake_cost(stage_uid);
  const double floor_slack_cost_per_unit =
      (stage_floor_cost && *stage_floor_cost > 0.0) ? *stage_floor_cost
          * CostHelper::cost_factor(scenario.probability_factor(),
                                    stage.discount_factor())
                                                    : 0.0;

  const bool per_block = fuel().max_offtake_per_block.value_or(false);
  const bool floor_per_block = fuel().min_offtake_per_block.value_or(false);

  // Cap / floor rows now stamp their LHS coefficients directly onto
  // consuming generators' dispatch columns from `gen_coefs` (the per-
  // block `(gen_col, heat_rate · duration)` list built once above) —
  // no aggregator LP column, no binding equality row.
  //
  // Total stage duration is needed by either side's per-block path
  // (pro-rates the per-stage scalar by each block's share); compute
  // once.
  double total_duration = 0.0;
  for (const auto& block : blocks) {
    total_duration += block.duration();
  }
  if (total_duration <= 0.0) {
    return true;
  }

  const auto st_key = std::tuple {scen_uid, stage_uid};

  // ── Upper-bound (max_offtake) path ─────────────────────────────────
  if (stage_cap) {
    if (per_block) {
      // Per-block: pro-rate the stage cap by each block's share of
      // stage duration — equivalent to enforcing a uniform per-hour
      // rate cap `max_offtake / total_duration`.  Mirrors PLEXOS's
      // per-period `FueMaxOffWeek_<fuel>` semantics.  LHS is
      // `Σ_g heat_rate_g · dur · generation_g[b]` stamped directly on
      // each consuming generator's dispatch column.
      BIndexHolder<RowIndex> brows;
      BIndexHolder<ColIndex> bslacks;
      map_reserve(brows, blocks.size());
      if (slack_cost_per_unit > 0.0) {
        map_reserve(bslacks, blocks.size());
      }

      for (const auto& block : blocks) {
        const auto buid = block.uid();
        const auto coefs_it = gen_coefs.find(buid);
        if (coefs_it == gen_coefs.end() || coefs_it->second.empty()) {
          // No active consumers on this block — the cap is trivially
          // satisfied with zero offtake; skip the row.
          continue;
        }
        const auto block_ctx = make_block_context(scen_uid, stage_uid, buid);
        SparseRow brow {
            // Fuel-offtake cap dual is a $/fuel-unit scarcity price
            // (commodity / duration-independent → Energy time-basis).
            .cost_scale_type = ConstraintScaleType::Energy,
            .class_name = cname,
            .constraint_name = MaxOfftakeName,
            .variable_uid = uid(),
            .context = block_ctx,
        };
        for (const auto& [gcol, coef] : coefs_it->second) {
          const double existing = brow[gcol];
          brow[gcol] = existing + coef;
        }
        if (slack_cost_per_unit > 0.0) {
          const auto slack_col = lp.add_col(SparseCol {
              .lowb = 0.0,
              .cost = slack_cost_per_unit,
              .class_name = cname,
              .variable_name = MaxOfftakeSlackName,
              .variable_uid = uid(),
              .context = block_ctx,
          });
          brow[slack_col] = -1.0;
          bslacks[buid] = slack_col;
        }
        const double block_cap = *stage_cap * block.duration() / total_duration;
        brows[buid] = lp.add_row(std::move(brow).less_equal(block_cap));
      }

      if (!brows.empty()) {
        max_offtake_block_rows_[st_key] = std::move(brows);
      }
      if (!bslacks.empty()) {
        max_offtake_block_slack_cols_[st_key] = std::move(bslacks);
      }
    } else {
      // Per-stage SUM (default): Σ_b Σ_g (hr · dur · gen_g) ≤ max_offtake.
      SparseRow cap_row {
          // Fuel-offtake cap dual = $/fuel-unit (Energy time-basis).
          .cost_scale_type = ConstraintScaleType::Energy,
          .class_name = cname,
          .constraint_name = MaxOfftakeName,
          .variable_uid = uid(),
          .context = stage_ctx,
      };
      for (const auto& [_buid, legs] : gen_coefs) {
        for (const auto& [gcol, coef] : legs) {
          const double existing = cap_row[gcol];
          cap_row[gcol] = existing + coef;
        }
      }

      if (slack_cost_per_unit > 0.0) {
        const auto slack_col = lp.add_col(SparseCol {
            .lowb = 0.0,
            .cost = slack_cost_per_unit,
            .class_name = cname,
            .variable_name = MaxOfftakeSlackName,
            .variable_uid = uid(),
            .context = stage_ctx,
        });
        cap_row[slack_col] = -1.0;
        max_offtake_slack_cols_[st_key] = slack_col;
      }
      max_offtake_rows_[st_key] =
          lp.add_row(std::move(cap_row).less_equal(*stage_cap));
    }
  }

  // ── Lower-bound (min_offtake) FLOOR path — symmetric to max ────────
  // Same shape as the max-side branch (per-block pro-rate or per-stage
  // SUM), but the row sense is ``≥`` and the slack column enters with
  // coefficient +1 so it ABSORBS shortfall (LHS + slack ≥ RHS).  When
  // both sides are populated FuelLP emits both rows — they share the
  // same generator-side LHS coefficients but each is independently
  // bounded + dualised.
  if (stage_floor) {
    if (floor_per_block) {
      BIndexHolder<RowIndex> brows;
      BIndexHolder<ColIndex> bslacks;
      map_reserve(brows, blocks.size());
      if (floor_slack_cost_per_unit > 0.0) {
        map_reserve(bslacks, blocks.size());
      }

      for (const auto& block : blocks) {
        const auto buid = block.uid();
        const auto coefs_it = gen_coefs.find(buid);
        const bool has_gens =
            coefs_it != gen_coefs.end() && !coefs_it->second.empty();
        if (!has_gens) {
          // No active gens on this block — floor is unreachable
          // without slack; skip when soft (the per-block slack alone
          // would satisfy a `0 + slack ≥ floor` row at full price),
          // emit the hard row so the LP errors loudly when the user
          // mis-configured an unreachable floor.
          if (floor_slack_cost_per_unit > 0.0) {
            continue;
          }
        }
        const auto block_ctx = make_block_context(scen_uid, stage_uid, buid);
        SparseRow brow {
            // Fuel-offtake floor dual = $/fuel-unit (Energy time-basis).
            .cost_scale_type = ConstraintScaleType::Energy,
            .class_name = cname,
            .constraint_name = MinOfftakeName,
            .variable_uid = uid(),
            .context = block_ctx,
        };
        if (has_gens) {
          for (const auto& [gcol, coef] : coefs_it->second) {
            const double existing = brow[gcol];
            brow[gcol] = existing + coef;
          }
        }
        if (floor_slack_cost_per_unit > 0.0) {
          const auto slack_col = lp.add_col(SparseCol {
              .lowb = 0.0,
              .cost = floor_slack_cost_per_unit,
              .class_name = cname,
              .variable_name = MinOfftakeSlackName,
              .variable_uid = uid(),
              .context = block_ctx,
          });
          // +1 — the slack absorbs shortfall on the ≥ row, so the
          // augmented constraint is ``Σ hr·dur·gen + slack ≥ floor``.
          brow[slack_col] = 1.0;
          bslacks[buid] = slack_col;
        }
        const double block_floor =
            *stage_floor * block.duration() / total_duration;
        brows[buid] = lp.add_row(std::move(brow).greater_equal(block_floor));
      }

      if (!brows.empty()) {
        min_offtake_block_rows_[st_key] = std::move(brows);
      }
      if (!bslacks.empty()) {
        min_offtake_block_slack_cols_[st_key] = std::move(bslacks);
      }
    } else {
      // Per-stage SUM floor: Σ_b Σ_g (hr · dur · gen_g) + slack ≥ min_offtake.
      SparseRow floor_row {
          // Fuel-offtake floor dual = $/fuel-unit (Energy time-basis).
          .cost_scale_type = ConstraintScaleType::Energy,
          .class_name = cname,
          .constraint_name = MinOfftakeName,
          .variable_uid = uid(),
          .context = stage_ctx,
      };
      for (const auto& [_buid, legs] : gen_coefs) {
        for (const auto& [gcol, coef] : legs) {
          const double existing = floor_row[gcol];
          floor_row[gcol] = existing + coef;
        }
      }

      if (floor_slack_cost_per_unit > 0.0) {
        const auto slack_col = lp.add_col(SparseCol {
            .lowb = 0.0,
            .cost = floor_slack_cost_per_unit,
            .class_name = cname,
            .variable_name = MinOfftakeSlackName,
            .variable_uid = uid(),
            .context = stage_ctx,
        });
        floor_row[slack_col] = 1.0;  // shortfall slack (+1)
        min_offtake_slack_cols_[st_key] = slack_col;
      }
      min_offtake_rows_[st_key] =
          lp.add_row(std::move(floor_row).greater_equal(*stage_floor));
    }
  }

  return true;
}

bool FuelLP::add_to_output(OutputContext& out) const
{
  const bool any_max =
      !max_offtake_rows_.empty() || !max_offtake_block_rows_.empty();
  const bool any_min =
      !min_offtake_rows_.empty() || !min_offtake_block_rows_.empty();
  if (!any_max && !any_min) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  if (!max_offtake_rows_.empty()) {
    out.add_row_dual(cname, MaxOfftakeName, pid, max_offtake_rows_);
  }
  if (!max_offtake_slack_cols_.empty()) {
    out.add_col_sol(cname, MaxOfftakeSlackName, pid, max_offtake_slack_cols_);
    out.add_col_cost(cname, MaxOfftakeSlackName, pid, max_offtake_slack_cols_);
  }
  if (!max_offtake_block_rows_.empty()) {
    // Per-block fuel-offtake cap `Σ hr·dur·gen ≤ cap` — its dual is a
    // $/fuel-unit scarcity price (duration-independent).  The rows carry
    // `cost_scale_type = Energy` (set at add_to_lp), so OutputContext
    // back-scales WITHOUT the 1/duration term.
    out.add_row_dual(cname, MaxOfftakeName, pid, max_offtake_block_rows_);
  }
  if (!max_offtake_block_slack_cols_.empty()) {
    out.add_col_sol(
        cname, MaxOfftakeSlackName, pid, max_offtake_block_slack_cols_);
    out.add_col_cost(
        cname, MaxOfftakeSlackName, pid, max_offtake_block_slack_cols_);
  }
  if (!min_offtake_rows_.empty()) {
    out.add_row_dual(cname, MinOfftakeName, pid, min_offtake_rows_);
  }
  if (!min_offtake_slack_cols_.empty()) {
    out.add_col_sol(cname, MinOfftakeSlackName, pid, min_offtake_slack_cols_);
    out.add_col_cost(cname, MinOfftakeSlackName, pid, min_offtake_slack_cols_);
  }
  if (!min_offtake_block_rows_.empty()) {
    // Per-block fuel-offtake floor — dual is a $/fuel-unit price; same
    // Energy `cost_scale_type` (duration-free back-scale) as the
    // max-offtake block dual above.
    out.add_row_dual(cname, MinOfftakeName, pid, min_offtake_block_rows_);
  }
  if (!min_offtake_block_slack_cols_.empty()) {
    out.add_col_sol(
        cname, MinOfftakeSlackName, pid, min_offtake_block_slack_cols_);
    out.add_col_cost(
        cname, MinOfftakeSlackName, pid, min_offtake_block_slack_cols_);
  }
  return true;
}

}  // namespace gtopt
