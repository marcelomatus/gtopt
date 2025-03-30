/**
 * @file      capacity.hpp
 * @brief     Header of
 * @date      Thu Mar 27 10:45:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

struct CapacityAttrs
{
#define GTOPT_CAPACITY_ATTRS \
  OptTRealFieldSched capacity {}; \
  OptTRealFieldSched expcap {}; \
  OptTRealFieldSched expmod {}; \
  OptTRealFieldSched capmax {}; \
  OptTRealFieldSched annual_capcost {}; \
  OptTRealFieldSched annual_derating {}

  GTOPT_CAPACITY_ATTRS;
};

struct Capacity : Object
{
  GTOPT_OBJECT_ATTRS;
  GTOPT_CAPACITY_ATTRS;
};

}  // namespace gtopt
