/**
 * @file      lp_class_name.hpp
 * @brief     Defines the LPClassName struct with modern C++23 features
 * @date      Wed Aug 06 15:10:00 2025
 * @author    ai-developer
 * @copyright BSD-3-Clause
 *
 * This header defines the LPClassName struct used to hold the full class
 * name for LP object types.  `short_name()` returns a `std::string_view`
 * over the snake_case form of the full PascalCase name, materialized
 * once into a fixed buffer at constexpr construction time
 * (e.g. `"ReserveProvision" → "reserve_provision"`).
 */
#pragma once

#include <array>
#include <format>
#include <string_view>

#include <gtopt/as_label.hpp>

namespace gtopt
{

struct LPClassName
{
  /// Maximum characters in the materialized snake_case short name.
  /// Longest current name: "ReservoirProductionFactor" → 27 chars.
  static constexpr std::size_t max_short_len = 48;

  /// Default constructor yields an empty class name (both full_name()
  /// and snake_case() return empty views).  Required so that
  /// aggregate types like `StateVariable::Key` (which stores an
  /// `LPClassName` by value) can be default-initialised — the empty
  /// instance signals "unset" and comparisons against a live class
  /// name reliably fail.
  constexpr LPClassName() noexcept = default;

  explicit constexpr LPClassName(std::string_view pfull_name) noexcept
      : m_full_name(pfull_name)
      , m_short_len(detail::snake_case_size(pfull_name))
  {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < pfull_name.size(); ++i) {
      if (i > 0 && detail::snake_needs_underscore_before(pfull_name, i)) {
        m_short_buf[pos++] = '_';
      }
      m_short_buf[pos++] = detail::to_lower_char(pfull_name[i]);
    }
  }

  [[nodiscard]] constexpr bool empty() const noexcept
  {
    return m_full_name.empty();
  }

  [[nodiscard]] constexpr auto operator<=>(
      const LPClassName& other) const noexcept
  {
    return m_full_name <=> other.m_full_name;
  }

  /// Heterogeneous equality against any string-like (string_view /
  /// std::string / const char*).  Compares the PascalCase
  /// `full_name()`.  Lets call sites compare a stored `LPClassName`
  /// against a string literal without explicit conversion — e.g.
  /// `key.class_name == "Reservoir"` works.
  [[nodiscard]] constexpr bool operator==(std::string_view other) const noexcept
  {
    return m_full_name == other;
  }

  [[nodiscard]] constexpr bool operator==(
      const LPClassName& other) const noexcept
  {
    return m_full_name == other.m_full_name;
  }

  [[nodiscard]] constexpr std::string_view full_name() const noexcept
  {
    return m_full_name;
  }

  /// snake_case of the full class name, materialized once at construction.
  /// Returns a `std::string_view` over the internal buffer — zero-cost
  /// to call and usable anywhere a `string_view` is expected.
  /// `"ReserveProvision" → "reserve_provision"`,
  /// `"Generator" → "generator"`, etc.
  [[nodiscard]] constexpr std::string_view short_name() const noexcept
  {
    return {m_short_buf.data(), m_short_len};
  }

  /// Alias for `short_name()`.
  [[nodiscard]] constexpr std::string_view snake_case() const noexcept
  {
    return short_name();
  }

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  constexpr operator std::string_view() const noexcept { return m_full_name; }

private:
  std::string_view m_full_name;
  std::array<char, max_short_len> m_short_buf {};  ///< materialized snake_case
  std::size_t m_short_len {0};
};

}  // namespace gtopt

// Specialize std::formatter for LPClassName
namespace std
{
template<>
struct formatter<gtopt::LPClassName> : formatter<string_view>
{
  constexpr auto parse(format_parse_context& ctx)
  {
    return formatter<string_view>::parse(ctx);
  }

  template<typename FormatContext>
  auto format(const gtopt::LPClassName& name, FormatContext& ctx) const
  {
    return formatter<string_view>::format(name.full_name(), ctx);
  }
};
}  // namespace std
