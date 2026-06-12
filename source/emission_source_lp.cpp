/**
 * @file      emission_source_lp.cpp
 * @brief     Substitute-out wiring for `EmissionSource` — every per-MWh
 *            emission contribution is stamped directly onto the
 *            generator dispatch column.
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * For each (scenario, stage, block), the per-source contribution to
 * its zone is the scalar
 *
 *   α_{s,b}  =  weight_z,p · (1 − capture_s,p) · (rate_s + upstream_s) · dur_b
 *              [tCO₂-eq per MW of dispatch]
 *
 * which is wired in three places, each only when active:
 *
 *   * cap row (if `EmissionZone.cap` set):
 *         cap_row[gen_col] += α_{s,b}
 *   * carbon tax (if `EmissionZone.price` set OR
 *     `objective_mode = "emissions"`):
 *         gen_col.cost += block_ecost(price · α_rate)
 *     (where `α_rate = α / dur` is the dur-free $/MWh form; block_ecost
 *     adds the per-block duration · probability · discount factors.)
 *   * allowance-pool drawdown (if `EmissionZone.allowance_pool` set):
 *         pool.energy_row_{b}[gen_col] += α_{s,b}
 *   * CCS opex (always, when `Generator.emission_captures[]` carries
 *     a row for this pollutant with non-zero `cost`):
 *         gen_col.cost += block_ecost(capture · effective_rate · capture_cost)
 *
 * Mirrors the existing CCS pattern (see the `lp.col_at(gcol).cost += ...`
 * adder below).  When the zone is *pure reporting* (no cap, no price,
 * no pool, not emissions-objective), nothing is wired into the LP and
 * only the per-block factor cache is populated — the cache feeds the
 * per-source output streams in `add_to_output`.
 */

#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/emission_source_lp.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
/// Resolve an `OptTRealFieldSched` to a scalar.  Handles the scalar
/// and per-stage-vector cases; falls back to 0 for FileSched (which
/// requires the InputContext machinery — not threaded through here
/// yet for `Generator.emission_captures[].rate/cost`).
[[nodiscard]] double resolve_stage_scalar(const OptTRealFieldSched& field,
                                          std::size_t stage_index) noexcept
{
  if (!field.has_value()) {
    return 0.0;
  }
  const auto& val = *field;
  if (std::holds_alternative<Real>(val)) {
    return std::get<Real>(val);
  }
  if (std::holds_alternative<std::vector<Real>>(val)) {
    const auto& vec = std::get<std::vector<Real>>(val);
    if (stage_index < vec.size()) {
      return vec[stage_index];
    }
  }
  // FileSched (string) path requires InputContext + Schedule and is
  // not yet wired for inline emission_captures.  TODO.
  return 0.0;
}
}  // namespace

EmissionSourceLP::EmissionSourceLP(const EmissionSource& src,
                                   const InputContext& ic)
    : Base(src, ic, Element::class_name)
    , rate_(ic, Element::class_name, id(), std::move(object().rate))
    , upstream_rate_(
          ic, Element::class_name, id(), std::move(object().upstream_rate))
{
}

