/**
 * @file      test_hydro_all.cpp
 * @brief     Consolidated hydro system test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for hydro data (junction, reservoir,
 * turbine, waterway), hydro LP, seepage, and reservoir efficiency into
 * a single compilation unit to reduce build time.
 */

#include <filesystem>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/flow.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/reservoir_production_factor_lp.hpp>
#include <gtopt/reservoir_seepage.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway.hpp>

using namespace gtopt;

#include "test_expand_reservoir_constraints.hpp"
#include "test_flow.hpp"
#include "test_flow_discharge_lp.hpp"
#include "test_flow_right.hpp"
#include "test_hydro_data.hpp"
#include "test_hydro_lp.hpp"
#include "test_reservoir_discharge_limit.hpp"
#include "test_reservoir_production_factor.hpp"
#include "test_reservoir_seepage.hpp"
#include "test_right_bound_rule.hpp"
#include "test_turbine_lp.hpp"
