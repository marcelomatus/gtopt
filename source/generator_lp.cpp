/**
 * @file      generator_lp.cpp
 * @brief     Implementation of GeneratorLP class for generator LP formulation
 * @date      Tue Apr  1 22:03:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the GeneratorLP class, which handles the
 * representation of power generators in linear programming problems. It
 * includes methods to:
 * - Create variables for generation across time blocks
 * - Add capacity constraints
 * - Add bus power balance contributions
 * - Process generation costs in the objective function
 * - Output planning results for generation variables
 */

#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/**
 * @brief Constructs a GeneratorLP from a Generator
 * @param ic Input context for parameter processing
 * @param generator Generator object to convert to LP representation
 *
 * Creates an LP representation of a generator including time-dependent
 * parameters like minimum/maximum generation limits, loss factors, and costs.
 */
GeneratorLP::GeneratorLP(const Generator& generator, const InputContext& ic)
    : CapacityBase(generator, ic, Element::class_name)
    , pmin(ic, Element::class_name, id(), std::move(object().pmin))
    , pmax(ic, Element::class_name, id(), std::move(object().pmax))
    , lossfactor(ic, Element::class_name, id(), std::move(object().lossfactor))
    , gcost(ic, Element::class_name, id(), std::move(object().gcost))
    , heat_rate(ic, Element::class_name, id(), std::move(object().heat_rate))
    , emission_rate(
          ic, Element::class_name, id(), std::move(object().emission_rate))
{
  // Pre-size the per-segment slack-column vector so add_to_lp can
  // index into it by segment index `k = 1..K-1` (the cheapest
  // segment k=0 rides on the primary `generation_cols` entry).
  if (has_heat_rate_segments()) {
    const auto n_segments = object().heat_rate_segments.size();
    if (n_segments > 1) {
      heat_rate_slack_cols_.resize(n_segments - 1);
    }
  }
  SPDLOG_DEBUG("GeneratorLP created for generator with uid {}", uid());
}

/**
 * @brief Adds generator variables and constraints to the linear problem
 * @param sc System context containing current state
 * @param lp Linear problem to add variables and constraints to
 * @return True if successful, false otherwise
 *
 * This method creates:
 * 1. Generation variables for each time block
 * 2. Capacity constraints linking generation to installed capacity
 * 3. Contributions to bus power balance equations
 *
 * It handles:
 * - Time-dependent generation limits
 * - Generator loss factors
 * - Generation costs in the objective function
 * - Capacity constraints when capacity expansion is modeled
 */