bool EmissionSourceLP::add_to_lp(const SystemContext& sc,
                                 const ScenarioLP& scenario,
                                 const StageLP& stage,
                                 LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto& src = emission_source();
  if (!src.generator.has_value()) {
    SPDLOG_WARN("EmissionSource uid={} has no generator FK — skipping", uid());
    return false;
  }

  // Combustion (TTW) + optional upstream (WTT) rates [t/MWh].  Both
  // are counted toward the zone's α; reporting can split them post-
  // solve when needed.
  const auto rate = param_rate(stage.uid()).value_or(0.0);
  const auto upstream = param_upstream_rate(stage.uid()).value_or(0.0);
  const auto effective_rate = rate + upstream;
  if (effective_rate == 0.0) {
    return true;
  }

  auto&& zone = sc.element<EmissionZoneLP>(EmissionZoneLPSId {src.zone});
  if (!zone.is_active(stage)) {
    return true;
  }

  // Look up the GWP weight for THIS pollutant in the zone's
  // `emissions[]` table.  Sources whose pollutant isn't in the
  // zone's list contribute nothing (programming error or
  // out-of-scope pollutant — log and skip).  Weight defaults to 1.0
  // when the zone-factor row has no explicit `weight` (single-
  // pollutant zone, or CO₂-only basket).
  const auto& zone_emissions = zone.emission_zone().emissions;
  const auto& src_emission = src.emission;
  double weight = 0.0;
  bool emission_in_zone = false;
  for (const auto& zef : zone_emissions) {
    if (zef.emission == src_emission) {
      weight = zef.weight.value_or(1.0);
      emission_in_zone = true;
      break;
    }
  }
  if (!emission_in_zone) {
    SPDLOG_WARN(
        "EmissionSource uid={} targets zone uid={} but the zone does not "
        "list pollutant — skipping",
        uid(),
        zone.uid());
    return true;
  }

  auto&& gen = sc.element<GeneratorLP>(GeneratorLPSId {*src.generator});
  if (!gen.is_active(stage)) {
    return true;
  }

  // ── CCS / abatement: per-(generator, pollutant) capture row ────────
  // If `Generator.emission_captures[]` contains a row for this
  // pollutant, scale the contribution by `(1 − capture_rate)` and add
  // a per-MWh objective adder of `capture_rate · effective_rate ·
  // capture_cost` on the generator dispatch column.
  double capture_rate = 0.0;
  double capture_cost = 0.0;
  const auto stage_index = static_cast<std::size_t>(stage.index());
  for (const auto& cap : gen.generator().emission_captures) {
    if (cap.emission == src.emission) {
      capture_rate = resolve_stage_scalar(cap.rate, stage_index);
      capture_cost = resolve_stage_scalar(cap.cost, stage_index);
      break;
    }
  }
  const double net_factor = 1.0 - capture_rate;
  // Per-MWh α rate [tCO₂-eq / MWh]: dur-free, lifted to a per-block
  // tonnage in the per-block loop below.
  const double alpha_rate = weight * net_factor * effective_rate;

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  // Tolerant lookup: when every block of this (scenario, stage) was
  // skipped by the P1 zero-pmax optimization, ``generation_cols`` has
  // no outer key.  Use ``lookup_generation_cols`` (returns an empty
  // inner map) instead of the throwing ``generation_cols_at`` — the
  // per-block loop below then has nothing to do.  Symmetric to
  // ``WaterwayLP::flow_cols_at`` / ``lookup_flow_cols``.
  const auto& gen_cols = gen.lookup_generation_cols(scenario, stage);
  if (gen_cols.empty()) {
    return true;
  }

  // Wiring decisions for this (scenario, stage):
  //   * cap_row exists       ⇒ stamp `+α` per block onto gen_col
  //   * effective_price > 0  ⇒ add `block_ecost(price · α_rate)` to
  //   gen_col.cost
  //   * pool_fk set          ⇒ stamp `+α` per block onto pool.energy_row
  const auto& cap_rows_map = zone.cap_rows();
  const auto cap_it = cap_rows_map.find(st_key);
  const RowIndex* cap_row_idx =
      (cap_it != cap_rows_map.end()) ? &cap_it->second : nullptr;

  const bool emissions_mode = sc.options().is_emissions_objective();
  const auto stage_price = zone.param_price(stage.uid());
  double effective_price = 0.0;
  if (stage_price) {
    effective_price = *stage_price;
  } else if (emissions_mode) {
    effective_price = 1.0;
  }
  const bool has_tax = (effective_price != 0.0);

  const auto pool_fk = zone.emission_zone().allowance_pool;
  const STBIndexHolder<RowIndex>::mapped_type* pool_energy_rows = nullptr;
  if (pool_fk.has_value()) {
    auto&& pool = sc.element<AllowancePoolLP>(AllowancePoolLPSId {*pool_fk});
    pool_energy_rows = &pool.energy_rows_at(scenario, stage);
  }

  // Per-block output-reconstruction caches (populated below;
  // consumed by add_to_output to compute the per-source emission
  // streams).  Populated regardless of whether any LP wiring fires,
  // so reporting-only zones still emit the full per-source streams.
  BIndexHolder<ColIndex> blk_gen_cols;
  BIndexHolder<double> blk_combustion_factor;
  BIndexHolder<double> blk_upstream_factor;
  BIndexHolder<double> blk_captured_factor;
  BIndexHolder<double> blk_rate_value;
  BIndexHolder<double> blk_upstream_rate_value;

  for (const auto& block : stage.blocks()) {
    const auto buid = block.uid();
    const auto gcol_it = gen_cols.find(buid);
    if (gcol_it == gen_cols.end()) {
      continue;
    }
    const auto dur = block.duration();
    const auto gcol = gcol_it->second;
    // α [tCO₂-eq / MW] for this block — convert α_rate ($/MWh basis)
    // to per-block tonnage by multiplying by duration.  Used by both
    // the cap row coefficient and the pool drawdown.
    const double alpha_block = alpha_rate * dur;

    // ── Cap row coefficient (only when zone has a cap row at (s,t)) ──
    // Multiple sources may target the same (zone, generator) pair
    // (e.g. two pollutants in a basket).  Read-modify-write to
    // accumulate contributions rather than overwriting.
    if (cap_row_idx != nullptr) {
      const auto existing = lp.get_coeff(*cap_row_idx, gcol);
      lp.set_coeff(*cap_row_idx, gcol, existing + alpha_block);
    }

    // ── Carbon tax (price or emissions_mode) ────────────────────────
    // Per-MWh adder = `effective_price · α_rate`; block_ecost adds the
    // per-block dur · probability · discount factors.  Mirrors the CCS
    // opex adder pattern already in this file (below).
    if (has_tax) {
      const auto tax_adder = CostHelper::block_ecost(
          scenario, stage, block, effective_price * alpha_rate);
      lp.col_at(gcol).cost += tax_adder;
    }

    // ── Allowance-pool drawdown ─────────────────────────────────────
    if (pool_energy_rows != nullptr) {
      const auto it = pool_energy_rows->find(buid);
      if (it != pool_energy_rows->end()) {
        const auto existing = lp.get_coeff(it->second, gcol);
        lp.set_coeff(it->second, gcol, existing + alpha_block);
      }
    }

    // ── CCS opex (per-block) ────────────────────────────────────────
    // Charge `capture_cost` per ton captured.  The captured tonnage
    // per MWh is `capture_rate · effective_rate`; multiplied by
    // `capture_cost` gives $/MWh, then through block_ecost the same
    // probability · discount · scale chain every other thermal cost
    // uses.
    if (capture_rate > 0.0 && capture_cost > 0.0) {
      const auto adder = CostHelper::block_ecost(
          scenario, stage, block, capture_rate * effective_rate * capture_cost);
      lp.col_at(gcol).cost += adder;
    }

    // ── Reporting cache (always populated) ──────────────────────────
    blk_gen_cols[buid] = gcol;
    blk_combustion_factor[buid] = net_factor * rate * dur;
    blk_upstream_factor[buid] = net_factor * upstream * dur;
    blk_captured_factor[buid] = capture_rate * effective_rate * dur;
    blk_rate_value[buid] = rate;
    if (upstream != 0.0) {
      blk_upstream_rate_value[buid] = upstream;
    }
  }

  if (!blk_gen_cols.empty()) {
    // ── PAMPL `emission_source("X").emissions` registration ────────
    // Expose the per-block NET (post-capture) emission expression
    // `(combustion + upstream)_b · gen_col_b` to user constraints via
    // the AMPL `block_cols_weighted_sum` registry — one leg per
    // (gen_col, factor_b) per block.  Mirror of the new `fuel.offtake`
    // accessor: a weighted sum of an existing LP column, no aggregator
    // variable.  UCs can express "emissions of source X over the
    // stage" as `sum(b in blocks) emission_source("X").emissions` and
    // it expands directly onto the generator dispatch col.
    BIndexHolder<std::vector<std::pair<ColIndex, double>>> blk_weighted_emit;
    for (const auto& [buid, gcol] : blk_gen_cols) {
      const double net_factor_b =
          blk_combustion_factor.at(buid) + blk_upstream_factor.at(buid);
      blk_weighted_emit[buid] = {{gcol, net_factor_b}};
    }
    static constexpr auto ampl_name = Element::class_name.snake_case();
    sc.add_ampl_variable(
        ampl_name, uid(), "emissions", scenario, stage, blk_weighted_emit);

    gen_cols_[st_key] = std::move(blk_gen_cols);
    combustion_factor_[st_key] = std::move(blk_combustion_factor);
    upstream_factor_[st_key] = std::move(blk_upstream_factor);
    captured_factor_[st_key] = std::move(blk_captured_factor);
    rate_values_[st_key] = std::move(blk_rate_value);
    if (!blk_upstream_rate_value.empty()) {
      upstream_rate_values_[st_key] = std::move(blk_upstream_rate_value);
    }
  }

  return true;
}

