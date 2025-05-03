/**
 * @file      utils.hpp
 * @brief     Header of
 * @date      Sat Mar 22 22:12:49 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <range/v3/all.hpp>

namespace gtopt
{

/**
 * Efficiently appends elements from source vector to destination vector using
 * move semantics. After the operation, the source vector will be empty but in a
 * valid state.
 *
 * @tparam T Type of elements in the vectors (must be move-constructible)
 * @param dest Destination vector (will receive elements)
 * @param src Source vector (will be emptied)
 *
 */
template<typename T>
constexpr void merge(std::vector<T>& dest, std::vector<T>&& src) noexcept(
    std::is_nothrow_move_constructible_v<T>)
{
  static_assert(std::is_move_constructible_v<T>,
                "Type T must be move-constructible for efficient append");

  if (&dest == &src) {
    // Handle self-append safely
    std::vector<T> tmp = dest;
    merge(dest, std::move(tmp));
    return;
  }

  if (dest.empty()) {
    dest = std::move(src);  // Complete takeover
  } else if (!src.empty()) {  // Skip if src is empty
    dest.reserve(dest.size() + src.size());
    dest.insert(dest.end(),
                std::make_move_iterator(src.begin()),
                std::make_move_iterator(src.end()));
  }
  src.clear();  // Leave source in valid empty state
}

/**
 * Overload for lvalue source vector (converts to rvalue reference)
 */
template<typename T>
constexpr void merge(std::vector<T>& dest, std::vector<T>& src) noexcept(
    std::is_nothrow_move_constructible_v<T>)
{
  merge(dest, std::move(src));
}

template<typename IndexType = size_t, typename... Ranges>
constexpr auto enumerate(const Ranges&... ranges)
{
  return ranges::views::zip(
      ranges::views::iota(0)
          | ranges::views::transform([](size_t i)
                                     { return static_cast<IndexType>(i); }),
      ranges...);
}

template<typename IndexType = size_t, typename Range, typename Op>
constexpr auto enumerate_if(const Range& range, Op op)
{
  const auto op_second = [&](auto&& p) { return op(std::get<1>(p)); };
  return enumerate<IndexType>(range) | ranges::views::filter(op_second);
}

constexpr auto active_fnc = [](auto&& e) { return e.is_active(); };
constexpr auto true_fnc = [](auto&&) { return true; };

template<typename IndexType = size_t, typename Range>
constexpr auto enumerate_active(const Range& range)
{
  return enumerate_if<IndexType>(range, active_fnc);
}

template<typename Range>
constexpr auto active(const Range& range)
{
  return range | ranges::views::filter(active_fnc);
}

template<typename T, typename K = typename T::key_type>
constexpr auto get_optiter(const T& map, K&& key)
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it)> {it} : std::nullopt;
}

template<typename T, typename K = typename T::key_type>
constexpr auto get_optvalue(const T& map, K&& key)
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it->second)> {it->second}
                           : std::nullopt;
}

template<typename T, typename K>
constexpr auto get_optvalue_optkey(const T& map, const std::optional<K>& key)
{
  return key.has_value() ? get_optvalue(map, key.value()) : std::nullopt;
}

template<typename OptA, typename OptB>
constexpr auto& merge_opt(OptA& a, OptB&& b)
{
  if (!b.has_value()) {
    return a;
  }

  return a = std::forward<OptB>(b);
}

}  // namespace gtopt
