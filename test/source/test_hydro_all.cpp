/**
 * @file      test_hydro_all.cpp
 * @brief     Consolidated hydro system test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for hydro data (junction, reservoir,
 * turbine, waterway), hydro LP, filtration, and reservoir efficiency into
 * a single compilation unit to reduce build time.
 */

#include <vector>

#include <doctest/doctest.h>
#include <gtopt/filtration.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_efficiency.hpp>
#include <gtopt/reservoir_efficiency_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway.hpp>

using namespace gtopt;

#include "test_filtration.hpp"
#include "test_hydro_data.hpp"
#include "test_hydro_lp.hpp"
#include "test_reservoir_efficiency.hpp"
