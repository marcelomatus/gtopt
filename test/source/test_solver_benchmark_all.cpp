/**
 * @file      test_solver_benchmark_all.cpp
 * @brief     Compilation unit for solver benchmark tests
 * @date      2026-03-27
 * @copyright BSD-3-Clause
 */

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

#include "test_solver_benchmark.hpp"
