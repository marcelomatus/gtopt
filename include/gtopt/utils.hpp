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
  return fmt::format("{}", arg);
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

}  // namespace gtopt
