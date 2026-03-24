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
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

// Compile-time lookup table mapping integers 0–1023 to their string
// representations.  The buffer and index are built entirely at compile
// time so that cached_int_view() returns a std::string_view pointing
// into static storage with zero runtime allocation.

inline constexpr std::size_t int_cache_size = 1024;

// Total chars: 1*10 + 2*90 + 3*900 + 4*24 = 2986, plus 1024 NULs.
// Upper bound: 4 * 1024 + 1024 = 5120 chars is safe.
inline constexpr std::size_t int_cache_buf_len = 5120;

struct IntCacheData
{
  std::array<char, int_cache_buf_len> buf {};
  std::array<std::uint16_t, int_cache_size> offsets {};
  std::array<std::uint8_t, int_cache_size> lengths {};
};

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,misc-const-correctness)
consteval IntCacheData build_int_cache() noexcept
{
  IntCacheData data {};
  std::size_t pos = 0;

  for (std::size_t i = 0; i < int_cache_size; ++i) {
    data.offsets[i] = static_cast<std::uint16_t>(pos);

    // Convert i to decimal digits into the buffer
    if (i == 0) {
      data.buf[pos] = '0';
      data.lengths[i] = 1;
      pos += 2;  // char + NUL
    } else {
      // Write digits in reverse, then flip
      std::size_t start = pos;
      std::size_t val = i;
      while (val > 0) {
        data.buf[pos] = static_cast<char>('0' + (val % 10));
        ++pos;
        val /= 10;
      }
      data.lengths[i] = static_cast<std::uint8_t>(pos - start);
      // Reverse the digits in place
      std::size_t lo = start;
      std::size_t hi = pos - 1;
      while (lo < hi) {
        char tmp = data.buf[lo];
        data.buf[lo] = data.buf[hi];
        data.buf[hi] = tmp;
        ++lo;
        --hi;
      }
      ++pos;  // NUL terminator (already zero-initialized)
    }
  }
  return data;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,misc-const-correctness)

inline constexpr IntCacheData int_cache = build_int_cache();

[[nodiscard]] constexpr std::optional<std::string_view> cached_int_view(
    std::int64_t n) noexcept
{
  if (std::cmp_less(n, 0) || std::cmp_greater_equal(n, int_cache_size))
      [[unlikely]]
  {
    return std::nullopt;
  }
  auto idx = static_cast<std::size_t>(n);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
  return std::string_view(int_cache.buf.data() + int_cache.offsets[idx],
                          int_cache.lengths[idx]);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
}

/// String holder that avoids heap allocation for integral and string_view
/// arguments.  Integer values are formatted into a small stack buffer.
/// Only the std::format fallback path allocates on the heap.
///
/// Uses a tag to track which storage is active, computing the view on
/// demand in view().  This avoids self-referential pointers that would
/// break on move/copy.
class string_holder
{
  enum class Tag : std::uint8_t
  {
    ext,
    buf,
    owned,
  };

  std::array<char, int_buf_size> int_buf_ {};
  std::string owned_;
  std::string_view ext_;
  std::uint8_t int_len_ {0};
  Tag tag_ {Tag::ext};

public:
  // For string-like types — zero allocation
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
  template<string_like T>
    requires(!std::same_as<std::remove_cvref_t<T>, std::string>)
  constexpr explicit string_holder(T&& value) noexcept
      : ext_(std::string_view(std::forward<T>(value)))
  {
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)

  // For const string refs — zero allocation, just view
  constexpr explicit string_holder(const std::string& s) noexcept
      : ext_(s)
  {
  }

  // For rvalue strings — take ownership
  explicit string_holder(std::string&& s) noexcept
      : owned_(std::move(s))
      , tag_(Tag::owned)
  {
  }

  // Fast path for integral and integral-convertible types.
  // Uses compile-time cache for 0–1023, stack buffer otherwise.
  template<typename T>
    requires(!string_like<T> && integral_convertible<T>)
  explicit string_holder(const T& value) noexcept
  {
    const auto ival = static_cast<std::int64_t>(value);
    if (auto sv = cached_int_view(ival)) {
      ext_ = *sv;
      tag_ = Tag::ext;
      return;
    }
    tag_ = Tag::buf;
    auto* const begin = int_buf_.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto* const end = begin + int_buf_.size();
    const auto [ptr, ec] = std::to_chars(begin, end, ival);
    int_len_ = static_cast<std::uint8_t>(ptr - begin);
  }

  // Fallback for non-string, non-integral types (floating point, custom
  // formatters) — allocates via std::format.
  template<typename T>
    requires(!string_like<T> && !integral_convertible<T>)
  explicit string_holder(const T& value)
      : owned_(std::format("{}", value))
      , tag_(Tag::owned)
  {
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept
  {
    switch (tag_) {
      case Tag::buf:
        return {int_buf_.data(), int_len_};
      case Tag::owned:
        return owned_;
      case Tag::ext:
        return ext_;
    }
    return {};
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

/**
 * @brief Clears and writes a label into an existing string buffer
 *
 * @tparam sep Separator character (default '_')
 * @return constexpr void
 *
 * @note Zero-argument overload — simply clears the buffer
 */
template<char sep = '_'>
constexpr void as_label_into(std::string& result) noexcept
{
  result.clear();
}

/**
 * @brief Writes a concatenated label into an existing string buffer
 *
 * Like as_label(), but reuses the buffer's existing capacity to avoid
 * repeated heap allocations across calls.  The buffer is cleared (not
 * deallocated) on each call, so its capacity grows monotonically to
 * the high-water mark.
 *
 * @tparam sep Separator character between components (default '_')
 * @tparam Args Argument types (automatically deduced)
 * @param result String buffer to write into (cleared, not deallocated)
 * @param args Values to concatenate into label
 */
template<char sep = '_', typename... Args>
void as_label_into(std::string& result, Args&&... args) noexcept(
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

  result.clear();  // Keeps existing capacity
  if (size.total == 0) [[unlikely]] {
    return;
  }
  result.reserve(size.total);

  // Build the result string
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
}

}  // namespace gtopt
