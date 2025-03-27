/**
 * @file      utils.hpp
 * @brief     Header of
 * @date      Sat Mar 22 22:12:49 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <format>
#include <ranges>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

template<char sep = '_'>
constexpr auto as_label() -> std::string
{
  return {};
}

constexpr auto as_string(const auto& arg) -> std::string
{
  return std::format("{}", arg);
}

template<typename Arg>
concept string_convertable = requires(Arg v) {
  {
    v
  } -> std::convertible_to<std::string>;
};

constexpr auto as_string(const string_convertable auto& arg) -> std::string
{
  return {arg};
}

constexpr auto&& as_string(std::string&& arg)
{
  return arg;
}

template<char sep = '_', typename T, typename... Types>
constexpr auto as_label(const T& var1, const Types&... var2)

{
  auto&& sbeg = as_string(var1);
  auto&& send = as_label<sep>(var2...);

  if (sbeg.empty()) {
    return send;
  }

  if (send.empty()) {
    return sbeg;
  }

  std::string sres;
  sres.reserve(sbeg.size() + 1 + send.size());
  sres += sbeg;
  sres += sep;
  sres += send;

  return sres;
}

template<typename IndexType = size_t, typename Range>
constexpr auto enumerate(const Range& range)
{
  return std::ranges::views::zip(
      std::ranges::views::iota(0)
          | std::ranges::views::transform(
              [](size_t i) { return static_cast<IndexType>(i); }),
      range);
}

template<typename IndexType = size_t, typename Range, typename Op>
constexpr auto enumerate_if(const Range& range, Op op)
{
  const auto op_second = [&](auto&& p) { return op(std::get<1>(p)); };
  return enumerate<IndexType>(range) | std::ranges::views::filter(op_second);
}

constexpr auto active_fnc = [](auto&& e) { return e.is_active(); };
constexpr auto true_fnc = [](auto&&) { return true; };

template<typename IndexType = size_t, typename Range>
constexpr auto enumerate_active(const Range& range)
{
  return enumerate_if<IndexType>(range, active_fnc);
}

template<typename Range>
constexpr auto active(const Range& range)
{
  return range | std::ranges::views::filter(active_fnc);
}

template<typename T, typename K = typename T::key_type>
constexpr auto get_optiter(const T& map, K&& key)
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it)> {it} : std::nullopt;
}

template<typename T, typename K = typename T::key_type>
constexpr auto get_optvalue(const T& map, K&& key)
{
  const auto it = map.find(std::forward<K>(key));
  return (it != map.end()) ? std::optional<decltype(it->second)> {it->second}
                           : std::nullopt;
}

template<typename T, typename K>
constexpr auto get_optvalue_optkey(const T& map, const std::optional<K>& key)
{
  return key.has_value() ? get_optvalue(map, key.value()) : std::nullopt;
}

}  // namespace gtopt
