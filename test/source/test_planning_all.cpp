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

#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/array.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/variable_scale.hpp>

using namespace gtopt;

#include "test_demand_emin_planning.hpp"
#include "test_element_column_resolver.hpp"
#include "test_ieee14_planning.hpp"
#include "test_ieee14b_ori_planning.hpp"
#include "test_ieee4b_ori_planning.hpp"
#include "test_ieee9b_equilibration.hpp"
#include "test_ieee9b_ori_planning.hpp"
#include "test_scale_lp_effects.hpp"
#include "test_unified_battery_planning.hpp"
#include "test_user_constraint_planning.hpp"
#include "test_validate_planning.hpp"
