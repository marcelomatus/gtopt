/**
 * @file      bus.hpp
 * @brief     Header of
 * @date      Tue Mar 18 13:31:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Bus class, which defines an electric Busbar.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>

namespace gtopt
{

struct BusAttrs
{
#define GTOPT_BUS_ATTRS \
  OptReal voltage {}; \
  OptReal reference_theta {}; \
  OptBool use_kirchhoff {}

  GTOPT_BUS_ATTRS;
};

struct Bus
{
  Uid uid {};
  Name name {};
  OptActive active {};

  GTOPT_BUS_ATTRS;

  [[nodiscard]] auto id() const -> Id { return {uid, name}; }

  [[nodiscard]] constexpr bool needs_kirchhoff(const double v_threshold) const
  {
    return use_kirchhoff.value_or(true) && voltage.value_or(1) > v_threshold;
  }
};

}  // namespace gtopt
