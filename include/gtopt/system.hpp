/**c
 * @file      system.hpp<gtopt>
 * @brief     Header of System class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the System class, which contains all the system elements.
 */

#pragma once

#include <gtopt/block.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/line.hpp>
#include <gtopt/scenery.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_options.hpp>

#include "gtopt/basic_types.hpp"
#include "gtopt/battery.hpp"
#include "gtopt/converter.hpp"
#include "gtopt/reserve_provision.hpp"
#include "gtopt/reserve_zone.hpp"

namespace gtopt
{

struct System
{
  Name name {};
  String version {};

  SystemOptions options {};

  Array<Block> block_array {};
  Array<Stage> stage_array {};
  Array<Scenery> scenery_array {};

  Array<Bus> bus_array {};
  Array<Demand> demand_array {};
  Array<Generator> generator_array {};
  Array<Line> line_array {};

  Array<GeneratorProfile> generator_profile_array {};
  Array<DemandProfile> demand_profile_array {};

  Array<Battery> battery_array {};
  Array<Converter> converter_array {};

  Array<ReserveZone> reserve_zone_array {};
  Array<ReserveProvision> reserve_provision_array {};

  System& merge(System& sys);
};

}  // namespace gtopt
