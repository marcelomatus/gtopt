#pragma once

#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

struct Converter
{
  Uid uid {};
  Name name {};
  OptActive active {};

  SingleId battery {};

  /**
   * You must specify a 'bus_generator' or a 'generator'. If you specify the
   *  bus_generator, an auxiliary generator will be created using the same
   *  converter uid and name.
   */
  OptSingleId bus_generator {};
  OptGeneratorVar generator {};

  /**
   * You must specify a 'bus_demand' or a 'demand'. If you specify the
   *  bus_demand, an auxiliary demand will be created using the same
   *  converter uid and name.
   */
  OptSingleId bus_demand {};
  OptDemandVar demand {};

  /**
   * The lossfactor will be used in both auxiliary generator and demand created
   * when you use define the bus_generator and bus_demand
   */
  OptTRealFieldSched lossfactor {};

  OptTRealFieldSched conversion_rate {};

  OptTRealFieldSched capacity {};
  OptTRealFieldSched expcap {};
  OptTRealFieldSched expmod {};
  OptTRealFieldSched capmax {};
  OptTRealFieldSched annual_capcost {};
  OptTRealFieldSched annual_derating {};
};

}  // namespace gtopt
