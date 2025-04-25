/**
 * @file      as_label.hpp
 * @brief     Header of
 * @date      Sat Apr  5 23:56:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */
#pragma once

#include <array>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gtopt
{

template<char sep = '_'>
constexpr std::string as_label()
{
  return {};
}

namespace detail
{

template<typename T>
concept direct_string_viewable =
    std::is_convertible_v<const T&, std::string_view>
    && !std::is_convertible_v<const T&, std::string>;

class string_view_or_owner
{
  std::string_view view;
  std::unique_ptr<std::string> owner;

public:
  // For lvalue strings - view first, then owner (empty)
  constexpr explicit string_view_or_owner(const std::string& s) noexcept
      : view(s)
      , owner(nullptr)
  {
  }

  // For rvalue strings - create owner first, then view from it
  constexpr explicit string_view_or_owner(std::string&& s)
      : view()
      , owner(std::make_unique<std::string>(std::move(s)))
  {
    view = *owner;
  }

  // For string_views - view first, owner empty
  constexpr explicit string_view_or_owner(const std::string_view& s) noexcept
      : view(s)
      , owner(nullptr)
  {
  }

  // For C strings - view first, owner empty
  constexpr explicit string_view_or_owner(const char* s) noexcept
      : view(s)
      , owner(nullptr)
  {
  }

  // For directly convertible types - view first, owner empty
  template<direct_string_viewable T>
  constexpr explicit string_view_or_owner(const T& value) noexcept
      : view(static_cast<std::string_view>(value))
      , owner(nullptr)
  {
  }

  // For other types - create owner first, then view from it
  template<typename T>
    requires(!direct_string_viewable<T>)
  constexpr explicit string_view_or_owner(const T& value)
      : view()
      , owner(std::make_unique<std::string>(std::format("{}", value)))
  {
    view = *owner;
  }

  constexpr explicit operator std::string_view() const noexcept { return view; }

  [[nodiscard]] std::string_view get() const noexcept { return view; }
};

struct size_calculation
{
  size_t total = 0;
  bool needs_sep = false;

  [[nodiscard]] constexpr size_calculation add(std::string_view view) const
  {
    if (view.empty()) [[unlikely]] {
      return *this;
    }
    return {.total = total + (needs_sep ? 1 : 0) + view.size(),
            .needs_sep = true};
  }
};

}  // namespace detail

template<char sep = '_', typename... Args>
constexpr std::string as_label(Args&&... args)
{
  std::array<detail::string_view_or_owner, sizeof...(Args)> owners {
      detail::string_view_or_owner(std::forward<Args>(args))...};

  detail::size_calculation sc {};
  for (const auto& owner : owners) {
    sc = sc.add(owner.get());
  }

  if (sc.total == 0) [[unlikely]] {
    return {};
  }

  std::string result;
  result.reserve(sc.total);

  bool needs_separator = false;
  for (const auto& owner : owners) {
    const auto view = owner.get();
    if (view.empty()) [[unlikely]] {
      continue;
    }
    if (needs_separator) {
      result.push_back(sep);
    }
    result.append(view);
    needs_separator = true;
  }

  return result;
}

}  // namespace gtopt
