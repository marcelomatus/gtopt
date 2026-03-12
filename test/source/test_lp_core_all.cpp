/**
 * @file      test_lp_core_all.cpp
 * @brief     Consolidated LP core test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for sparse columns/rows, linear
 * problem, parser, and interface into a single compilation unit to reduce
 * build time.
 */

#include <expected>
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_parser.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;

#include "test_constraint_parser.hpp"
#include "test_linear_interface.hpp"
#include "test_linear_parser.hpp"
#include "test_linear_problem.hpp"
#include "test_sparse_col.hpp"
#include "test_sparse_row.hpp"
