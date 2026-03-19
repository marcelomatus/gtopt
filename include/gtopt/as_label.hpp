/**
 * @file      as_label.hpp
 * @brief     String label generation utilities
 * @date      Fri May 16 20:16:01 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides compile-time string label generation functionality with:
 * - Efficient concatenation of multiple values
 * - Type-safe string conversion
 * - Custom separator support
 * - Move semantics optimization
 *
 * The main interface is the `as_label()` function which:
 * - Accepts any number of arguments of different types
 * - Converts each argument to string representation
 * - Joins them with a configurable separator
 * - Returns a concatenated std::string
 *
 * Supported argument types:
 * - std::string and string views
 * - Built-in numeric types (converted via std::format)
 * - Any type convertible to string_view
 * - Any type formattable via std::format
 *
 * Example usage:
 * @code
 * auto label1 = as_label("prefix", 42, "suffix"); // "prefix_42_suffix"
 * auto label2 = as_label<'-'>("a", "b", "c");    // "a-b-c"
 * @endcode
 */

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace gtopt
{

namespace detail
{

[[nodiscard]] constexpr char to_lower_char(char c) noexcept
{
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

// Improved concept for string-like types
template<typename T>
concept string_like = std::is_convertible_v<const T&, std::string_view>;

// Concept for types that are integral or implicitly convertible to an integral
// type (e.g. strong::type<int, ...> with implicitly_convertible_to<int>).
// This enables the fast std::to_chars path instead of std::format.
template<typename T>
concept integral_convertible = std::integral<T>
    || (std::is_convertible_v<T, std::int64_t> && !std::is_floating_point_v<T>
        && !string_like<T> && !std::is_same_v<std::remove_cvref_t<T>, bool>);

// Maximum chars needed for a 64-bit signed integer: "-9223372036854775808"
inline constexpr std::size_t int_buf_size = 21;

// Convert an integral value to a string using std::to_chars (no format string
// parsing overhead).  Returns a std::string owning the converted text.
template<integral_convertible T>
[[nodiscard]] inline std::string int_to_string(const T& value)
{
  std::array<char, int_buf_size> buf {};
  const auto ival = static_cast<std::int64_t>(value);
  auto* const begin = buf.data();
  auto* const end = begin
      + buf.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] = std::to_chars(begin, end, ival);
  return {begin, ptr};
}

// Simplified string holder using C++23 features
class string_holder
{
  std::variant<std::string_view, std::string> storage;

  template<string_like T>
  static constexpr auto as_string(T&& t)
  {
    if constexpr (std::is_constructible_v<T, std::string_view>) {
      return std::string_view(std::forward<T>(t));
    } else {
      return std::format("{}", std::forward<T>(t));
    }
  }

public:
  // For string-like types (views)
  template<string_like T>
    requires(!std::same_as<std::remove_cvref_t<T>, std::string>)
  constexpr explicit string_holder(T&& value) noexcept(
      noexcept(as_string(std::forward<T>(value))))
      : storage(as_string(std::forward<T>(value)))
  {
  }

  // For strings (avoid extra conversion)
  constexpr explicit string_holder(const std::string& s) noexcept
      : storage(std::string_view(s))
  {
  }

  // For rvalue strings (take ownership)
  constexpr explicit string_holder(std::string&& s) noexcept
      : storage(std::move(s))
  {
  }

  // Fast path for integral and integral-convertible types (strong int types).
  // Uses std::to_chars instead of std::format to avoid format string parsing.
  template<typename T>
    requires(!string_like<T> && integral_convertible<T>)
  explicit string_holder(const T& value)
      : storage(int_to_string(value))
  {
  }

  // Fallback for non-string, non-integral types (floating point, custom
  // formatters)
  template<typename T>
    requires(!string_like<T> && !integral_convertible<T>)
  explicit string_holder(const T& value)
      : storage(std::format("{}", value))
  {
  }

  [[nodiscard]] constexpr std::string_view view() const
  {
    return std::visit(
        [](const auto& s) noexcept -> std::string_view
        {
          if constexpr (std::same_as<std::decay_t<decltype(s)>,
                                     std::string_view>) {
            return s;
          } else {
            return std::string_view(s);
          }
        },
        storage);
  }
};

// Compile-time size calculation
struct label_size
{
  size_t total = 0;
  bool needs_sep = false;

  [[nodiscard]] constexpr label_size add(std::string_view view) const noexcept
  {
    if (view.empty()) [[unlikely]] {
      return *this;
    }
    return {
        .total = total + (needs_sep ? 1 : 0) + view.size(),
        .needs_sep = true,
    };
  }
};

}  // namespace detail

/**
 * @brief Creates an empty label string
 *
 * @tparam sep Separator character (default '_')
 * @return constexpr std::string Empty string
 *
 * @note This is the base case for empty argument lists
 */
template<char sep = '_'>
[[nodiscard]] constexpr auto as_label() noexcept
{
  return std::string();
}

/**
 * @brief Creates a concatenated label from multiple arguments
 *
 * @tparam sep Separator character between components (default '_')
 * @tparam Args Argument types (automatically deduced)
 * @param args Values to concatenate into label
 * @return constexpr std::string Concatenated label string
 *
 * @throws Nothing if all arguments can be converted to string without throwing
 *
 * @note Arguments are converted to strings in order
 * @note Empty arguments are skipped (no trailing separators)
 * @note The function is constexpr and noexcept when possible
 *
 * Example:
 * @code
 * auto label = as_label("config", "value", 42); // "config_value_42"
 * @endcode
 */
template<char sep = '_', typename... Args>
[[nodiscard]] std::string as_label(Args&&... args) noexcept(
    (std::is_nothrow_constructible_v<detail::string_holder, Args> && ...))
{
  // Create holders for all arguments
  const std::array<detail::string_holder, sizeof...(Args)> holders {
      detail::string_holder(std::forward<Args>(args))...};

  // Calculate total size needed
  detail::label_size size;
  for (const auto& holder : holders) {
    size = size.add(holder.view());
  }

  if (size.total == 0) [[unlikely]] {
    return {};
  }

  // Build the result string
  std::string result;
  result.reserve(size.total);

  bool needs_sep = false;
  for (const auto& holder : holders) {
    const auto view = holder.view();
    if (view.empty()) [[unlikely]] {
      continue;
    }
    if (needs_sep) {
      result.push_back(detail::to_lower_char(sep));
    }
    std::ranges::transform(
        view, std::back_inserter(result), detail::to_lower_char);
    needs_sep = true;
  }

  return result;
}

}  // namespace gtopt
