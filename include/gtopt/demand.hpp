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
  OptTRealFieldSched ecost {}

  GTOPT_DEMAND_ATTRS;
  GTOPT_CAPACITY_ATTRS;
};

struct Demand
{
  GTOPT_OBJECT_ATTRS;
  GTOPT_DEMAND_ATTRS;
  GTOPT_CAPACITY_ATTRS;

  auto& set_attrs(auto&& attrs)
  {
    bus = std::exchange(attrs.bus, {});
    lmax = std::move(attrs.lmax);
    lossfactor = std::move(attrs.lossfactor);
    fcost = std::move(attrs.fcost);
    emin = std::move(attrs.emin);
    ecost = std::move(attrs.ecost);

    capacity = std::move(attrs.capacity);
    expcap = std::move(attrs.expcap);
    expmod = std::move(attrs.expmod);
    capmax = std::move(attrs.capmax);
    annual_capcost = std::move(attrs.annual_capcost);
    annual_derating = std::move(attrs.annual_derating);

    return *this;
  }
};

using DemandVar = std::variant<Uid, Name, DemandAttrs>;
using OptDemandVar = std::optional<DemandVar>;

}  // namespace gtopt
