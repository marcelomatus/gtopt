/**
 * @file      lp_class_name.hpp
 * @brief     Defines the LPClassName struct with modern C++23 features
 * @date      Wed Aug 06 15:10:00 2025
 * @author    ai-developer
 * @copyright BSD-3-Clause
 *
 * This header defines the LPClassName struct used to hold both the full and
 * short names for LP object classes with C++23 features.
 */
#pragma once

#include <format>
#include <string_view>

namespace gtopt
{

struct LPClassName
{
  explicit constexpr LPClassName(std::string_view pfull_name) noexcept
      : m_full_name(pfull_name)
      , m_short_name(pfull_name)
  {
  }

  explicit constexpr LPClassName(std::string_view pfull_name,
                                 std::string_view pshort_name) noexcept
      : m_full_name(pfull_name)
      , m_short_name(pshort_name)
  {
  }

  [[nodiscard]] constexpr std::string_view full_name() const noexcept
  {
    return m_full_name;
  }
  [[nodiscard]] constexpr std::string_view short_name() const noexcept
  {
    return m_short_name;
  }

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  constexpr operator std::string_view() const noexcept { return m_full_name; }

private:
  std::string_view m_full_name;
  std::string_view m_short_name;
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
