/**
 * @file      test_sddp_all.cpp
 * @brief     Aggregation unit for SDDP solver tests
 * @date      2026-03-08
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <fstream>
#include <sstream>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_monitor.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

// clang-format off
// test_sddp_solver.hpp must come first: it defines make_3phase_hydro_planning()
// and make_single_phase_planning() used by test_sddp_cut_io.hpp.
#include "test_benders_cut.hpp"
#include "test_strong_uid_index.hpp"
#include "test_sddp_solver.hpp"
#include "test_sddp_cut_io.hpp"
#include "test_sddp_monitor.hpp"
#include "test_sddp_aperture_functions.hpp"
// clang-format on
