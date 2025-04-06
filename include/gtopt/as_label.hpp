/**
 * @file      as_label.hpp
 * @brief     Header of
 * @date      Sat Apr  5 23:56:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gtopt
{

// Base case - empty parameter pack
template<char sep = '_'>
constexpr std::string as_label()
{
  return {};
}

namespace detail
{
// String view conversion traits
template<typename T>
concept DirectStringViewable = std::is_convertible_v<const T&, std::string_view>
    && !std::is_convertible_v<const T&, std::string>;

// Safe string view generator that owns temporaries
class StringViewOrOwner
{
  std::string_view view;
  std::unique_ptr<std::string> owner;

public:
  // Handle string types efficiently
  explicit StringViewOrOwner(const std::string& s) noexcept
      : view(s)
  {
  }
  explicit StringViewOrOwner(std::string&& s)
      : owner(std::make_unique<std::string>(std::move(s)))
  {
    view = *owner;
  }

  explicit StringViewOrOwner(const std::string_view& s) noexcept
      : view(s)
  {
  }
  explicit StringViewOrOwner(const char* s) noexcept
      : view(s)
  {
  }

  // Handle directly convertible types
  template<DirectStringViewable T>
  explicit StringViewOrOwner(const T& value) noexcept
      : view(static_cast<std::string_view>(value))
  {
  }

  // Handle other types via format
  template<typename T>
    requires(!DirectStringViewable<T>)
  explicit StringViewOrOwner(const T& value)
      : owner(std::make_unique<std::string>(std::format("{}", value)))
  {
    view = *owner;
  }

  // Access the view
  explicit operator std::string_view() const noexcept { return view; }

  [[nodiscard]] std::string_view get() const noexcept { return view; }
};

// Calculate required size and whether we need separators
struct SizeCalculation
{
  size_t total = 0;
  bool needs_sep = false;

  [[nodiscard]] constexpr SizeCalculation add(std::string_view view) const
  {
    if (view.empty()) {
      return *this;
    }
    return {.total = total + (needs_sep ? 1 : 0) + view.size(),
            .needs_sep = true};
  }
};
}  // namespace detail

// Main concatenation function
template<char sep = '_', typename... Args>
std::string as_label(Args&&... args)
{
  // Convert and own all arguments
  std::array<detail::StringViewOrOwner, sizeof...(Args)> owners {
      detail::StringViewOrOwner(std::forward<Args>(args))...};

  // Calculate total size needed
  detail::SizeCalculation sc {};
  for (const auto& owner : owners) {
    sc = sc.add(owner.get());
  }

  // Handle empty case
  if (sc.total == 0) {
    return {};
  }

  // Build the result with a single allocation
  std::string result;
  result.reserve(sc.total);

  bool needs_separator = false;
  for (const auto& owner : owners) {
    auto view = owner.get();
    if (view.empty()) {
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
