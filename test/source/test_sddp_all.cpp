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
#include <gtopt/cascade_method.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_clone_pool.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_monitor.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/validate_planning.hpp>
#include <spdlog/spdlog.h>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

// clang-format off
// test_sddp_method.hpp must come first: it defines make_3phase_hydro_planning()
// and make_single_phase_planning() used by test_sddp_cut_io.hpp.
#include "test_benders_cut.hpp"
#include "test_strong_uid_index.hpp"
#include "test_sddp_method.hpp"
#include "test_sddp_cut_io.hpp"
#include "test_sddp_state_io.hpp"
#include "test_sddp_monitor.hpp"
#include "test_sddp_aperture_functions.hpp"
#include "test_sddp_pool.hpp"
#include "test_sddp_clone_pool.hpp"
#include "test_sddp_cut_sharing.hpp"
#include "test_sddp_feasibility.hpp"
#include "test_cascade_method.hpp"
#include "test_convergence_mode.hpp"
#include "test_monolithic_method.hpp"
// clang-format on
