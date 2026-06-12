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
  static constexpr std::string_view SpilloverName {"spillover"};

  explicit GeneratorProfileLP(const GeneratorProfile& pgenerator_profile,
                              InputContext& ic);

  [[nodiscard]] constexpr auto&& generator_profile(this auto&& self) noexcept
  {
    return self.object();
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

  /// Update profile constraints for an aperture scenario.
  [[nodiscard]] bool update_aperture(
      LinearInterface& li,
      const ScenarioLP& base_scenario,
      const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
      const StageLP& stage) const;
};

// Pin the data-struct constant value so an accidental rename of the
// `GeneratorProfile::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"GeneratorProfile"`).
static_assert(GeneratorProfileLP::Element::class_name
                  == LPClassName {"GeneratorProfile"},
              "GeneratorProfile::class_name must remain \"GeneratorProfile\"");

}  // namespace gtopt
