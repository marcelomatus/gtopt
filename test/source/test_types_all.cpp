/**
 * @file      test_types_all.cpp
 * @brief     Consolidated basic-types and utility test compilation unit
 * @date      2026-03-04
 * @copyright BSD-3-Clause
 *
 * Includes individual test .hpp files for basic types, utilities, and
 * flat-map helpers into a single compilation unit to reduce build time
 * while keeping each topic in its own file.
 */

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/error.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/overload.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/utils.hpp>

using namespace gtopt;

#include "test_basic_types.hpp"
#include "test_cost_helper.hpp"
#include "test_fmap.hpp"
#include "test_label_maker.hpp"
#include "test_overload.hpp"
#include "test_single_id.hpp"
#include "test_strong_index_vector.hpp"
#include "test_utils.hpp"
