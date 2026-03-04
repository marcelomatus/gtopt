/**
 * @file      test_options_all.cpp
 * @brief     Consolidated options and CLI test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for options (Options, OptionsLP,
 * SolverOptions), CLI options, and application options into a single
 * compilation unit to reduce build time.
 */

#include <array>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/app_options.hpp>
#include <gtopt/cli_options.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;

#include "test_app_options.hpp"
#include "test_cli_options.hpp"
#include "test_options.hpp"
#include "test_solver_options.hpp"
