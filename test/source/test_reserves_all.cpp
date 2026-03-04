/**
 * @file      test_reserves_all.cpp
 * @brief     Consolidated reserve zone and provision test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for reserve zone and reserve provision
 * into a single compilation unit to reduce build time.
 */

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

#include "test_reserve_provision.hpp"
#include "test_reserve_zone.hpp"
