/**
 * @file      test_array_all.cpp
 * @brief     Consolidated array/index/collection test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for array index traits, collections,
 * element indices, UID index traits, multi-vector traits, and multi-array
 * into a single compilation unit to reduce build time.
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/block.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/element_index.hpp>
#include <gtopt/multi_array_2d.hpp>
#include <gtopt/mvector_traits.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/uididx_traits.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;

#include "test_array_index_traits.hpp"
#include "test_collection.hpp"
#include "test_element_index.hpp"
#include "test_multi_array_2d.hpp"
#include "test_mvector_traits.hpp"
#include "test_uididx_traits.hpp"
