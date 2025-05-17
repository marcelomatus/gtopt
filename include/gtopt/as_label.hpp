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
 * - Built-in numeric types (converted via std::to_string)
 * - Any type convertible to string_view
 * - Any type formattable via std::to_string
 *
 * Example usage:
 * @code
 * auto label1 = as_label("prefix", 42, "suffix"); // "prefix_42_suffix"
 * auto label2 = as_label<'-'>("a", "b", "c");    // "a-b-c"
 * @endcode
 */

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace gtopt
{

namespace detail
{

// Improved concept for string-like types
template<typename T>
concept string_like = std::is_convertible_v<const T&, std::string_view>;

// Simplified string holder using C++23 features
class string_holder
{
  std::variant<std::string_view, std::string> storage;

  template<string_like T>
  static constexpr auto as_string(T&& t)
  {
    if constexpr (std::is_convertible_v<T, std::string_view>) {
      return std::string_view(std::forward<T>(t));
    } else {
      return std::to_string(std::forward<T>(t));
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

  // For non-string types
  template<typename T>
    requires(!string_like<T>)
  constexpr explicit string_holder(const T& value)
      : storage(std::to_string(value))
  {
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept
  {
    return std::visit(
        [](const auto& s) -> std::string_view
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
    return {.total = total + (needs_sep ? 1 : 0) + view.size(),
            .needs_sep = true};
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
[[nodiscard]] constexpr std::string as_label() noexcept
{
  return {};
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
[[nodiscard]] constexpr std::string as_label(Args&&... args) noexcept(
    (std::is_nothrow_constructible_v<detail::string_holder, Args> && ...))
{
  // Create holders for all arguments
  std::array<detail::string_holder, sizeof...(Args)> holders {
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
      result.push_back(sep);
    }
    result.append(view);
    needs_sep = true;
  }

  return result;
}

}  // namespace gtopt
