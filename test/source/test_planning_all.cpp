/**
 * @file      test_planning_all.cpp
 * @brief     Consolidated IEEE planning integration test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for IEEE 4-bus, 9-bus, 14-bus, and
 * 14-bus original planning tests into a single compilation unit to reduce
 * build time.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

#include "test_demand_emin_planning.hpp"
#include "test_ieee14_planning.hpp"
#include "test_ieee14b_ori_planning.hpp"
#include "test_ieee4b_ori_planning.hpp"
#include "test_ieee9b_ori_planning.hpp"
#include "test_unified_battery_planning.hpp"
#include "test_user_constraint_planning.hpp"
#include "test_validate_planning.hpp"
