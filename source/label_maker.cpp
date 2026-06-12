/**
 * @file      label_maker.cpp
 * @brief     Implementation of LabelMaker: the single LP label formatter.
 * @date      2026-04-10
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

namespace
{

/// Format a label from (class_name, variable, uid) and an unpacked context
/// tuple (e.g. {scenario_uid, stage_uid, block_uid}).
template<typename Tuple, size_t... Is>
[[nodiscard]] auto format_with_context(std::string_view class_name,
                                       std::string_view variable,
                                       Uid uid,
                                       const Tuple& context,
                                       std::index_sequence<Is...> /*unused*/)
    -> std::string
{
  return as_label(
      lowercase(class_name), variable, uid, std::get<Is>(context)...);
}

/// Format a label from (class_name, variable, uid) without context.
[[nodiscard]] auto format_no_context(std::string_view class_name,
                                     std::string_view variable,
                                     Uid uid) -> std::string
{
  return as_label(lowercase(class_name), variable, uid);
}

/// Format a label from (class_name, variable, uid, LpContext).
/// Returns an empty string when `class_name` is empty.
[[nodiscard]] auto format_label(std::string_view class_name,
                                std::string_view variable,
                                Uid uid,
                                const LpContext& context) -> std::string
{
  if (class_name.empty()) {
    return {};
  }
  return std::visit(
      [&](const auto& ctx) -> std::string
      {
        using T = std::decay_t<decltype(ctx)>;
        if constexpr (std::same_as<T, std::monostate>) {
          return format_no_context(class_name, variable, uid);
        } else {
          return format_with_context(
              class_name,
              variable,
              uid,
              ctx,
              std::make_index_sequence<std::tuple_size_v<T>> {});
        }
      },
      context);
}

}  // namespace

std::string LabelMaker::make_col_label(const SparseCol& col) const
{
  if (!col_names_enabled()) [[likely]] {
    return {};
  }
  return format_label(
      col.class_name, col.variable_name, col.variable_uid, col.context);
}

std::string LabelMaker::make_row_label(const SparseRow& row) const
{
  if (!row_names_enabled()) [[likely]] {
    return {};
  }
  return format_label(
      row.class_name, row.constraint_name, row.variable_uid, row.context);
}

}  // namespace gtopt
