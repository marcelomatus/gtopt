/**
 * @file      generator_profile.hpp
 * @brief     Header of
 * @date      Tue Apr  1 21:20:35 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/generator.hpp>

namespace gtopt
{

struct GeneratorProfile
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  GeneratorVar generator {};
  STBRealFieldSched profile {};
  OptTRealFieldSched scost {};
};

}  // namespace gtopt