bool EmissionSourceLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // Reconstruct each per-source stream from
  //   value_{s,t,b} = primal(gen_col_{s,t,b}) · factor_{s,t,b}
  // and emit via the value-emit OutputContext path (no LP-scale
  // unwrap — values are physical tons per block).
  STBIndexHolder<double> emissions_values;
  STBIndexHolder<double> combustion_values;
  STBIndexHolder<double> upstream_values;
  STBIndexHolder<double> captured_values;

  bool any_upstream = false;
  bool any_captured = false;

  for (const auto& [st_key, blk_cols] : gen_cols_) {
    auto& emissions_st = emissions_values[st_key];
    auto& combustion_st = combustion_values[st_key];
    auto& upstream_st = upstream_values[st_key];
    auto& captured_st = captured_values[st_key];

    const auto& combustion_st_in = combustion_factor_.at(st_key);
    const auto& upstream_st_in = upstream_factor_.at(st_key);
    const auto& captured_st_in = captured_factor_.at(st_key);

    for (const auto& [buid, gcol] : blk_cols) {
      const auto gen_sol = out.primal(gcol);
      const auto comb = combustion_st_in.at(buid) * gen_sol;
      const auto upstr = upstream_st_in.at(buid) * gen_sol;
      const auto capt = captured_st_in.at(buid) * gen_sol;

      combustion_st[buid] = comb;
      upstream_st[buid] = upstr;
      captured_st[buid] = capt;
      emissions_st[buid] = comb + upstr;

      if (upstream_st_in.at(buid) != 0.0) {
        any_upstream = true;
      }
      if (captured_st_in.at(buid) != 0.0) {
        any_captured = true;
      }
    }
  }

  out.add_col_sol_values(cname, "emissions", pid, emissions_values);
  out.add_col_sol_values(cname, "combustion", pid, combustion_values);
  if (any_upstream) {
    out.add_col_sol_values(cname, "upstream", pid, upstream_values);
  }
  if (any_captured) {
    out.add_col_sol_values(cname, "captured", pid, captured_values);
  }

  // Per-MWh rate streams (raw input coefficient broadcast across blocks).
  if (!rate_values_.empty()) {
    out.add_col_sol_values(cname, "rate", pid, rate_values_);
  }
  if (!upstream_rate_values_.empty()) {
    out.add_col_sol_values(cname, "upstream_rate", pid, upstream_rate_values_);
  }

  return true;
}

}  // namespace gtopt
