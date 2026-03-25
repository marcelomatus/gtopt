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
 * LpNamesLevel:
 *   - minimal:      state-variable column names only (cascade solver)
 *   - only_cols:    all column names + name maps
 *   - cols_and_rows: column + row names + maps + warn on duplicates
 */

#pragma once

#include <gtopt/as_label.hpp>
#include <gtopt/planning_options_lp.hpp>
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
  explicit constexpr LabelMaker(const PlanningOptionsLP& options) noexcept
      : m_names_level_(options.names_level())
  {
  }

  [[nodiscard]]
  constexpr LpNamesLevel names_level() const noexcept
  {
    return m_names_level_;
  }

  // ── state_col_label: always generates (state variables need names) ──

  template<typename... Types>
  [[nodiscard]] auto state_col_label(Types&&... args) const -> std::string
  {
    gtopt::as_label_into(label_buf_, std::forward<Types>(args)...);
    return label_buf_;
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto state_col_label(StageLP&& stage, Types&&... args) const
      -> std::string
  {
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<StageLP>(stage).uid());
    return label_buf_;
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto state_col_label(ScenarioLP&& scenario,
                                     StageLP&& stage,
                                     Types&&... args) const -> std::string
  {
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<ScenarioLP>(scenario).uid(),
                         std::forward<StageLP>(stage).uid());
    return label_buf_;
  }

  template<typename ScenarioLP,
           typename StageLP,
           typename BlockLP,
           typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && std::same_as<std::remove_cvref_t<BlockLP>, gtopt::BlockLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto state_col_label(ScenarioLP&& scenario,
                                     StageLP&& stage,
                                     BlockLP&& block,
                                     Types&&... args) const -> std::string
  {
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<ScenarioLP>(scenario).uid(),
                         std::forward<StageLP>(stage).uid(),
                         std::forward<BlockLP>(block).uid());
    return label_buf_;
  }

  // ── lp_col_label: generates column names at level >= 1 ──────────────

  template<typename... Types>
  [[nodiscard]] auto lp_col_label(Types&&... args) const -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_, std::forward<Types>(args)...);
    return label_buf_;
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto lp_col_label(StageLP&& stage, Types&&... args) const
      -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<StageLP>(stage).uid());
    return label_buf_;
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto lp_col_label(ScenarioLP&& scenario,
                                  StageLP&& stage,
                                  Types&&... args) const -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<ScenarioLP>(scenario).uid(),
                         std::forward<StageLP>(stage).uid());
    return label_buf_;
  }

  template<typename ScenarioLP,
           typename StageLP,
           typename BlockLP,
           typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && std::same_as<std::remove_cvref_t<BlockLP>, gtopt::BlockLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto lp_col_label(ScenarioLP&& scenario,
                                  StageLP&& stage,
                                  BlockLP&& block,
                                  Types&&... args) const -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<ScenarioLP>(scenario).uid(),
                         std::forward<StageLP>(stage).uid(),
                         std::forward<BlockLP>(block).uid());
    return label_buf_;
  }

  // ── lp_row_label: generates row names at level >= 1 ─────────────────

  template<typename... Types>
  [[nodiscard]] auto lp_row_label(Types&&... args) const -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_, std::forward<Types>(args)...);
    return label_buf_;
  }

  template<typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto lp_row_label(StageLP&& stage, Types&&... args) const
      -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<StageLP>(stage).uid());
    return label_buf_;
  }

  template<typename ScenarioLP, typename StageLP, typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto lp_row_label(ScenarioLP&& scenario,
                                  StageLP&& stage,
                                  Types&&... args) const -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<ScenarioLP>(scenario).uid(),
                         std::forward<StageLP>(stage).uid());
    return label_buf_;
  }

  template<typename ScenarioLP,
           typename StageLP,
           typename BlockLP,
           typename... Types>
    requires std::same_as<std::remove_cvref_t<ScenarioLP>, gtopt::ScenarioLP>
      && std::same_as<std::remove_cvref_t<StageLP>, gtopt::StageLP>
      && std::same_as<std::remove_cvref_t<BlockLP>, gtopt::BlockLP>
      && (sizeof...(Types) >= 3)
  [[nodiscard]] auto lp_row_label(ScenarioLP&& scenario,
                                  StageLP&& stage,
                                  BlockLP&& block,
                                  Types&&... args) const -> std::string
  {
    if (m_names_level_ < LpNamesLevel::only_cols) [[likely]] {
      return {};
    }
    gtopt::as_label_into(label_buf_,
                         std::forward<Types>(args)...,
                         std::forward<ScenarioLP>(scenario).uid(),
                         std::forward<StageLP>(stage).uid(),
                         std::forward<BlockLP>(block).uid());
    return label_buf_;
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
  LpNamesLevel m_names_level_;
  mutable std::string label_buf_;
};

}  // namespace gtopt
