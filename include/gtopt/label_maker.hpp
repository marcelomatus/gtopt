/**
 * @file      label_maker.hpp
 * @brief     Defines the LabelMaker class for creating LP variable labels.
 * @date      Sun Jun 22 15:59:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The LabelMaker class provides functionality to generate labels for linear
 * programming (LP) variables based on various context objects (like stages,
 * scenarios, blocks) and options. It conditionally creates labels only when the
 * use_lp_names option is enabled.
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
      : m_dont_use_lp_names_(!options.use_lp_names())
  {
  }

  [[nodiscard]]
  constexpr bool dont_use_lp_names() const noexcept
  {
    return m_dont_use_lp_names_;
  }

  template<typename... Types>
  [[nodiscard]] auto lp_label(Types&&... args) const -> std::string
  {
    if (dont_use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...);
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(StageLP&& stage, Types&&... args) const
      -> std::string
  {
    if (dont_use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<StageLP>(stage).uid());
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(ScenarioLP&& scenario,
                                        StageLP&& stage,
                                        Types&&... args) const -> std::string
  {
    if (dont_use_lp_names()) [[likely]] {
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
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && std::same_as<std::remove_cvref_t<BlockLP>, gtopt::BlockLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(ScenarioLP&& scenario,
                                        StageLP&& stage,
                                        BlockLP&& block,
                                        Types&&... args) const -> std::string
  {
    if (dont_use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<ScenarioLP>(scenario).uid(),
                           std::forward<StageLP>(stage).uid(),
                           std::forward<BlockLP>(block).uid());
  }

private:
  bool m_dont_use_lp_names_;
};

}  // namespace gtopt
