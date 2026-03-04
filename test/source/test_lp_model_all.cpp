/**
 * @file      test_lp_model_all.cpp
 * @brief     Consolidated LP time/phase/system model test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for time LP, phase LP, and system LP
 * into a single compilation unit to reduce build time.
 */

#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/element_index.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

#include "test_phase_lp.hpp"
#include "test_system_lp.hpp"
#include "test_time_lp.hpp"
