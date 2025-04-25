/**
 * @file      generator_profile_lp.hpp
 * @brief     Header of
 * @date      Tue Apr  1 22:01:16 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile.hpp>

namespace gtopt
{

class GeneratorProfileLP : public ObjectLP<GeneratorProfile>
{
public:
  constexpr static std::string_view ClassName = "GeneratorProfile";

  explicit GeneratorProfileLP(InputContext& ic,
                              GeneratorProfile pgenerator_profile);

  [[nodiscard]] constexpr auto&& generator_profile() { return object(); }
  [[nodiscard]] constexpr auto&& generator_profile() const { return object(); }

  [[nodiscard]] auto&& generator() const
  {
    return generator_profile().generator;
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioIndex& scenario_index,
                               const StageIndex& stage_index,
                               LinearProblem& lp);
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched scost;
  STBRealSched profile;

  STBIndexHolder spillover_cols;
  STBIndexHolder spillover_rows;

  ElementIndex<GeneratorLP> generator_index;
};

}  // namespace gtopt
