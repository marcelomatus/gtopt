/**
 * @file      test_check_solvers_all.cpp
 * @brief     Compilation unit for check_solvers tests
 * @date      2026-03-26
 * @copyright BSD-3-Clause
 *
 * Aggregates the check_solvers test header into a single translation unit.
 */

#include <string>

#include <doctest/doctest.h>
#include <gtopt/check_solvers.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;

#include "test_check_solvers.hpp"
