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
  constexpr static ::std::string_view ClassName = "GeneratorProfile";

  explicit GeneratorProfileLP(GeneratorProfile&& pgenerator_profile,
                             InputContext& ic);

  [[nodiscard]] constexpr auto&& generator_profile(this auto&& self) noexcept
  {
    return ::std::forward_like<decltype(self)>(self.object());
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

private:
  OptTRealSched scost;
  STBRealSched profile;

  STBIndexHolder<ColIndex> spillover_cols;
  STBIndexHolder<RowIndex> spillover_rows;
};

}  // namespace gtopt
