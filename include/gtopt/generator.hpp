/**
 * @file      generator.hpp
 * @brief     Header of
 * @date      Sat Mar 29 11:52:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

struct GeneratorAttrs
{
#define GTOPT_GENERATOR_ATTRS \
  SingleId bus {}; \
  OptTBRealFieldSched pmin {}; \
  OptTBRealFieldSched pmax {}; \
  OptTRealFieldSched lossfactor {}; \
  OptTRealFieldSched gcost {}

  GTOPT_GENERATOR_ATTRS;
  GTOPT_CAPACITY_ATTRS;
};

struct Generator
{
  GTOPT_OBJECT_ATTRS;
  GTOPT_GENERATOR_ATTRS;
  GTOPT_CAPACITY_ATTRS;
};

using GeneratorVar = std::variant<Uid, Name, GeneratorAttrs>;
using OptGeneratorVar = std::optional<GeneratorVar>;

}  // namespace gtopt
