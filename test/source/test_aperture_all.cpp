/**
 * @file      test_aperture_all.cpp
 * @brief     Aggregation unit for aperture LP update tests
 * @date      2026-03-17
 * @copyright BSD-3-Clause
 *
 * Tests FlowLP::update_aperture_lp, Aperture struct/JSON, and the
 * aperture scenario file mechanism used in the SDDP backward pass.
 */

#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/aperture.hpp>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/json/json_aperture.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

#include "test_aperture_data_cache.hpp"
#include "test_aperture_lp.hpp"
