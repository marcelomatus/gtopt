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

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>

namespace gtopt
{

/// Throw `std::logic_error` if @p opt has no value, otherwise return `*opt`.
///
/// Used by element-LP wrappers to convert an optional foreign-key field
/// (e.g. `Turbine::waterway`) into a strong SId without silent UB when
/// the field was unset.  The previous form (`assert(opt.has_value()); *opt`)
/// compiled out in release, leaving `*opt` on a disengaged optional —
/// silent UB.  This helper makes the precondition violation visible.
template<typename U>
[[nodiscard]] constexpr auto require_sid(const std::optional<U>& opt,
                                         std::string_view caller,
                                         std::string_view field) -> U
{
  if (!opt.has_value()) {
    throw std::logic_error(
        std::format("{}: required field '{}' is unset (precondition violated)",
                    caller,
                    field));
  }
  return *opt;
}

/// Lightweight replacement for std::views::iota (not available in libc++ 21).
/// Returns a random-access range [first, last) that is compatible with
/// range adaptors such as std::views::reverse and std::views::transform.
template<typename T>
class IotaRange : public std::ranges::view_interface<IotaRange<T>>
{
public:
  class iterator
  {
  public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = T;

    constexpr iterator() = default;
    constexpr explicit iterator(T value)
        : value_(value)
    {
    }

    constexpr auto operator*() const -> T { return value_; }
    constexpr auto operator[](difference_type n) const -> T
    {
      auto copy = value_;
      copy += static_cast<T>(n);
      return copy;
    }

    constexpr auto operator++() -> iterator&
    {
      ++value_;
      return *this;
    }
    constexpr auto operator++(int) -> iterator
    {
      auto tmp = *this;
      ++value_;
      return tmp;
    }
    constexpr auto operator--() -> iterator&
    {
      --value_;
      return *this;
    }
    constexpr auto operator--(int) -> iterator
    {
      auto tmp = *this;
      --value_;
      return tmp;
    }

    constexpr auto operator+=(difference_type n) -> iterator&
    {
      value_ += static_cast<T>(n);
      return *this;
    }
    constexpr auto operator-=(difference_type n) -> iterator&
    {
      value_ -= static_cast<T>(n);
      return *this;
    }

    friend constexpr auto operator+(iterator it, difference_type n) -> iterator
    {
      it += n;
      return it;
    }
    friend constexpr auto operator+(difference_type n, iterator it) -> iterator
    {
      it += n;
      return it;
    }
    friend constexpr auto operator-(iterator it, difference_type n) -> iterator
    {
      it -= n;
      return it;
    }
    friend constexpr auto operator-(iterator lhs, iterator rhs)
        -> difference_type
    {
      return static_cast<difference_type>(lhs.value_)
          - static_cast<difference_type>(rhs.value_);
    }

    constexpr auto operator<=>(const iterator&) const = default;
    constexpr auto operator==(const iterator&) const -> bool = default;

  private:
    T value_ {};
  };

  constexpr IotaRange(T first, T last)
      : first_(first)
      , last_(last)
  {
  }

  [[nodiscard]] constexpr auto begin() const -> iterator
  {
    return iterator(first_);
  }
  [[nodiscard]] constexpr auto end() const -> iterator
  {
    return iterator(last_);
  }
  [[nodiscard]] constexpr auto size() const -> std::size_t
  {
    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(last_)
                                    - static_cast<std::ptrdiff_t>(first_));
  }
  [[nodiscard]] constexpr auto empty() const -> bool { return first_ == last_; }

private:
  T first_;
  T last_;
};

/// Create an IotaRange [first, last) with deduced type.
template<typename T>
constexpr auto iota_range(T first, T last) -> IotaRange<T>
{
  return IotaRange<T>(first, last);
}

/// Create an IotaRange [first, last) with explicit IndexType,
/// similar to enumerate<IndexType>(...).
/// Usage: iota_range<PhaseIndex>(0, num_phases)
template<typename IndexType, typename A, typename B>
constexpr auto iota_range(A first, B last) -> IotaRange<IndexType>
{
  return IotaRange<IndexType>(static_cast<IndexType>(first),
                              static_cast<IndexType>(last));
}

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
constexpr bool merge(std::vector<T>& dest, std::vector<T>&& src) noexcept(
    std::is_nothrow_move_constructible_v<T>)
{
  if (&dest == &src) {
    return false;  // No-op for self-merge
  }

  if (dest.empty()) {
    dest = std::move(src);  // Complete takeover
    return true;
  }

  if (src.empty()) {
    return false;
  }

  dest.reserve(dest.size() + src.size());
  std::ranges::move(src, std::back_inserter(dest));
  src.clear();
  return true;
}

/**
 * Overload for lvalue source vector (converts to rvalue reference)
 */
