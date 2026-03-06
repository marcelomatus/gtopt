/**
 * @file      test_battery_all.cpp
 * @brief     Consolidated battery, converter, and state variable test
 * compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for battery, converter, and state
 * variable into a single compilation unit to reduce build time.
 */

#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/block.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

#include "test_battery.hpp"
#include "test_battery_expand.hpp"
#include "test_converter.hpp"
#include "test_state_variable.hpp"
