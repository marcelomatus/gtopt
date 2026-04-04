/**
 * @file      test_components_all.cpp
 * @brief     Consolidated power-system component test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for bus, generator, generator profile,
 * demand, and line into a single compilation unit to reduce build time.
 */

#include <filesystem>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/as_label.hpp>
#include <gtopt/block.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;

#include "test_bus.hpp"
#include "test_demand.hpp"
#include "test_generator.hpp"
#include "test_generator_profile.hpp"
#include "test_hot_uncovered.hpp"
#include "test_line.hpp"
#include "test_line_losses.hpp"
