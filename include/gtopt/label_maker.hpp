/**
 * @file      label_maker.hpp
 * @brief     Defines the LabelMaker class for creating LP variable labels.
 * @date      Sun Jun 22 15:59:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The LabelMaker class provides functionality to generate labels for linear
 * programming (LP) variables based on various context objects (like stages,
 * scenarios, blocks) and options. It conditionally creates labels based on the
 * use_lp_names level:
 *   - Level 0: column names only (for internal use, e.g. cascade solver)
 *   - Level 1: column + row names (for LP file output)
 *   - Level 2: column + row names + strict duplicate detection
 */

#pragma once

#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>

namespace gtopt
{

/// Tag types for dispatching lp_label to col or row variant.
struct ColLabelTag
{
};
struct RowLabelTag
{
};

inline constexpr ColLabelTag col_label_tag {};
inline constexpr RowLabelTag row_label_tag {};

class LabelMaker
{
public:
  explicit constexpr LabelMaker(const OptionsLP& options) noexcept
      : m_lp_names_level_(options.use_lp_names())
  {
  }

  [[nodiscard]]
  constexpr bool dont_use_lp_names() const noexcept
  {
    return m_lp_names_level_ < 0;
  }

  [[nodiscard]]
  constexpr int lp_names_level() const noexcept
  {
    return m_lp_names_level_;
  }

  // ── lp_col_label: generates column names at level >= 0 ──────────────

  template<typename... Types>
  [[nodiscard]] auto lp_col_label(Types&&... args) const -> std::string
  {
    if (m_lp_names_level_ < 0) [[unlikely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...);
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_col_label(StageLP&& stage,
                                            Types&&... args) const
      -> std::string
  {
    if (m_lp_names_level_ < 0) [[unlikely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<StageLP>(stage).uid());
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_col_label(ScenarioLP&& scenario,
                                            StageLP&& stage,
                                            Types&&... args) const
      -> std::string
  {
    if (m_lp_names_level_ < 0) [[unlikely]] {
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
  [[nodiscard]] constexpr auto lp_col_label(ScenarioLP&& scenario,
                                            StageLP&& stage,
                                            BlockLP&& block,
                                            Types&&... args) const
      -> std::string
  {
    if (m_lp_names_level_ < 0) [[unlikely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<ScenarioLP>(scenario).uid(),
                           std::forward<StageLP>(stage).uid(),
                           std::forward<BlockLP>(block).uid());
  }

  // ── lp_row_label: generates row names at level >= 1 ─────────────────

  template<typename... Types>
  [[nodiscard]] auto lp_row_label(Types&&... args) const -> std::string
  {
    if (m_lp_names_level_ < 1) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...);
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_row_label(StageLP&& stage,
                                            Types&&... args) const
      -> std::string
  {
    if (m_lp_names_level_ < 1) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<StageLP>(stage).uid());
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_row_label(ScenarioLP&& scenario,
                                            StageLP&& stage,
                                            Types&&... args) const
      -> std::string
  {
    if (m_lp_names_level_ < 1) [[likely]] {
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
  [[nodiscard]] constexpr auto lp_row_label(ScenarioLP&& scenario,
                                            StageLP&& stage,
                                            BlockLP&& block,
                                            Types&&... args) const
      -> std::string
  {
    if (m_lp_names_level_ < 1) [[likely]] {
      return {};
    }
    return gtopt::as_label(std::forward<Types>(args)...,
                           std::forward<ScenarioLP>(scenario).uid(),
                           std::forward<StageLP>(stage).uid(),
                           std::forward<BlockLP>(block).uid());
  }

  // ── lp_label: backward-compatible, generates at level >= 1 ──────────
  // (same as lp_row_label — preserves old behavior)

  template<typename... Types>
  [[nodiscard]] auto lp_label(Types&&... args) const -> std::string
  {
    return lp_row_label(std::forward<Types>(args)...);
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(StageLP&& stage, Types&&... args) const
      -> std::string
  {
    return lp_row_label(std::forward<StageLP>(stage),
                        std::forward<Types>(args)...);
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] constexpr auto lp_label(ScenarioLP&& scenario,
                                        StageLP&& stage,
                                        Types&&... args) const -> std::string
  {
    return lp_row_label(std::forward<ScenarioLP>(scenario),
                        std::forward<StageLP>(stage),
                        std::forward<Types>(args)...);
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
    return lp_row_label(std::forward<ScenarioLP>(scenario),
                        std::forward<StageLP>(stage),
                        std::forward<BlockLP>(block),
                        std::forward<Types>(args)...);
  }

  // ── Tag-dispatched lp_label: ColLabelTag → col, RowLabelTag → row ──

  template<typename... Types>
  [[nodiscard]] auto lp_label(ColLabelTag /*tag*/, Types&&... args) const
      -> std::string
  {
    return lp_col_label(std::forward<Types>(args)...);
  }

  template<typename... Types>
  [[nodiscard]] auto lp_label(RowLabelTag /*tag*/, Types&&... args) const
      -> std::string
  {
    return lp_row_label(std::forward<Types>(args)...);
  }

private:
  int m_lp_names_level_;
};

}  // namespace gtopt