bool GeneratorLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) [[unlikely]]
  {
    return false;
  }

  // Register filter metadata (F9) so `sum(generator(all : type="hydro")...)`
  // predicates can be evaluated at row-assembly time.
  {
    AmplElementMetadata metadata;
    metadata.reserve(2);
    if (const auto& t = generator().type) {
      metadata.emplace_back(TypeKey, *t);
    }
    // Resolve via `sc.element<BusLP>` (handles both Uid and Name forms
    // of the JSON-side `bus` SingleId variant — `std::get<Uid>` would
    // throw if the JSON used a string name).
    metadata.emplace_back(
        BusKey, static_cast<double>(sc.element<BusLP>(bus_sid()).uid()));
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus = sc.element<BusLP>(bus_sid());
  if (!bus.is_active(stage)) [[unlikely]] {
    return true;
  }

  auto&& [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);

  // ── Resolve fuel parameters (PLEXOS-style FK + heat-rate model) ─────
  // When `Generator.fuel` is set together with either `heat_rate`
  // (per-(stage, block) scalar/list) or `heat_rate_segments`
  // (piecewise), the per-MWh fuel cost is derived as
  // `fuel.price × heat_rate_slope` and added to the existing `gcost`
  // (treated as a non-combustion variable adder).  When `fuel` is
  // unset, the fuel-derived cost is zero and the column carries
  // `gcost` alone (legacy behaviour).
  const FuelLP* fuel_lp = nullptr;
  if (const auto& fuel_ref = generator().fuel; fuel_ref.has_value()) {
    fuel_lp = &sc.element<FuelLP>(FuelLPSId {fuel_ref.value()});
  }
  const double stage_fuel_price = (fuel_lp != nullptr)
      ? fuel_lp->param_price(stage.uid()).value_or(0.0)
      : 0.0;
  const auto& hr_segs = generator().heat_rate_segments;
  const auto& pmax_segs_arr = generator().pmax_segments;
  const bool has_pw = has_heat_rate_segments();

  // Per-MWh cost of segment k (cheapest first, k = 0), evaluated at a
  // given (stage, block) gcost / heat_rate.  All three coefficients
  // (`gcost`, `heat_rate`, `lossfactor`) are now per-(stage, block);
  // fuel price remains per-stage.
  const auto slope_cost_per_mwh = [&](double hr_slope, double block_gcost)
  { return (stage_fuel_price * hr_slope) + block_gcost; };

  // Effective slope for the primary `generation` column at a given
  // block.  When piecewise: slope of the cheapest segment; otherwise
  // scalar heat rate (or 0 when no fuel/heat_rate is configured).
  const auto primary_slope_cost_at =
      [&](double block_gcost, double block_heat_rate)
  {
    if (has_pw) {
      return slope_cost_per_mwh(hr_segs.front(), block_gcost);
    }
    if (fuel_lp != nullptr) {
      return slope_cost_per_mwh(block_heat_rate, block_gcost);
    }
    return block_gcost;
  };

  const auto& balance_rows = bus.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> gcols;
  BIndexHolder<RowIndex> crows;
  map_reserve(gcols, blocks.size());
  // `crows` is populated only on the expansion branch
  // (`if (capacity_col)` below).  For generators without
  // expansion (the majority at Juan scale), the reserved buckets
  // would be allocated and discarded.
  if (capacity_col) {
    map_reserve(crows, blocks.size());
  }

  const auto guid = uid();
  // Pre-compute st_key here (used both by the block loop's heat-rate
  // slack stash AND by the post-loop AMPL registration / output
  // bookkeeping).
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  for (auto&& block : blocks) {
    const auto buid = block.uid();

    const auto [block_pmax, block_pmin] =
        sc.block_maxmin_at(stage, block, pmax, pmin, stage_capacity);

    // P1 LP-size: when both bounds are zero generation is fixed at
    // zero — the LP column, the bus-balance coefficient, and the
    // (gen ≤ capacity) capacity row are all degenerate.  Skip the
    // whole block (saves 1 col + up to 1 row per offline-gen block).
    if (block_pmax == 0.0 && block_pmin == 0.0) [[unlikely]] {
      continue;
    }

    const auto balance_row = balance_rows.at(buid);

    SPDLOG_DEBUG(
        "GeneratorLP::add_to_lp: gen {} stage {} block {} pmin {} "
        "pmax {} capacity {}",
        guid,
        stage.uid(),
        block.uid(),
        block_pmin,
        block_pmax,
        stage_capacity);

    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());

    // Per-(stage, block) cost coefficients; fall back to 0 when unset.
    const auto block_gcost =
        gcost.optval(stage.uid(), block.uid()).value_or(0.0);
    const auto block_heat_rate =
        heat_rate.optval(stage.uid(), block.uid()).value_or(0.0);
    const auto block_lossfactor =
        lossfactor.optval(stage.uid(), block.uid()).value_or(0.0);

    // Validation: scalar `heat_rate` and `heat_rate_segments` are
    // mutually exclusive — earlier validate_planning has already
    // emitted the user-facing error; defensively short-circuit here.
    if (has_pw && block_heat_rate > 0.0) [[unlikely]] {
      SPDLOG_WARN(
          "GeneratorLP uid={}: both `heat_rate` and `heat_rate_segments` "
          "are set — using segments, ignoring scalar heat_rate.",
          uid());
    }

    const auto block_primary_slope_cost =
        primary_slope_cost_at(block_gcost, block_heat_rate);

    // Create generation variable for this time block.  Cost on the
    // primary column carries the cheapest-segment slope (or scalar
    // heat rate, or plain gcost when no fuel is set).
    const auto gcol = lp.add_col({
        .lowb = block_pmin,
        .uppb = block_pmax,
        .cost = CostHelper::block_ecost(
            scenario, stage, block, block_primary_slope_cost),
        .class_name = Element::class_name.full_name(),
        .variable_name = GenerationName,
        .variable_uid = guid,
        .context = block_ctx,
    });
    gcols[buid] = gcol;

    // ── Stash per-block cost-stack components (physical $/MWh) ─────────
    // VOM = block_gcost; Fuel = primary slope minus VOM (heat_rate ·
    // fuel.price); SRMC = primary slope = VOM + Fuel.  Emitted later
    // as `Generator/{vom_cost,fuel_cost,srmc}_sol.parquet` for the
    // dispatch-cost-stack analysis (Path A — source-schedule
    // reconstruction; no LP scaling involved).
    vom_cost_values_[st_key][buid] = block_gcost;
    fuel_cost_values_[st_key][buid] = block_primary_slope_cost - block_gcost;
    srmc_values_[st_key][buid] = block_primary_slope_cost;

    // Add generator output to the bus power balance equation
    // Factor (1-lossfactor) accounts for generator losses
    auto& brow = lp.row_at(balance_row);
    brow[gcol] = 1 - block_lossfactor;

    // Add capacity constraint if capacity expansion is modeled
    // Ensures generation <= installed capacity
    if (capacity_col) {
      auto crow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = CapacityName,
              .variable_uid = guid,
              .context = block_ctx,
          }
              .greater_equal(0);
      crow[*capacity_col] = 1;
      crow[gcol] = -1;

      crows[buid] = lp.add_row(std::move(crow));
    }

    // ── Piecewise heat-rate slacks ────────────────────────────────────
    // For each higher segment k = 1..K-1 add a slack `s_k ≥ 0` with
    // marginal cost `(h_k − h_{k-1}) × fuel.price` ($/MWh) and a kink
    // row `p − s_k ≤ pmax_segments[k-1]`.  Convexity (strictly
    // increasing heat rates) lets the LP pick segments in order
    // without any binary or SOS-2; convexity is enforced separately
    // in validate_planning.
    if (has_pw && hr_segs.size() > 1) {
      const auto K = hr_segs.size();
      for (std::size_t k = 1; k < K; ++k) {
        const double marginal_slope = hr_segs[k] - hr_segs[k - 1];
        const double slack_cost_per_mwh = stage_fuel_price * marginal_slope;
        // Disambiguate the per-segment slack/kink (same class+var+uid)
        // by stamping the segment index into the BlockExContext —
        // mirrors how commitment_lp.cpp segments its piecewise rows.
        const auto seg_ctx = make_block_context(
            scenario.uid(), stage.uid(), buid, static_cast<int>(k));
        const auto s_col = lp.add_col({
            .lowb = 0.0,
            .uppb = LinearProblem::DblMax,
            .cost = CostHelper::block_ecost(
                scenario, stage, block, slack_cost_per_mwh),
            .class_name = Element::class_name.full_name(),
            .variable_name = HeatRateSlackName,
            .variable_uid = guid,
            .context = seg_ctx,
        });

        // Kink row: gcol − s_col ≤ pmax_segments[k-1].
        // The breakpoint pmax_segments[k-1] is the cumulative MW at
        // the END of segment (k-1) (= start of segment k).  When p
        // exceeds the breakpoint, s_col ≥ p − breakpoint and the
        // marginal cost above kicks in.
        auto kink_row =
            SparseRow {
                .class_name = Element::class_name.full_name(),
                .constraint_name = HeatRateKinkName,
                .variable_uid = guid,
                .context = seg_ctx,
            }
                .less_equal(pmax_segs_arr[k - 1]);
        kink_row[gcol] = 1.0;
        kink_row[s_col] = -1.0;
        std::ignore = lp.add_row(std::move(kink_row));

        // Stash the slack col per segment index for output / cap
        // refactoring later.
        heat_rate_slack_cols_[k - 1][st_key][buid] = s_col;
      }
    }
  }

  // Store generation and capacity rows for output (st_key was
  // pre-computed before the block loop).
  if (!gcols.empty()) {
    generation_cols[st_key] = std::move(gcols);
    // Register PAMPL-visible columns with the variable registry.
    sc.add_ampl_variable(ampl_name,
                         guid,
                         GenerationName,
                         scenario,
                         stage,
                         generation_cols.at(st_key));
  }
  if (!crows.empty()) {
    capacity_rows[st_key] = std::move(crows);
  }

  // `capainst` is registered centrally by CapacityBase::add_to_lp.

  return true;
}

