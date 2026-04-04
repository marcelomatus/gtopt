/**
 * @file      test_main_all.cpp
 * @brief     Consolidated gtopt_main, work pool, and benchmark map test
 * compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for gtopt_main, work pool/CPU monitor,
 * and flat_map benchmark into a single compilation unit to reduce build time.
 */

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <doctest/doctest.h>
#include <gtopt/cpu_monitor.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/work_pool.hpp>

using namespace gtopt;

#include "test_benchmark_map.hpp"
#include "test_cpu_monitor.hpp"
#include "test_gtopt_main.hpp"
#include "test_lp_debug_writer.hpp"
#include "test_set_cli_option.hpp"
#include "test_solver_registry.hpp"
#include "test_work_pool.hpp"
#include "test_work_pool_coverage.hpp"
