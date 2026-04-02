/**
 * @file      test_irrigation_all.cpp
 * @brief     Consolidated water rights test compilation unit
 * @date      2026-04-01
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for water rights data structs
 * and LP integration into a single compilation unit.
 */

#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right.hpp>

using namespace gtopt;

#include "test_irrigation_data.hpp"
#include "test_irrigation_lp.hpp"
