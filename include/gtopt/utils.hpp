/**
 * @file      utils.hpp
 * @brief     General purpose utility functions and helpers
 * @date      Sat Mar 22 22:12:49 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides various utility functions for:
 * - Container operations (merging, filtering)
 * - Range/view operations (enumerating, filtering)
 * - Optional value handling
 * - Common predicates
 */
#pragma once

#include <optional>
#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <range/v3/all.hpp>

namespace gtopt
{

/**
 * @brief Efficiently merges two vectors using move semantics
 *
 * Appends elements from source vector to destination vector. After the
 * operation, the source vector will be empty but in a valid state. Handles
 * self-merge safely.
 *
 * @tparam T Type of elements (must be move-constructible)
 * @param dest Destination vector (will receive elements)
 * @param src Source vector (will be emptied)
 * @throws Nothing (noexcept)
 *
 * @note Complexity: O(N) where N is size of src
 * @note If dest is empty, simply moves src into dest
 * @note If src is empty, does nothing
 * @note Self-merge is handled safely via temporary copy
 */
template<typename T>
  requires std::is_move_constructible_v<T>
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

/**
 * @brief Creates an enumerated view over one or more ranges
 *
 * Returns a zip view pairing indices (starting from 0) with range elements.
 *
 * @tparam IndexType Type of index (default size_t)
 * @tparam Ranges Range types to enumerate
 * @param ranges Ranges to enumerate
 * @return Zip view of (index, range_element...) pairs
 * @throws Nothing (noexcept)
 *
 * @example
 * for (auto [i, val] : enumerate(vec)) {
 *   // i is index, val is element
 * }
 */
template<typename IndexType = size_t, ranges::range... Ranges>
[[nodiscard]] constexpr auto enumerate(const Ranges&... ranges) noexcept
{
  return ranges::views::zip(
      ranges::views::iota(0)
          | ranges::views::transform([](size_t i)
                                     { return static_cast<IndexType>(i); }),
      ranges...);
}

template<typename IndexType = size_t, ranges::range Range, typename Op>
  requires std::invocable<Op, ranges::range_value_t<Range>>
[[nodiscard]] constexpr auto enumerate_if(const Range& range, Op op) noexcept(
    noexcept(op(std::declval<ranges::range_value_t<Range>>())))
{
  const auto op_second = [&](auto&& p) { return op(std::get<1>(p)); };
  return enumerate<IndexType>(range) | ranges::views::filter(op_second);
}

/// Predicate that checks if element has is_active() member returning true
constexpr auto active_fnc = [](const auto& e) noexcept(noexcept(
                                e.is_active())) -> decltype(e.is_active())
{ return e.is_active(); };

/// Predicate that always returns true
constexpr auto true_fnc = [](const auto&) noexcept { return true; };

/// Predicate that checks if element exists (not nullopt)
constexpr auto has_value_fnc = [](const auto& opt) noexcept
{ return opt.has_value(); };

/// Predicate that checks if optional has value and value is true
constexpr auto is_true_fnc = [](const auto& opt) noexcept
{ return opt.has_value() && opt.value(); };

template<typename IndexType = size_t, typename Range>
constexpr auto enumerate_active(const Range& range) noexcept
{
  return enumerate_if<IndexType>(range, active_fnc);
}

template<typename Range>
constexpr auto active(const Range& range) noexcept
{
  return range | ranges::views::filter(active_fnc);
}

/**
 * @brief Finds key in map and returns optional iterator
 *
 * @tparam T Map type
 * @tparam K Key type (defaults to map's key_type)
 * @param map Map to search
 * @param key Key to find
 * @return std::optional containing iterator if found, nullopt otherwise
 * @throws Nothing (noexcept)
 */
template<typename T, typename K = typename T::key_type>
  requires requires(const T& m, K&& k) { m.find(std::forward<K>(k)); }
[[nodiscard]] constexpr auto get_optiter(const T& map, K&& key) noexcept
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it)> {it} : std::nullopt;
}

template<typename T, typename K = typename T::key_type>
constexpr auto get_optvalue(const T& map, K&& key) noexcept
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it->second)> {it->second}
                           : std::nullopt;
}

template<typename T, typename K>
constexpr auto get_optvalue_optkey(const T& map,
                                   const std::optional<K>& key) noexcept
{
  return key.has_value() ? get_optvalue(map, key.value()) : std::nullopt;
}

/**
 * @brief Merges two optionals, keeping value if source has value
 *
 * If source optional (b) has value, assigns it to destination (a).
 * Otherwise leaves destination unchanged.
 *
 * @tparam OptA Destination optional type
 * @tparam OptB Source optional type
 * @param a Destination optional (modified in place)
 * @param b Source optional (rvalue reference)
 * @return Reference to modified destination optional
 * @throws Nothing (noexcept)
 */
template<typename OptA, typename OptB>
  requires requires(OptA& a, OptB&& b) { a = std::forward<OptB>(b); }
constexpr auto& merge_opt(OptA& a, OptB&& b) noexcept
{
  if (b.has_value()) {
    a = std::forward<OptB>(b);
  }
  return a;
}

/**
 * @brief Converts range to vector, optionally transforming elements
 *
 * @tparam Range Input range type
 * @tparam Transform Unary transform operation (defaults to identity)
 * @param range Input range
 * @param transform Transform operation (optional)
 * @return std::vector containing transformed elements
 */
template<ranges::range Range, typename Transform = std::identity>
[[nodiscard]] auto to_vector(Range&& range, Transform transform = {})
{
  std::vector<std::remove_cvref_t<
      std::invoke_result_t<Transform, ranges::range_value_t<Range>>>>
      result;
  if constexpr (ranges::sized_range<Range>) {
    result.reserve(ranges::size(range));
  }
  ranges::transform(
      std::forward<Range>(range), std::back_inserter(result), transform);
  return result;
}

/**
 * @brief Checks if all elements in range satisfy predicate
 *
 * @tparam Range Input range type
 * @tparam Pred Predicate type
 * @param range Input range
 * @param pred Predicate to test
 * @return true if all elements satisfy predicate, false otherwise
 */
template<ranges::range Range, typename Pred>
[[nodiscard]] constexpr bool all_of(Range&& range, Pred pred) noexcept(
    noexcept(pred(*ranges::begin(range))))
{
  return ranges::all_of(std::forward<Range>(range), pred);
}

}  // namespace gtopt
