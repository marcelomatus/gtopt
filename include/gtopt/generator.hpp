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

  auto& set_attrs(auto&& attrs)
  {
    bus = std::exchange(attrs.bus, {});
    pmin = std::move(attrs.pmin);
    pmax = std::move(attrs.pmax);
    lossfactor = std::move(attrs.lossfactor);
    gcost = std::move(attrs.gcost);

    capacity = std::move(attrs.capacity);
    expcap = std::move(attrs.expcap);
    expmod = std::move(attrs.expmod);
    capmax = std::move(attrs.capmax);
    annual_capcost = std::move(attrs.annual_capcost);
    annual_derating = std::move(attrs.annual_derating);

    return *this;
  }
};

using GeneratorVar = std::variant<Uid, Name, GeneratorAttrs>;
using OptGeneratorVar = std::optional<GeneratorVar>;

}  // namespace gtopt
