/**
 * @file      generator_profile_lp.hpp
 * @brief     Linear programming representation of generator profiles
 * @date      Tue Apr  1 22:01:16 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the GeneratorProfileLP class which:
 * - Wraps GeneratorProfile for LP problem formulation
 * - Manages spillover variables and constraints
 * - Handles integration with the optimization problem
 * - Provides output generation capabilities
 */

#pragma once

#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/profile_object_lp.hpp>

namespace gtopt
{

/**
 * @brief Linear programming representation of a generator profile
 *
 * Handles the LP formulation of generator profile constraints including:
 * - Spillover variable management
 * - Profile constraint generation
 * - Solution output processing
 */
class GeneratorProfileLP : public ProfileObjectLP<GeneratorProfile, GeneratorLP>
{
public:
  /// Class name constant used for labeling LP elements
  static constexpr LPClassName ClassName {"GeneratorProfile", "gpr"};

  explicit GeneratorProfileLP(const GeneratorProfile& pgenerator_profile,
                              InputContext& ic);

  [[nodiscard]] constexpr auto&& generator_profile(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.object());
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {generator_profile().generator};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;
};

}  // namespace gtopt
