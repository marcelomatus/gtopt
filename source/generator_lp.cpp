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

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

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
    , pmin_fcost(ic, Element::class_name, id(), std::move(object().pmin_fcost))
    , heat_rate(ic, Element::class_name, id(), std::move(object().heat_rate))
    , emission_rate(
          ic, Element::class_name, id(), std::move(object().emission_rate))
    , fuel_per_block_(
          ic, Element::class_name, id(), std::move(object().fuel_per_block))
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
  //
  // Per-(stage, block) override: when ``Generator.fuel_per_block`` is
  // set (Issue #510 Phase 1), the per-block resolved Fuel supersedes
  // the static ``Generator.fuel`` for that cell — its stage price
  // drives the cost coefficient.  Cells whose resolved uid is the
  // sentinel ``0`` (or absent from ``fuel_array``) fall back to the
  // static fuel.  When ``fuel_per_block`` is unset, the per-block
  // cache stays empty and the cost path is byte-for-byte legacy.
  const FuelLP* static_fuel_lp = nullptr;
  if (const auto& fuel_ref = generator().fuel; fuel_ref.has_value()) {
    static_fuel_lp = &sc.element<FuelLP>(FuelLPSId {fuel_ref.value()});
  }
  const Uid static_fuel_uid =
      (static_fuel_lp != nullptr) ? static_fuel_lp->uid() : Uid {0};

  // Per-block fuel override cache.  Empty when ``fuel_per_block`` is
  // unset; otherwise holds (uid, FuelLP*) for every distinct uid that
  // appears in the schedule at this stage.  Sized for small N
  // (typically 2-3 fuels per generator).  The fuel PRICE is now
  // per-(stage, block) (Fuel.price upgraded to OptTBRealFieldSched), so
  // it is resolved per block in ``resolve_block_fuel`` rather than
  // cached once per stage.
  struct PerBlockFuelEntry
  {
    Uid uid;
    const FuelLP* fuel_lp;
  };
  std::vector<PerBlockFuelEntry> per_block_fuels;
  if (has_fuel_per_block()) {
    std::set<Uid> seen_uids;
    for (auto&& blk : stage.blocks()) {
      const auto opt = param_fuel_per_block(stage.uid(), blk.uid());
      if (!opt.has_value() || opt.value() == Uid {0}) {
        continue;
      }
      if (!seen_uids.insert(opt.value()).second) {
        continue;
      }
      const auto* flp = &sc.element<FuelLP>(FuelLPSId {SingleId {opt.value()}});
      per_block_fuels.push_back({
          .uid = opt.value(),
          .fuel_lp = flp,
      });
    }
  }

  // Resolves the effective (FuelLP*, per-block price) at a given block.
  // Falls back to the static fuel when no per-block override is set OR
  // the override cell is the sentinel uid 0.  The price is read
  // per-(stage, block); a scalar / per-stage `Fuel.price` broadcasts to
  // every block, so the legacy single-value behaviour is preserved.
  // Hot-path: when ``per_block_fuels`` is empty the lambda
  // short-circuits to the static fuel.
  const auto resolve_block_fuel =
      [&](BlockUid buid) -> std::pair<const FuelLP*, double>
  {
    const FuelLP* flp = static_fuel_lp;
    if (!per_block_fuels.empty()) {
      if (const auto opt = param_fuel_per_block(stage.uid(), buid);
          opt.has_value() && opt.value() != Uid {0})
      {
        const auto it = std::ranges::find_if(per_block_fuels,
                                             [u = opt.value()](const auto& f)
                                             { return f.uid == u; });
        if (it != per_block_fuels.end()) {
          flp = it->fuel_lp;
        }
      }
    }
    const double price = (flp != nullptr)
        ? flp->param_price(stage.uid(), buid).value_or(0.0)
        : 0.0;
    return {flp, price};
  };

  const auto& hr_segs = generator().heat_rate_segments;
  const auto& pmax_segs_arr = generator().pmax_segments;
  const bool has_pw = has_heat_rate_segments();

  // Per-MWh cost of segment k (cheapest first, k = 0), evaluated at a
  // given (stage, block) gcost / heat_rate / fuel price.  All three
  // coefficients (`gcost`, `heat_rate`, `lossfactor`) are
  // per-(stage, block); fuel price varies per block when
  // ``fuel_per_block`` is set, else per-stage.
  const auto slope_cost_per_mwh =
      [](double price, double hr_slope, double block_gcost)
  { return (price * hr_slope) + block_gcost; };

  // Effective slope for the primary `generation` column at a given
  // block.  When piecewise: slope of the cheapest segment; otherwise
  // scalar heat rate (or 0 when no fuel/heat_rate is configured).
  //
  // Emissions-mode override (issue #519): when
  // ``model_options.objective_mode = "emissions"``, the LP minimizes
  // Σ (emission_rate × generation) instead of dispatch cost.  Zero
  // out the dispatch-cost slope here; the per-MWh emission cost is
  // injected via the existing ``EmissionSource``/``EmissionZone``
  // coupling (emission_source adds ``-rate × weight`` to the zone
  // balance row, and EmissionZone production carries the cost-per-
  // tCO2eq, set to 1.0 in emissions mode — see
  // ``EmissionZoneLP::add_to_lp``).  Net effect: the LP minimizes
  // total CO2eq, and ``Reservoir/water_value_dual`` /
  // ``Battery/energy_dual`` carry the carbon shadow price of stored
  // water / energy directly.
  const bool emissions_mode = sc.options().is_emissions_objective();
  const auto primary_slope_cost_at = [&](const FuelLP* block_fuel_lp,
                                         double block_fuel_price,
                                         double block_gcost,
                                         double block_heat_rate)
  {
    if (emissions_mode) {
      return 0.0;
    }
    if (has_pw) {
      return slope_cost_per_mwh(block_fuel_price, hr_segs.front(), block_gcost);
    }
    if (block_fuel_lp != nullptr) {
      return slope_cost_per_mwh(block_fuel_price, block_heat_rate, block_gcost);
    }
    return block_gcost;
  };

  const auto& balance_rows = bus.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> gcols;
  BIndexHolder<ColIndex> ucols;  // soft-pmin `unserved` slack columns
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
    // This is the SOURCE elimination: dropping the generation column
    // here cascades into the robust consumers (commitment, reserve,
    // inertia) creating no u/v/w/provision for the unit.  Gated by
    // `--no-lp-reduction` for un-reduced diagnostic LPs.
    if (sc.options().lp_reduction() && block_pmax == 0.0 && block_pmin == 0.0)
        [[unlikely]]
    {
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

    const auto [block_fuel_lp, block_fuel_price] = resolve_block_fuel(buid);
    const auto block_primary_slope_cost = primary_slope_cost_at(
        block_fuel_lp, block_fuel_price, block_gcost, block_heat_rate);

    // ── Per-block resolved fuel uid (for `Generator/fuel_sol.parquet`) ──
    // Only stashed when ``fuel_per_block`` is set — without it, the
    // static ``Generator.fuel`` already carries the constant uid in
    // the JSON (no per-block parquet needed).  Records the sentinel
    // 0 ONLY when the per-block cell is unspecified AND the static
    // fuel is unset — that combination should be rare since it means
    // the gen has fuel_per_block declared but neither a static
    // fallback nor a per-block override at this cell.
    if (has_fuel_per_block()) {
      const auto opt_uid = param_fuel_per_block(stage.uid(), buid);
      const Uid resolved_uid = opt_uid.value_or(Uid {0});
      const Uid effective_uid =
          (resolved_uid != Uid {0}) ? resolved_uid : static_fuel_uid;
      fuel_uid_values_[st_key][buid] = static_cast<double>(effective_uid);
    }

    // Soft-`pmin`: when `pmin_fcost` is set (> 0) and there is a real
    // floor (`block_pmin > 0`), relax the hard lower bound to 0 and
    // enforce the floor via an `unserved` slack + `pmin_soft` row
    // (added below).  This keeps a forced-dispatch / must-run floor
    // economically honoured while remaining feasible if a transmission
    // / commitment / ramp limit drives the unit below `pmin`.
    const auto block_pmin_fcost = pmin_fcost.optval(stage.uid(), block.uid());
    const bool soft_pmin =
        block_pmin > 0.0 && block_pmin_fcost.value_or(0.0) > 0.0;

    // Create generation variable for this time block.  Cost on the
    // primary column carries the cheapest-segment slope (or scalar
    // heat rate, or plain gcost when no fuel is set).
    const auto gcol = lp.add_col({
        .lowb = soft_pmin ? 0.0 : block_pmin,
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

    // ── Soft-`pmin` floor ──────────────────────────────────────────────
    // `generation + unserved ≥ pmin`, `unserved ≥ 0` priced at
    // `pmin_fcost`.  The slack is capped at `block_pmin` (the largest
    // possible shortfall, since `generation ≥ 0`).  Whenever the unit
    // can physically reach `pmin` the LP drives `unserved` to 0; under
    // congestion it pays `pmin_fcost · (pmin − generation)` instead of
    // going infeasible.
    if (soft_pmin) {
      const auto ucol = lp.add_col({
          .lowb = 0.0,
          .uppb = block_pmin,
          .cost = CostHelper::block_ecost(
              scenario, stage, block, block_pmin_fcost.value_or(0.0)),
          .class_name = Element::class_name.full_name(),
          .variable_name = UnservedName,
          .variable_uid = guid,
          .context = block_ctx,
      });
      auto srow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = PminSoftName,
              .variable_uid = guid,
              .context = block_ctx,
          }
              .greater_equal(block_pmin);
      srow[gcol] = 1.0;
      srow[ucol] = 1.0;
      [[maybe_unused]] const auto srow_idx = lp.add_row(std::move(srow));
      ucols[buid] = ucol;
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
      // Resize the row / breakpoint trackers once per (gen, stage,
      // block) entry — CommitmentLP retro-fits the kink rows with
      // the ``-pmax_segs[k-1] · u`` term so the LP-relax tightens as
      // ``u`` drops (without this, the piecewise stays at full
      // capacity even when ``u`` is fractional — the regression that
      // accompanied the move of piecewise from Commitment to
      // Generator).
      if (heat_rate_kink_rows_.size() < K - 1) {
        heat_rate_kink_rows_.resize(K - 1);
      }
      if (heat_rate_kink_breakpoints_.size() < K - 1) {
        heat_rate_kink_breakpoints_.assign(
            pmax_segs_arr.begin(),
            pmax_segs_arr.begin() + static_cast<std::ptrdiff_t>(K - 1));
      }
      for (std::size_t k = 1; k < K; ++k) {
        const double marginal_slope = hr_segs[k] - hr_segs[k - 1];
        // Piecewise slack uses the per-block resolved fuel price so
        // per-block fuel switching threads through every segment of
        // the heat-rate curve (not just the cheapest / primary segment).
        const double slack_cost_per_mwh = block_fuel_price * marginal_slope;
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
        const auto krow = lp.add_row(std::move(kink_row));

        // Stash the slack col per segment index for output / cap
        // refactoring later.
        heat_rate_slack_cols_[k - 1][st_key][buid] = s_col;
        // Stash the kink row index so CommitmentLP can retro-fit the
        // u-gating coefficient (see commitment_lp.cpp).
        heat_rate_kink_rows_[k - 1][st_key][buid] = krow;
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
  if (!ucols.empty()) {
    unserved_cols[st_key] = std::move(ucols);
    // `unserved` is the soft-pmin shortfall slack — an internal LP column
    // (kept for output and the pmin_soft row) with no user-constraint use
    // case, so it is NOT registered as an AMPL attribute.
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
  // Primary dispatch + marginal-unit signal: kept on the default
  // `solution` / `reduced_cost` gates — every downstream consumer
  // (gtopt_marginal_units, gtopt_check_output, gtopt_compare,
  // gtopt_results_summary) reads at least one of these.
  out.add_col_sol(cname, GenerationName, pid, generation_cols);
  out.add_col_cost(cname, GenerationName, pid, generation_cols);

  // ── Extras (opt-in via `--write-out ...,extras:Generator`) ─────────
  //
  // The capacity-row dual is the shadow price on `gen <= pmax`; useful
  // for capacity-expansion audits but unused by the dispatch /
  // marginal-unit pipelines.  Heat-rate slack primals + reduced costs
  // are per-segment piecewise diagnostics: marginal-unit attribution
  // uses `srmc_sol` (kept under `solution` below), which already
  // encodes the active segment's slope.  No current consumer reads
  // either set.
  out.add_row_dual_extras(cname, CapacityName, pid, capacity_rows);
  for (const auto& scols : heat_rate_slack_cols_) {
    out.add_col_sol_extras(cname, HeatRateSlackName, pid, scols);
    out.add_col_cost_extras(cname, HeatRateSlackName, pid, scols);
  }

  // ── PLEXOS-aligned per-MWh dispatch cost stack ($/MWh) ─────────────
  //
  //   Generator/srmc_sol.parquet      — VOM + Fuel = active-segment
  //                                    cost coefficient on the
  //                                    `generation` column, matches
  //                                    PLEXOS `SRMC` (Short-Run
  //                                    Marginal Cost).  Used by
  //                                    `gtopt_marginal_units` for
  //                                    piecewise / time-varying SRMC
  //                                    attribution.
  //   Generator/vom_cost_sol.parquet  — `gcost(stage, block) · dispatch`
  //                                    decomposition (PLEXOS `VOM Cost`).
  //   Generator/fuel_cost_sol.parquet — `heat_rate · fuel.price · dispatch`
  //                                    decomposition (PLEXOS `Fuel Cost`).
  //
  // VOM and fuel are demoted to `extras`: `srmc_sol` already gives the
  // active-segment marginal cost (`srmc = vom + fuel`), and total
  // operational cost is `srmc · dispatch · duration` — neither
  // decomposition is needed by any current consumer.  Anyone who
  // wants the accounting split opts in via `--write-out
  // ...,extras:Generator`.  SRMC stays on the default `solution` gate.
  out.add_col_sol_values(cname, "srmc", pid, srmc_values_);
  out.add_col_sol_values_extras(cname, "vom_cost", pid, vom_cost_values_);
  out.add_col_sol_values_extras(cname, "fuel_cost", pid, fuel_cost_values_);

  // ── Per-block resolved fuel uid (Issue #510 Phase 1) ──────────────
  // Emitted as `Generator/fuel_sol.parquet` ONLY when
  // `has_fuel_per_block()` is true (i.e. ``fuel_uid_values_`` was
  // populated during ``add_to_lp``).  Skipping the call when the
  // stash is empty satisfies acceptance criterion #4 — for the legacy
  // single-fuel case the static ``Generator.fuel`` uid is already in
  // the input JSON, so a constant per-block parquet would be pure
  // redundancy.
  if (!fuel_uid_values_.empty()) {
    out.add_col_sol_values(cname, "fuel", pid, fuel_uid_values_);
  }

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
