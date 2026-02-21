/**
 * @file      test_json_all.cpp
 * @brief     JSON serialization/deserialization tests - includes all .ipp test
 * files
 * @date      2026-02-21
 * @copyright BSD-3-Clause
 *
 * Includes individual JSON test .ipp files into a single compilation unit
 * to reduce build time while keeping each type in its own file.
 */

#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_demand_profile.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_filtration.hpp>
#include <gtopt/json/json_flow.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_generator_profile.hpp>
#include <gtopt/json/json_junction.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/json/json_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json/json_reserve_provision.hpp>
#include <gtopt/json/json_reserve_zone.hpp>
#include <gtopt/json/json_reservoir.hpp>
#include <gtopt/json/json_simulation.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/json/json_turbine.hpp>
#include <gtopt/json/json_waterway.hpp>
#include <gtopt/object.hpp>
#include <gtopt/reservoir.hpp>

using namespace gtopt;

#include "test_basic_types_json.ipp"
#include "test_bus_json.ipp"
#include "test_demand_json.ipp"
#include "test_demand_profile_json.ipp"
#include "test_filtration_json.ipp"
#include "test_flow_json.ipp"
#include "test_generator_json.ipp"
#include "test_generator_profile_json.ipp"
#include "test_junction_json.ipp"
#include "test_line_json.ipp"
#include "test_optimization_json.ipp"
#include "test_options_json.ipp"
#include "test_planning_json.ipp"
#include "test_reserve_provision_json.ipp"
#include "test_reserve_zone_json.ipp"
#include "test_reservoir_json.ipp"
#include "test_simulation_json.ipp"
#include "test_system_json.ipp"
#include "test_turbine_json.ipp"
#include "test_waterway_json.ipp"
