/**
 * @file      test_schedule_io_all.cpp
 * @brief     Consolidated schedule, I/O, and output context test compilation
 * unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for schedules (CSV and Parquet),
 * Arrow/Parquet I/O, and output context into a single compilation unit
 * to reduce build time.
 */

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/uididx_traits.hpp>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/exception.h>
#include <zlib.h>

using namespace gtopt;

#include "test_input_traits.hpp"
#include "test_output_context.hpp"
#include "test_parquet.hpp"
#include "test_schedule.hpp"
#include "test_schedule_csv.hpp"
