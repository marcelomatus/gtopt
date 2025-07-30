/**
 * @file      waterway.hpp
 * @brief     Header of
 * @date      Wed Jul 30 11:40:33 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{
struct Waterway
{
  Uid uid {};
  Name name {};
  OptActive active {};

  SingleId junction_a {};
  SingleId junction_b {};

  OptTRealFieldSched capacity {};
  OptTRealFieldSched lossfactor {};

  OptTBRealFieldSched fmin {};
  OptTBRealFieldSched fmax {};
};

}  // namespace gtopt
