#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

struct LineAttrs
{
#define GTOPT_LINE_ATTRS \
  SingleId bus_a {}; \
  SingleId bus_b {}; \
  OptTRealFieldSched voltage {}; \
  OptTRealFieldSched resistance {}; \
  OptTRealFieldSched reactance {}; \
  OptTRealFieldSched lossfactor {}; \
  OptTBRealFieldSched tmin {}; \
  OptTBRealFieldSched tmax {}; \
  OptTRealFieldSched tcost {}

  GTOPT_LINE_ATTRS;
};

struct Line
{
  GTOPT_OBJECT_ATTRS;
  GTOPT_LINE_ATTRS;
  GTOPT_CAPACITY_ATTRS;
};

}  // namespace gtopt
