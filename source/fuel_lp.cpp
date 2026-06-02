/**
 * @file      fuel_lp.cpp
 * @brief     Implementation of FuelLP — parameter carrier + max-offtake cap
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * FuelLP resolves the Fuel schedules (price, heat content, emission
 * factors, max offtake) at construction.  Generators consume the
 * resolved schedules via the `param_*` accessors.
 *
 * When `Fuel.max_offtake` is set for a (scenario, stage), `add_to_lp`
 * walks every active `GeneratorLP` whose `Generator.fuel == this_fuel`
 * and creates a cap row.  Two modes are supported:
 *
 *   * default (per-stage SUM):
 *       Σ_b Σ_g (heat_rate · gen · dur_b)  ≤  max_offtake(s)
 *     — one row per (scenario, stage).
 *
 *   * `Fuel.max_offtake_per_block = true` (per-block, mirrors PLEXOS):
 *       Σ_g (heat_rate · gen · dur_b)  ≤  max_offtake(s) · dur_b / Σ dur
 *     — one row per (scenario, stage, block); the per-stage cap is
 *     pro-rated by block duration.  Equivalent to enforcing a uniform
 *     per-hour rate cap `= max_offtake / Σ dur`, matching the PLEXOS
 *     `FueMaxOffWeek_<fuel>` Constraint's per-period semantics.
 *
 * Optional slack column priced at `max_offtake_cost(s)` is added with
 * coefficient −1 so the cap can be violated at a per-unit price.  In
 * per-block mode each block has its own slack column.
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

  // Walk generators once; reuse the (col → coefficient) map for the
  // offtake binding equation AND either cap mode.  GeneratorLP runs
  // BEFORE FuelLP in `lp_element_types_t`, so generation_cols_at() is
  // already populated.
  const auto gen_coefs = collect_gen_coefficients(sc, *this, scenario, stage);
  if (gen_coefs.empty()) {
    // No active generators reference this fuel at this stage — neither
    // the offtake DV nor the cap row contributes; skip to keep the LP
    // sparse.  KNOWN LIMITATION: ``fuel("X").offtake`` references on
    // this (scenario, stage) then fail the strict UC resolver because
    // the AMPL attribute "offtake" is not registered for *any* element
    // of the class until at least one FuelLP::add_to_lp call actually
    // creates a DV — the class-attribute leniency in
    // ``element_column_resolver.cpp::element_known_silent_zero``
    // requires a sister-element registration to fire.  This is why the
    // plexos2gtopt emission of ``α × fuel(name).offtake`` is gated
    // OFF by default behind ``GTOPT_USE_FUEL_OFFTAKE=1`` — it works
    // for the canonical test fixture (every fuel has consumers in
    // every stage) but breaks on CEN PCP scale where some
    // Generator → Fuel memberships exist in the XML but the gens are
    // never active in any block at the run date.  TODO: register
    // ``offtake`` as a class-level attribute marker so the leniency
    // catches the missing-DV case without requiring a sister-element.
    return true;
  }

  // ── Unconditional offtake DV + binding equation ──────────────────
  // For every block with at least one active gen, create
  // ``Y_f[b] >= 0`` and bind it via ``Y_f[b] − Σ hr_g·dur_b·gen_g = 0``.
  // Registering ``Y_f`` lets PLEXOS ``Offtake Coefficient`` UCs
  // (``Gas_MaxOpDay*``) translate verbatim as ``fuel("X").offtake``,
  // and lets the cap rows below reference a single column instead of
  // re-summing per-gen coefficients (sparser LP).  The DV is created
  // whenever the fuel has consumers, independent of whether
  // ``Fuel.max_offtake`` is set.
  BIndexHolder<ColIndex> obcols;
  map_reserve(obcols, blocks.size());
  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto coefs_it = gen_coefs.find(buid);
    if (coefs_it == gen_coefs.end() || coefs_it->second.empty()) {
      continue;
    }
    const auto block_ctx = make_block_context(scen_uid, stage_uid, buid);
    const auto ocol = lp.add_col(SparseCol {
        .lowb = 0.0,
        .class_name = cname,
        .variable_name = OfftakeName,
        .variable_uid = uid(),
        .context = block_ctx,
    });
    obcols[buid] = ocol;
    // Binding equation: +Y_f − Σ hr_g·dur_b·gen_g = 0.  Stamp the
    // equality constraint type at construction so the row is ready
    // to receive coefficients directly via ``operator[]``.
    auto bind_row =
        SparseRow {
            .class_name = cname,
            .constraint_name = OfftakeDefName,
            .variable_uid = uid(),
            .context = block_ctx,
        }
            .equal(0.0);
    bind_row[ocol] = 1.0;
    for (const auto& [gcol, coef] : coefs_it->second) {
      const double existing = bind_row[gcol];
      bind_row[gcol] = existing - coef;
    }
    // Definition row, not a physical constraint: its row index is
    // intentionally NOT stored (no member holder) and is NOT routed
    // through ``add_to_output``, so the dual is never written to the
    // output stream — saving a row-dual emission per (fuel, scenario,
    // stage, block).  ``std::ignore`` silences the [[nodiscard]] on
    // LinearProblem::add_row.
    std::ignore = lp.add_row(std::move(bind_row));
  }
  if (!obcols.empty()) {
    offtake_cols_[{scen_uid, stage_uid}] = obcols;
    // Register via the snake_case class label (``fuel``) so PAMPL
    // expressions ``fuel("X").offtake`` resolve.  The LP-row-class
    // names use the full-name form (``Fuel``) — see ``cname`` above
    // for the SparseRow / SparseCol class_name field which feeds LP
    // file labels and CSV outputs.
    static constexpr auto ampl_name = Element::class_name.snake_case();
    sc.add_ampl_variable(
        ampl_name, uid(), OfftakeName, scenario, stage, obcols);
  }

  const auto stage_floor = param_min_offtake(stage_uid);

  if (!stage_cap && !stage_floor) {
    // No cap nor floor set — offtake DV stays exposed (UCs can still
    // reference it), but no cap / floor / slack row is emitted.
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

  // Cap rows reference the offtake DV directly (sparser than re-summing
  // per-gen coefficients here — the binding equation above already
  // accumulates ``Σ hr·dur·gen`` into ``Y_f[b]``).
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
      // per-period `FueMaxOffWeek_<fuel>` semantics.
      BIndexHolder<RowIndex> brows;
      BIndexHolder<ColIndex> bslacks;
      map_reserve(brows, blocks.size());
      if (slack_cost_per_unit > 0.0) {
        map_reserve(bslacks, blocks.size());
      }

      for (const auto& block : blocks) {
        const auto buid = block.uid();
        const auto ocol_it = obcols.find(buid);
        if (ocol_it == obcols.end()) {
          // No offtake DV on this block (no active gens) — skip; the
          // cap is trivially satisfied with zero offtake.
          continue;
        }
        const auto block_ctx = make_block_context(scen_uid, stage_uid, buid);
        SparseRow brow {
            .class_name = cname,
            .constraint_name = MaxOfftakeName,
            .variable_uid = uid(),
            .context = block_ctx,
        };
        brow[ocol_it->second] = 1.0;
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
      // Per-stage SUM (default): Σ_b Y_f[b] ≤ max_offtake.
      SparseRow cap_row {
          .class_name = cname,
          .constraint_name = MaxOfftakeName,
          .variable_uid = uid(),
          .context = stage_ctx,
      };
      for (const auto& [_buid, ocol] : obcols) {
        const double existing = cap_row[ocol];
        cap_row[ocol] = existing + 1.0;
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
  // offtake DV LHS terms but each is independently bounded + dualised.
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
        const auto ocol_it = obcols.find(buid);
        if (ocol_it == obcols.end()) {
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
            .class_name = cname,
            .constraint_name = MinOfftakeName,
            .variable_uid = uid(),
            .context = block_ctx,
        };
        if (ocol_it != obcols.end()) {
          brow[ocol_it->second] = 1.0;
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
          // augmented constraint is ``Σ Y_f + slack ≥ floor``.
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
      // Per-stage SUM floor: Σ_b Y_f[b] + slack ≥ min_offtake.
      SparseRow floor_row {
          .class_name = cname,
          .constraint_name = MinOfftakeName,
          .variable_uid = uid(),
          .context = stage_ctx,
      };
      for (const auto& [_buid, ocol] : obcols) {
        const double existing = floor_row[ocol];
        floor_row[ocol] = existing + 1.0;
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
