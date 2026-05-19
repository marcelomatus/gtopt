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

#include <gtopt/emission_source_lp.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

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

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  const auto& brows_map = zone.balance_rows();
  const auto brows_it = brows_map.find(st_key);
  if (brows_it == brows_map.end()) {
    return true;
  }
  const auto& balance_rows = brows_it->second;

  const auto& gen_cols = gen.generation_cols_at(scenario, stage);

  // Per block: balance_b ← balance_b − weight · (rate + upstream) · dur · gen
  // (LP convention: the balance row pins
  //   production_b − Σ contributions = 0
  //  so contributions enter with a negative coefficient.)
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
    const auto coeff = -weight * effective_rate * block.duration();
    // Multiple sources may target the same (zone, generator) pair
    // (e.g. two pollutants in a basket, or a fuel-derived plus a
    // direct rate).  Read-modify-write to accumulate contributions
    // rather than overwriting.
    const auto existing = lp.get_coeff(brow_it->second, gcol_it->second);
    lp.set_coeff(brow_it->second, gcol_it->second, existing + coeff);
  }

  return true;
}

}  // namespace gtopt
