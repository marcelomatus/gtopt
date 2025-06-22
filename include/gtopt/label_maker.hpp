/**
 * @file      label_maker.hpp
 * @brief     Header of
 * @date      Sun Jun 22 15:59:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

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
  explicit constexpr LabelMaker(const OptionsLP& options) noexcept
      : m_options_(options)
  {
  }

  template<typename... Types>
  [[nodiscard]] constexpr auto lp_label(Types&&... args) const noexcept
      -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...);
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(StageLP&& stage,
                                        Types&&... args) const noexcept
      -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<StageLP>(stage).uid());
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(ScenarioLP&& scenario,
                                        StageLP&& stage,
                                        Types&&... args) const noexcept
      -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<ScenarioLP>(scenario).uid(),
                           std::forward<StageLP>(stage).uid());
  }

  template<typename ScenarioLP,
           typename StageLP,
           typename BlockLP,
           typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, StageLP>
      && std::same_as<std::remove_cvref_t<BlockLP>, BlockLP>
      && (sizeof...(Types) == 3)
  [[nodiscard]] constexpr auto lp_label(ScenarioLP&& scenario,
                                        StageLP&& stage,
                                        BlockLP&& block,
                                        Types&&... args) const noexcept
      -> std::string
  {
    if (!m_options_.get().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<ScenarioLP>(scenario).uid(),
                           std::forward<StageLP>(stage).uid(),
                           std::forward<BlockLP>(block).uid());
  }

private:
  std::reference_wrapper<OptionsLP> m_options_;
};

}  // namespace gtopt
