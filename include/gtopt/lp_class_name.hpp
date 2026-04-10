/**
 * @file      lp_class_name.hpp
 * @brief     Defines the LPClassName struct with modern C++23 features
 * @date      Wed Aug 06 15:10:00 2025
 * @author    ai-developer
 * @copyright BSD-3-Clause
 *
 * This header defines the LPClassName struct used to hold the full class
 * name for LP object types.  The legacy abbreviated short name was
 * removed — `short_name()` now returns a lazy lowercase view of the
 * full name via `gtopt::lowercase()`.
 */
#pragma once

#include <format>
#include <string_view>

#include <gtopt/as_label.hpp>

namespace gtopt
{

struct LPClassName
{
  explicit constexpr LPClassName(std::string_view pfull_name) noexcept
      : m_full_name(pfull_name)
  {
  }

  [[nodiscard]] constexpr std::string_view full_name() const noexcept
  {
    return m_full_name;
  }

  /// Lowercase view of the full class name.  Returns a lazy, zero-copy
  /// `LowercaseView` suitable for direct use in `as_label(...)`.  Callers
  /// that need a materialized `std::string` should wrap this in
  /// `as_label(lowercase(...))` or build a `std::string` from the view.
  [[nodiscard]] constexpr auto short_name() const noexcept
  {
    return lowercase(m_full_name);
  }

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  constexpr operator std::string_view() const noexcept { return m_full_name; }

private:
  std::string_view m_full_name;
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
