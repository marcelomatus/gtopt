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

  template<class StageLPs = std::vector<StageLP> >
  explicit ScenarioLP(Scenario scenario, const StageLPs& all_stages = {})
      : m_scenario_(std::move(scenario))
      , m_stages_(all_stages)
  {
  }

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_scenario_.active.value_or(true);
  }
  [[nodiscard]] constexpr auto uid() const
  {
    return ScenarioUid(m_scenario_.uid);
  }
  [[nodiscard]] constexpr auto probability_factor() const
  {
    return m_scenario_.probability_factor.value_or(1.0);
  }

  [[nodiscard]] constexpr auto&& stage(const StageIndex index) const
  {
    return m_stages_[index];
  }
  [[nodiscard]] constexpr auto&& stages() const { return m_stages_; }

private:
  Scenario m_scenario_;
  StageSpan m_stages_;
};

}  // namespace gtopt
