/**
 * @file      filtration.hpp
 * @brief     Header of
 * @date      Thu Jul 31 23:22:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

struct Filtration
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  SingleId waterway {unknown_uid};
  SingleId reservoir {unknown_uid};
  Real slope {};
  Real constant {};
};

}  // namespace gtopt
