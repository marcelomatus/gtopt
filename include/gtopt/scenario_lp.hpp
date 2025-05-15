#pragma once

#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class ScenarioLP
{
public:
  using StageSpan = std::span<const StageLP>;

  ScenarioLP() = default;

  template<class StageLPs = std::vector<StageLP>>
  explicit ScenarioLP(Scenario pscenario, const StageLPs& pstages = {})
      : scenario(std::move(pscenario))
      , stage_span(pstages)
  {
  }

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return scenario.active.value_or(true);
  }
  [[nodiscard]] constexpr auto uid() const { return ScenarioUid(scenario.uid); }
  [[nodiscard]] constexpr auto probability_factor() const
  {
    return scenario.probability_factor.value_or(1.0);
  }

  [[nodiscard]] constexpr auto&& stage(const StageIndex index) const
  {
    return stage_span[index];
  }
  [[nodiscard]] constexpr auto&& stages() const { return stage_span; }

private:
  Scenario scenario;
  StageSpan stage_span;
};

}  // namespace gtopt