/**
 * @brief Adds generator output results to the output context
 * @param out Output context to add results to
 * @return True if successful, false otherwise
 *
 * Processes planning results for:
 * - Generation variables (primal solution and costs)
 * - Capacity constraint dual values (shadow prices)
 * - Capacity-related outputs via base class
 */
bool GeneratorLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  const auto pid = id();
  out.add_col_sol(cname, GenerationName, pid, generation_cols);
  out.add_col_cost(cname, GenerationName, pid, generation_cols);
  out.add_row_dual(cname, CapacityName, pid, capacity_rows);

  // Heat-rate piecewise slack primals + reduced costs.  One emission
  // per segment; downstream tools can reconstruct per-segment
  // dispatch from these.
  for (const auto& scols : heat_rate_slack_cols_) {
    out.add_col_sol(cname, HeatRateSlackName, pid, scols);
    out.add_col_cost(cname, HeatRateSlackName, pid, scols);
  }

  // ── PLEXOS-aligned per-MWh dispatch cost stack ($/MWh) ─────────────
  //
  //   Generator/vom_cost_sol.parquet  — `gcost(stage, block)`,
  //                                    matches PLEXOS `VOM Cost`.
  //   Generator/fuel_cost_sol.parquet — `heat_rate · fuel.price`,
  //                                    matches PLEXOS `Fuel Cost`.
  //   Generator/srmc_sol.parquet      — VOM + Fuel = primary-segment
  //                                    cost coefficient on the
  //                                    `generation` column, matches
  //                                    PLEXOS `SRMC` (Short-Run
  //                                    Marginal Cost).
  //
  // All values are physical $/MWh (Path A — computed from source
  // schedules at `add_to_lp` time; no LP-scale un-wrapping needed).
  // Emission cost is wired separately via EmissionZone.price on the
  // `EmissionZone/production` column; reconstruct the full carbon-
  // inclusive SRMC downstream by summing this `srmc_sol` with the
  // per-block emission-zone marginal price scaled by the generator's
  // weighted emission rate.
  out.add_col_sol_values(cname, "vom_cost", pid, vom_cost_values_);
  out.add_col_sol_values(cname, "fuel_cost", pid, fuel_cost_values_);
  out.add_col_sol_values(cname, "srmc", pid, srmc_values_);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
