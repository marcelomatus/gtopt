/**
 * @file      test_simulation_all.cpp
 * @brief     Consolidated simulation LP, planning, and flat helper test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for simulation LP, planning, and flat
 * helper into a single compilation unit to reduce build time.
 */

#include <filesystem>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

#include "test_flat_helper.hpp"
#include "test_planning.hpp"
#include "test_simulation_lp.hpp"
