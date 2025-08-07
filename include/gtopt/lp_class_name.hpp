/**
 * @file      lp_class_name.hpp
 * @brief     Defines the LPClassName struct
 * @date      Wed Aug 06 15:10:00 2025
 * @author    ai-developer
 * @copyright BSD-3-Clause
 *
 * This header defines the LPClassName struct used to hold both the full and
 * short names for LP object classes.
 */
#pragma once

#include <format>
#include <string_view>

namespace gtopt
{
struct LPClassName : std::string_view
{
  [[nodiscard]] constexpr const std::string_view& full_name() const noexcept
  {
    return *this;
  }

  [[nodiscard]] constexpr const std::string_view& short_name() const noexcept
  {
    return m_short_name;
  }

  constexpr LPClassName(std::string_view pfull_name,
                        std::string_view pshort_name) noexcept
      : std::string_view(pfull_name)
      , m_short_name(pshort_name)
  {
  }

private:
  std::string_view m_short_name;
};

}  // namespace gtopt

// Specialize std::formatter for LPClassName
namespace std
{
template<>
struct formatter<gtopt::LPClassName> : formatter<std::string_view>
{
  auto format(const gtopt::LPClassName& name, format_context& ctx) const
  {
    return formatter<std::string_view>::format(name.full_name(), ctx);
  }
};
}  // namespace std
