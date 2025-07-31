/**
 * @file      flow.hpp
 * @brief     Header of
 * @date      Wed Jul 30 15:52:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

struct Flow
{
  Uid uid {};
  Name name {};
  OptActive active {};

  Int direction {1};  // Indicates if it is an input(1) or output(-1) flow

  SingleId junction {};
  STBRealFieldSched discharge {};
};

}  // namespace gtopt
