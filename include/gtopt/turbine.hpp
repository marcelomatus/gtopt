/**
 * @file      turbine.hpp
 * @brief     Header of
 * @date      Thu Jul 31 01:50:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

struct Turbine
{
  Uid uid {};
  Name name {};
  OptActive active {};

  SingleId waterway {};
  SingleId generator {};

  OptTRealFieldSched conversion_rate {};
  OptTRealFieldSched capacity {};
};

}  // namespace gtopt
