#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

struct LineAttrs
{
  SingleId bus_a {};
  SingleId bus_b {};
  OptTRealFieldSched voltage {};
  OptTRealFieldSched resistance {};
  OptTRealFieldSched reactance {};
  OptTRealFieldSched lossfactor {};
  OptTBRealFieldSched tmin {};
  OptTBRealFieldSched tmax {};
  OptTRealFieldSched tcost {};
};

struct Line
{
  Uid uid {};
  Name name {};
  OptActive active {};

  SingleId bus_a {};
  SingleId bus_b {};
  OptTRealFieldSched voltage {};
  OptTRealFieldSched resistance {};
  OptTRealFieldSched reactance {};
  OptTRealFieldSched lossfactor {};
  OptTBRealFieldSched tmin {};
  OptTBRealFieldSched tmax {};
  OptTRealFieldSched tcost {};

  OptTRealFieldSched capacity {};
  OptTRealFieldSched expcap {};
  OptTRealFieldSched expmod {};
  OptTRealFieldSched capmax {};
  OptTRealFieldSched annual_capcost {};
  OptTRealFieldSched annual_derating {};
};

}  // namespace gtopt
