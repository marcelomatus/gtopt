/**
 * @file      overload.hpp
 * @brief     Header of
 * @date      Sun Mar 23 21:08:27 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

namespace gtopt
{
template<typename... Ts>
struct Overload : Ts...
{
  using Ts::operator()...;
};
template<class... Ts>
Overload(Ts...) -> Overload<Ts...>;
}  // namespace gtopt
