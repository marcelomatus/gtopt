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

  const auto rate = param_rate(stage.uid()).value_or(0.0);
  if (rate == 0.0) {
    return true;  // zero-rate source contributes nothing to the balance row
  }

  auto&& zone = sc.element<EmissionZoneLP>(EmissionZoneLPSId {src.zone});
  if (!zone.is_active(stage)) {
    return true;
  }

  auto&& gen = sc.element<GeneratorLP>(GeneratorLPSId {*src.generator});
  if (!gen.is_active(stage)) {
    return true;
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  // Find the zone's balance rows for this (scenario, stage).
  const auto& brows_map = zone.balance_rows();
  const auto brows_it = brows_map.find(st_key);
  if (brows_it == brows_map.end()) {
    return true;
  }
  const auto& balance_rows = brows_it->second;

  // Find the generator's dispatch columns for this (scenario, stage).
  const auto& gen_cols = gen.generation_cols_at(scenario, stage);

  // Per block, inject -rate · duration onto the generator's dispatch
  // column in the balance row.
  for (const auto& block : stage.blocks()) {
    const auto buid = block.uid();
    const auto brow_it = balance_rows.find(buid);
    if (brow_it == balance_rows.end()) {
      continue;
    }
    const auto gcol_it = gen_cols.find(buid);
    if (gcol_it == gen_cols.end()) {
      continue;  // generator not active in this block
    }
    lp.set_coeff(brow_it->second, gcol_it->second, -rate * block.duration());
  }

  return true;
}

}  // namespace gtopt
