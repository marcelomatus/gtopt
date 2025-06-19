
#pragma once

#include <gtopt/as_label.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class LabelMaker
{
public:
  explicit LabelMaker(const OptionsLP& options,
                      const std::vector<ScenarioLP>& scenarios,
                      const std::vector<StageLP>& stages)
      : m_options_(options)
      , m_scenarios_(scenarios)
      , m_stages_(stages)
  {
  }

  template<typename... Types>
    requires(std::constructible_from<std::string, Types> && ...)
  constexpr auto label(const Types&... var) const noexcept -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var...);
  }

  template<typename... Types>
    requires(sizeof...(Types) >= 3)
  constexpr auto t_label(const StageLP& stage, const Types&... var) const
      -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var..., stage.uid());
  }

  template<typename... Types>
    requires(sizeof...(Types) == 3)
  constexpr auto st_label(const ScenarioLP& scenario,
                          const StageLP& stage,
                          const Types&... var) const -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var..., scenario.uid(), stage.uid());
  }

  template<typename... Types>
    requires(sizeof...(Types) == 3)
  constexpr auto stb_label(const ScenarioLP& scenario,
                           const StageLP& stage,
                           const BlockLP& block,
                           const Types&... var) const noexcept -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var..., scenario.uid(), stage.uid(), block.uid());
  }

private:
  std::reference_wrapper<const OptionsLP> m_options_;
  std::reference_wrapper<const std::vector<ScenarioLP>> m_scenarios_;
  std::reference_wrapper<const std::vector<StageLP>> m_stages_;
};

}  // namespace gtopt
