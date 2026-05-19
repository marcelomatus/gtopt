/**
 * @file      emission_source_lp.cpp
 * @brief     LP-active wiring for EmissionSource (per-source coefficient
 *            injection into its zone's balance row).
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * For each (scenario, stage, block):
 *
 *   balance_{zone, b}:  − rate · dur_b · gen_{source.generator, b}
 *
 * The balance row was created by `EmissionZoneLP::add_to_lp` running
 * earlier in the same visitor pass; here we simply inject the
 * coefficient.  Mirrors `InertiaProvisionLP` injecting its provision
 * factor into `InertiaZoneLP::requirement_rows()`.
 */

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

  // Combustion (TTW) + optional upstream (WTT) rates.  Both are
  // counted toward the zone's balance row; reporting can split them
  // post-solve when needed.
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
      // OptReal is std::optional<double> — the .value_or fallback
      // is 1.0 for single-pollutant / CO₂-equivalent reference.
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
  //
  // If `Generator.emission_captures[]` contains a row for this
  // pollutant, scale the source contribution to the balance by
  // `(1 − capture_rate)` and add a per-MWh objective adder of
  // `capture_rate · effective_rate · capture_cost` on the generator
  // dispatch column.
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

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  const auto& brows_map = zone.balance_rows();
  const auto brows_it = brows_map.find(st_key);
  if (brows_it == brows_map.end()) {
    return true;
  }
  const auto& balance_rows = brows_it->second;

  const auto& gen_cols = gen.generation_cols_at(scenario, stage);

  // Per block:
  //   balance_b ← balance_b − weight · (1 − capture_rate) · (rate + upstream)
  //                            · dur · gen
  //   gen_b col cost  ← + capture_rate · (rate + upstream) · capture_cost · dur
  //                      (scaled via CostHelper::block_ecost — the same
  //                      probability × discount × scale chain every other
  //                      thermal cost uses)
  // Per-block output-reconstruction caches (populated below; consumed
  // by add_to_output to compute the per-source emission streams).
  BIndexHolder<ColIndex> blk_gen_cols;
  BIndexHolder<double> blk_combustion_factor;
  BIndexHolder<double> blk_upstream_factor;
  BIndexHolder<double> blk_captured_factor;
  BIndexHolder<double> blk_rate_value;
  BIndexHolder<double> blk_upstream_rate_value;

  for (const auto& block : stage.blocks()) {
    const auto buid = block.uid();
    const auto brow_it = balance_rows.find(buid);
    if (brow_it == balance_rows.end()) {
      continue;
    }
    const auto gcol_it = gen_cols.find(buid);
    if (gcol_it == gen_cols.end()) {
      continue;
    }
    const auto dur = block.duration();
    const auto coeff = -weight * net_factor * effective_rate * dur;
    // Multiple sources may target the same (zone, generator) pair
    // (e.g. two pollutants in a basket, or a fuel-derived plus a
    // direct rate).  Read-modify-write to accumulate contributions
    // rather than overwriting.
    const auto existing = lp.get_coeff(brow_it->second, gcol_it->second);
    lp.set_coeff(brow_it->second, gcol_it->second, existing + coeff);

    // CCS opex: charge capture_cost per ton captured.  The captured
    // tonnage per MWh is `capture_rate · (rate + upstream)`, and we
    // multiply by `capture_cost` to get $/MWh, then through
    // `block_ecost` to put it on the same probability × discount ×
    // scale chain the rest of the dispatch cost stack uses.
    if (capture_rate > 0.0 && capture_cost > 0.0) {
      const auto adder = CostHelper::block_ecost(
          scenario, stage, block, capture_rate * effective_rate * capture_cost);
      lp.col_at(gcol_it->second).cost += adder;
    }

    // Stash factors for `add_to_output` per-source streams.
    blk_gen_cols[buid] = gcol_it->second;
    blk_combustion_factor[buid] = net_factor * rate * dur;
    blk_upstream_factor[buid] = net_factor * upstream * dur;
    blk_captured_factor[buid] = capture_rate * effective_rate * dur;
    blk_rate_value[buid] = rate;
    if (upstream != 0.0) {
      blk_upstream_rate_value[buid] = upstream;
    }
  }

  if (!blk_gen_cols.empty()) {
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
