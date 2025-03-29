/**
 * @file      demand.hpp
 * @brief     Header of
 * @date      Thu Mar 27 09:12:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

struct DemandAttrs
{
#define GTOPT_DEMAND_ATTRS \
  SingleId bus {}; \
  OptTBRealFieldSched lmax {}; \
  OptTRealFieldSched lossfactor {}; \
  OptTRealFieldSched fcost {}; \
  OptTRealFieldSched emin {}; \
  OptTRealFieldSched ecost {}; \
  GTOPT_CAPACITY_ATTRS

  GTOPT_DEMAND_ATTRS;
};

struct Demand
{
  Uid uid {};
  Name name {};
  OptActive active {};

  GTOPT_DEMAND_ATTRS;
};

using DemandVar = std::variant<Uid, Name, DemandAttrs>;
using OptDemandVar = std::optional<DemandVar>;

}  // namespace gtopt
