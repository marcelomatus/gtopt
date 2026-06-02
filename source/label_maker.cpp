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
#include <gtopt/ascii_name_cache.hpp>
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

/// Format an extended-style label: same shape as the compact form, but
/// with `element_name` substituted into the id segment in place of the
/// raw UID.  See `docs/design/lp-extended-labels.md` §4.2.  `class_name`
/// is still lowercased; the element name is preserved verbatim (the
/// ASCIIfication has already been applied by the cache).
template<typename Tuple, size_t... Is>
[[nodiscard]] auto format_with_context_name(
    std::string_view class_name,
    std::string_view variable,
    std::string_view element_name,
    const Tuple& context,
    std::index_sequence<Is...> /*unused*/) -> std::string
{
  return as_label(
      lowercase(class_name), variable, element_name, std::get<Is>(context)...);
}

/// Format a label from (class_name, variable, uid) without context.
[[nodiscard]] auto format_no_context(std::string_view class_name,
                                     std::string_view variable,
                                     Uid uid) -> std::string
{
  return as_label(lowercase(class_name), variable, uid);
}

/// Extended-style no-context format: substitutes `element_name` for `uid`.
[[nodiscard]] auto format_no_context_name(std::string_view class_name,
                                          std::string_view variable,
                                          std::string_view element_name)
    -> std::string
{
  return as_label(lowercase(class_name), variable, element_name);
}

/// Format a label from (class_name, variable, uid, LpContext) with
/// optional cache-aware extended-name rendering (issue #508).
///
/// Under `style == compact` (or when `cache == nullptr` / element name
/// is empty) the function is byte-identical to the pre-#508 path —
/// `cache` is never probed.  Under `style == extended` with a non-empty
/// cache hit, the asciified element name replaces the UID in the id
/// segment; on cache miss (anonymous synthetic column, unregistered
/// uid) the function falls back to the compact form for that single
/// label.
[[nodiscard]] auto format_label(LpLabelStyle style,
                                const AsciiNameCache* cache,
                                std::string_view class_name,
                                std::string_view variable,
                                Uid uid,
                                const LpContext& context) -> std::string
{
  if (class_name.empty()) {
    return {};
  }

  // The cache is keyed by snake_case class name to match the
  // `AmplElementNameKey` storage convention populated in
  // `system_lp.cpp` (which records `LP::Element::class_name.snake_case()`).
  // `SparseCol::class_name` carries the *PascalCase* `full_name()`
  // view, so we materialise the snake_case form once per label probe
  // into a small local buffer and feed that into the cache.
  //
  // Cost: one snake_case allocation per extended-style label, sized to
  // the LP class name (≤ 32 bytes in practice — `ReservoirProductionFactor`
  // is the longest at 25 chars + 2 underscores = 27).  Bounded by
  // `LPClassName::max_short_len`; small-string optimisation usually
  // avoids the heap.  Cheap relative to the surrounding `as_label`
  // call that materialises the full label string.
  std::string_view element_name {};
  std::string class_snake_storage;
  if (style == LpLabelStyle::extended && cache != nullptr) {
    class_snake_storage = std::string(snake_case(class_name));
    element_name = cache->lookup(class_snake_storage, uid);
  }
  const bool use_name = !element_name.empty();

  return std::visit(
      [&](const auto& ctx) -> std::string
      {
        using T = std::decay_t<decltype(ctx)>;
        if constexpr (std::same_as<T, std::monostate>) {
          return use_name
              ? format_no_context_name(class_name, variable, element_name)
              : format_no_context(class_name, variable, uid);
        } else {
          return use_name
              ? format_with_context_name(
                    class_name,
                    variable,
                    element_name,
                    ctx,
                    std::make_index_sequence<std::tuple_size_v<T>> {})
              : format_with_context(
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
  return format_label(m_style_,
                      m_cache_,
                      col.class_name,
                      col.variable_name,
                      col.variable_uid,
                      col.context);
}

std::string LabelMaker::make_row_label(const SparseRow& row) const
{
  if (!row_names_enabled()) [[likely]] {
    return {};
  }
  return format_label(m_style_,
                      m_cache_,
                      row.class_name,
                      row.constraint_name,
                      row.variable_uid,
                      row.context);
}

}  // namespace gtopt
