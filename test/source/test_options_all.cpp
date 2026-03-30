/**
 * @file      test_options_all.cpp
 * @brief     Consolidated options and CLI test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for options (PlanningOptions,
 * PlanningOptionsLP, SolverOptions), CLI options, main options, and all
 * sub-option structs (ModelOptions, SddpOptions, MonolithicOptions,
 * CascadeOptions, VariableScaleMap) into a single compilation unit to reduce
 * build time.
 */

#include <array>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/cascade_options.hpp>
#include <gtopt/cli_options.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/model_options.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/variable_scale.hpp>

using namespace gtopt;

#include "test_cascade_options.hpp"
#include "test_cli_options.hpp"
#include "test_config_file.hpp"
#include "test_main_options.hpp"
#include "test_model_options.hpp"
#include "test_monolithic_options.hpp"
#include "test_options.hpp"
#include "test_scale_alpha.hpp"
#include "test_sddp_options.hpp"
#include "test_solver_options.hpp"
#include "test_variable_scale_map.hpp"