template<typename T>
constexpr bool merge(std::vector<T>& dest, std::vector<T>& src) noexcept(
    std::is_nothrow_move_constructible_v<T>)
{
  return merge(dest, std::move(src));
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

template<typename IndexType = std::size_t>
struct enumerate_adapter
    : std::ranges::range_adaptor_closure<enumerate_adapter<IndexType>>
{
private:
  static constexpr auto make_index_view()
  {
    return IotaRange<std::size_t>(std::size_t {0},
                                  std::numeric_limits<std::size_t>::max())
        | std::ranges::views::transform([](std::size_t i)
                                        { return static_cast<IndexType>(i); });
  }

public:
  // Single range: pipeable via range_adaptor_closure
  template<std::ranges::range R>
  [[nodiscard]] constexpr auto operator()(R&& range) const
      noexcept(noexcept(std::ranges::views::zip(make_index_view(),
                                                std::forward<R>(range))))
  {
    return std::ranges::views::zip(make_index_view(), std::forward<R>(range));
  }

  // Multi-range: direct call only
  template<std::ranges::range R, std::ranges::range... Rs>
    requires(sizeof...(Rs) >= 1)
  [[nodiscard]] constexpr auto operator()(R&& range, Rs&&... ranges) const
      noexcept(noexcept(std::ranges::views::zip(make_index_view(),
                                                std::forward<R>(range),
                                                std::forward<Rs>(ranges)...)))
  {
    return std::ranges::views::zip(
        make_index_view(), std::forward<R>(range), std::forward<Rs>(ranges)...);
  }
};

// Function factory — allows enumerate(...) without template args
template<typename IndexType = std::size_t,
         std::ranges::range R,
         std::ranges::range... Rs>
[[nodiscard]] constexpr auto enumerate(R&& range, Rs&&... ranges) noexcept(
    noexcept(enumerate_adapter<IndexType> {}(std::forward<R>(range),
                                             std::forward<Rs>(ranges)...)))
{
  return enumerate_adapter<IndexType> {}(std::forward<R>(range),
                                         std::forward<Rs>(ranges)...);
}

template<typename IndexType = size_t, std::ranges::range Range, typename Op>
  requires std::invocable<Op, std::ranges::range_value_t<Range>>
[[nodiscard]] constexpr auto enumerate_if(Range&& range, Op&& op) noexcept(
    noexcept(op(std::declval<std::ranges::range_value_t<Range>>())))
{
  return enumerate<IndexType>(
      std::forward<Range>(range)
      | std::ranges::views::filter(std::forward<Op>(op)));
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

/// Predicate that checks if optional has value and value is true (C++23
/// value_or)
constexpr auto is_true_fnc = [](const auto& opt)
{ return opt.value_or(false); };

template<typename IndexType = size_t, typename Range>
[[nodiscard]] constexpr auto enumerate_active(const Range& range) noexcept
{
  return enumerate_if<IndexType>(range, active_fnc);
}

template<std::ranges::range Range>
[[nodiscard]] constexpr auto active(Range&& range) noexcept
{
  return std::forward<Range>(range) | std::ranges::views::filter(active_fnc);
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
template<typename T, typename K = T::key_type>
  requires requires(const T& m, K&& k) { m.find(std::forward<K>(k)); }
[[nodiscard]] constexpr auto get_optiter(const T& map, K&& key) noexcept
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it)> {it} : std::nullopt;
}

template<typename T, typename K>
[[nodiscard]] constexpr auto get_optvalue(const T& map, K&& key)
{
  auto it = map.find(std::forward<K>(key));
  using value_t = std::remove_cvref_t<decltype(it->second)>;
  return (it != map.end()) ? std::optional<value_t> {it->second} : std::nullopt;
}

template<typename T, typename K>
[[nodiscard]] constexpr auto get_optvalue_optkey(const T& map,
                                                 const std::optional<K>& key)
{
  return key.and_then([&map](const K& k) { return get_optvalue(map, k); });
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
constexpr auto& merge_opt(OptA& a, OptB&& b) noexcept(
    noexcept(a = std::forward<OptB>(b)))
{
  if (b) {
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
template<std::ranges::range Range,
         typename Transform = decltype([](const auto& x) { return x; }),
         typename RRef = decltype(*std::ranges::begin(std::declval<Range>()))>
  requires std::invocable<Transform, RRef>
[[nodiscard]] auto to_vector(Range&& range, Transform&& transform = {})
{
  std::vector<std::invoke_result_t<Transform, RRef>> result;
  if constexpr (std::ranges::sized_range<Range>) {
    result.reserve(std::ranges::size(range));
  }
  std::ranges::transform(std::forward<Range>(range),
                         std::back_inserter(result),
                         std::forward<Transform>(transform));
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
template<std::ranges::range Range, typename Pred>
[[nodiscard]] constexpr bool all_of(Range&& range, Pred&& pred) noexcept(
    noexcept(pred(*std::ranges::begin(range))))
{
  return std::ranges::all_of(std::forward<Range>(range),
                             std::forward<Pred>(pred));
}

[[nodiscard]] constexpr double annual_discount_factor(
    double annual_discount_rate, double time_hours) noexcept
{
  return std::exp(-std::log1p(annual_discount_rate)
                  * (time_hours / hours_per_year));
}

template<typename... Args>
[[nodiscard]] std::string as_string(const std::tuple<Args...>& t)
{
  return std::apply(
      [](const Args&... args)
      {
        std::string result = "(";
        std::size_t count = 0;
        ((result +=
          std::format("{}", args) + (++count < sizeof...(Args) ? ", " : "")),
         ...);
        result += ')';
        return result;
      },
      t);
}

}  // namespace gtopt
