#pragma once

#include <gtopt/as_label.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/block_lp.hpp>

namespace gtopt
{

class LabelMaker
{
public:
    explicit LabelMaker(const OptionsLP& options) : m_options(options) {}

    template<typename... Types>
        requires(std::constructible_from<std::string, Types> && ...)
    constexpr auto label(const Types&... var) const noexcept -> std::string
    {
        if (!m_options.use_lp_names()) [[likely]] {
            return {};
        }
        return gtopt::as_label(var...);
    }

    template<typename... Types>
        requires(sizeof...(Types) == 3)
    constexpr auto t_label(const StageIndex& stage_index,
                         const Types&... var) const noexcept -> std::string
    {
        if (!m_options.use_lp_names()) [[likely]] {
            return {};
        }
        return gtopt::as_label(var..., stage_uid(stage_index));
    }

    template<typename... Types>
        requires(sizeof...(Types) == 3)
    constexpr auto st_label(const ScenarioIndex& scenario_index,
                          const StageIndex& stage_index,
                          const Types&... var) const noexcept -> std::string
    {
        if (!m_options.use_lp_names()) [[likely]] {
            return {};
        }
        return gtopt::as_label(
            var..., scenario_uid(scenario_index), stage_uid(stage_index));
    }

    template<typename... Types>
        requires(sizeof...(Types) == 3)
    constexpr auto stb_label(const ScenarioLP& scenario,
                           const StageLP& stage,
                           const BlockLP& block,
                           const Types&... var) const -> std::string
    {
        if (!m_options.use_lp_names()) [[likely]] {
            return {};
        }
        return gtopt::as_label(var..., scenario.uid(), stage.uid(), block.uid());
    }

    template<typename... Types>
        requires(sizeof...(Types) == 3)
    constexpr auto stb_label(const ScenarioIndex& scenario_index,
                           const StageIndex& stage_index,
                           const BlockLP& block,
                           const Types&... var) const -> std::string
    {
        return stb_label(
            m_scenarios[scenario_index], m_stages[stage_index], block, var...);
    }

private:
    const OptionsLP& m_options;
    const std::vector<ScenarioLP>& m_scenarios;
    const std::vector<StageLP>& m_stages;
};

} // namespace gtopt
