/**
 * @file      generator_profile_lp.cpp
 * @brief     Implementation of generator profile LP operations
 * @date      Tue Apr  1 22:03:50 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the core LP operations for generator profiles including:
 * - Construction from input context
 * - Adding profile constraints to LP problem
 * - Managing spillover variables and constraints
 * - Output solution processing
 */

#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/generator_profile_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
GeneratorProfileLP::GeneratorProfileLP(
    const GeneratorProfile& pgenerator_profile, InputContext& ic)
    : ProfileObjectLP(pgenerator_profile, ic, ClassName)
{
}

bool GeneratorProfileLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  auto&& generator = sc.element<GeneratorLP>(generator_sid());
  if (!generator.is_active(stage)) {
    return true;
  }

  auto&& generation_cols = generator.generation_cols_at(scenario, stage);

  const auto [stage_capacity, capacity_col] =
      generator.capacity_and_col(stage, lp);

  if (!capacity_col && !generator.generator().capacity) {
    SPDLOG_WARN(
        "GeneratorProfile requires that Generator defines capacity or "
        "expansion");
    return false;
  }

  return add_profile_to_lp(ClassName.short_name(),
                           sc,
                           scenario,
                           stage,
                           lp,
                           "spo",
                           generation_cols,
                           capacity_col,
                           stage_capacity);
}

bool GeneratorProfileLP::add_to_output(OutputContext& out) const
{
  return add_profile_to_output(ClassName.full_name(), out, "spillover");
}

bool GeneratorProfileLP::update_aperture_lp(LinearInterface& li,
                                            const ScenarioLP& base_scenario,
                                            const ScenarioLP& aperture_scenario,
                                            const StageLP& stage) const
{
  return ProfileObjectLP::update_aperture_lp(
      li, base_scenario, aperture_scenario, stage);
}

bool GeneratorProfileLP::update_aperture_from_cache(
    LinearInterface& li,
    const ScenarioLP& base_scenario,
    ScenarioUid aperture_scenario_uid,
    const ApertureDataCache& cache,
    const StageLP& stage) const
{
  return ProfileObjectLP::update_aperture_from_cache(ClassName.full_name(),
                                                     li,
                                                     base_scenario,
                                                     aperture_scenario_uid,
                                                     cache,
                                                     stage);
}

}  // namespace gtopt
