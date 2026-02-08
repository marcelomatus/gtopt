/**
 * @file      overload.hpp
 * @brief     Overload pattern for combining multiple callables
 * @date      Sun Mar 23 21:08:27 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the Overload helper for std::visit and similar patterns.
 * C++20 aggregate CTAD eliminates the need for an explicit deduction guide.
 */

#pragma once

namespace gtopt
{
template<typename... Ts>
struct Overload : Ts...
{
  using Ts::operator()...;
};
}  // namespace gtopt
