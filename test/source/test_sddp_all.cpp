/**
 * @file      test_sddp_all.cpp
 * @brief     Aggregation unit for SDDP solver tests
 * @date      2026-03-08
 * @copyright BSD-3-Clause
 */

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

#include "test_sddp_solver.hpp"
