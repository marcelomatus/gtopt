// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Marcelo Matus. All rights reserved.
//
// test_power_bound_threshold.cpp — exercises
// `PlanningLP::validate_power_bounds` and
// `PlanningLP::validate_line_transmission`.
//
// Both validators rewrite any power/transmission bound whose raw MW value is
// nonzero but below the fixed 1e-4 MW threshold to scalar 0.0 (clamping
// offending entries inside 2-D vector schedules in place), so the LP-assembly
// zero-column/row skip can drop the dead column/row.  This file checks:
//   1. scalar tiny pmax/pmin → 0;
//   2. scalar ≥ 1e-4 pmax/pmin left untouched;
//   3. 2-D vector schedule: per-entry clamp (tiny → 0, normal kept);
//   4. line tmax_ab/tmax_ba scalar tiny → 0, ≥ 1e-4 kept;
//   5. line 2-D vector schedule per-entry clamp;
//   6. explicit zero and unset bounds left alone.

#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/basic_types.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/line.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace
{

[[nodiscard]] auto sched_scalar(const OptTBRealFieldSched& opt) -> double
{
  REQUIRE(opt.has_value());
  REQUIRE(std::holds_alternative<double>(*opt));
  return std::get<double>(*opt);
}

[[nodiscard]] auto sched_matrix(const OptTBRealFieldSched& opt)
    -> std::vector<std::vector<double>>
{
  REQUIRE(opt.has_value());
  REQUIRE(std::holds_alternative<std::vector<std::vector<double>>>(*opt));
  return std::get<std::vector<std::vector<double>>>(*opt);
}

[[nodiscard]] auto make_planning(Array<Generator> gens, Array<Line> lines)
    -> Planning
{
  Planning planning;
  planning.system.generator_array = std::move(gens);
  planning.system.line_array = std::move(lines);
  return planning;
}

}  // namespace

TEST_CASE("validate_power_bounds clamps tiny scalar generator bounds to zero")
{
  Array<Generator> gens = {
      {
          .uid = Uid {1},
          .name = "tiny_pmax",
          .bus = Uid {1},
          .pmin = 1e-5,
          .pmax = 1e-5,
      },
      {
          .uid = Uid {2},
          .name = "normal",
          .bus = Uid {1},
          .pmin = 0.5,
          .pmax = 100.0,
      },
  };
  auto planning = make_planning(std::move(gens), {});
  PlanningLP::validate_power_bounds(planning);

  // Tiny (1e-5 < 1e-4) → 0.
  CHECK(sched_scalar(planning.system.generator_array[0].pmax) == 0.0);
  CHECK(sched_scalar(planning.system.generator_array[0].pmin) == 0.0);
  // Normal (≥ 1e-4) untouched.
  CHECK(sched_scalar(planning.system.generator_array[1].pmax)
        == doctest::Approx(100.0));
  CHECK(sched_scalar(planning.system.generator_array[1].pmin)
        == doctest::Approx(0.5));
}

TEST_CASE("validate_power_bounds keeps a bound exactly at the threshold")
{
  // 1e-4 is NOT below 1e-4 (strict <) → keep.
  Array<Generator> gens = {
      {
          .uid = Uid {1},
          .name = "at_threshold",
          .bus = Uid {1},
          .pmax = 1e-4,
      },
  };
  auto planning = make_planning(std::move(gens), {});
  PlanningLP::validate_power_bounds(planning);
  CHECK(sched_scalar(planning.system.generator_array[0].pmax)
        == doctest::Approx(1e-4));
}

TEST_CASE("validate_power_bounds clamps offending entries in 2-D schedules")
{
  // 2-D (stage, block) schedule: only some entries below threshold.
  Array<Generator> gens = {
      {
          .uid = Uid {1},
          .name = "mixed",
          .bus = Uid {1},
          .pmax =
              std::vector<std::vector<double>> {{100.0, 1e-6}, {5e-5, 50.0}},
      },
  };
  auto planning = make_planning(std::move(gens), {});
  PlanningLP::validate_power_bounds(planning);
  const auto mat = sched_matrix(planning.system.generator_array[0].pmax);
  REQUIRE(mat.size() == 2);
  REQUIRE(mat[0].size() == 2);
  REQUIRE(mat[1].size() == 2);
  CHECK(mat[0][0] == doctest::Approx(100.0));
  CHECK(mat[0][1] == 0.0);
  CHECK(mat[1][0] == 0.0);
  CHECK(mat[1][1] == doctest::Approx(50.0));
}

TEST_CASE("validate_power_bounds leaves explicit-zero and unset bounds alone")
{
  Array<Generator> gens = {
      {
          .uid = Uid {1},
          .name = "explicit_zero",
          .bus = Uid {1},
          .pmax = 0.0,
      },
      {
          .uid = Uid {2},
          .name = "unset",
          .bus = Uid {1},
      },
  };
  auto planning = make_planning(std::move(gens), {});
  PlanningLP::validate_power_bounds(planning);
  CHECK(sched_scalar(planning.system.generator_array[0].pmax) == 0.0);
  CHECK_FALSE(planning.system.generator_array[1].pmax.has_value());
  CHECK_FALSE(planning.system.generator_array[1].pmin.has_value());
}

TEST_CASE("validate_line_transmission clamps tiny scalar line bounds to zero")
{
  Array<Line> lines = {
      {
          .uid = Uid {1},
          .name = "tiny_tmax",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.01,
          .tmax_ba = 5e-5,
          .tmax_ab = 5e-5,
      },
      {
          .uid = Uid {2},
          .name = "normal",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.01,
          .tmax_ba = 100.0,
          .tmax_ab = 200.0,
      },
  };
  auto planning = make_planning({}, std::move(lines));
  PlanningLP::validate_line_transmission(planning);

  CHECK(sched_scalar(planning.system.line_array[0].tmax_ab) == 0.0);
  CHECK(sched_scalar(planning.system.line_array[0].tmax_ba) == 0.0);
  CHECK(sched_scalar(planning.system.line_array[1].tmax_ab)
        == doctest::Approx(200.0));
  CHECK(sched_scalar(planning.system.line_array[1].tmax_ba)
        == doctest::Approx(100.0));
}

TEST_CASE("validate_line_transmission clamps offending entries in 2-D schedule")
{
  Array<Line> lines = {
      {
          .uid = Uid {1},
          .name = "mixed",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.01,
          .tmax_ab = std::vector<std::vector<double>> {{300.0, 9e-5}, {150.0}},
      },
  };
  auto planning = make_planning({}, std::move(lines));
  PlanningLP::validate_line_transmission(planning);
  const auto mat = sched_matrix(planning.system.line_array[0].tmax_ab);
  REQUIRE(mat.size() == 2);
  REQUIRE(mat[0].size() == 2);
  CHECK(mat[0][0] == doctest::Approx(300.0));
  CHECK(mat[0][1] == 0.0);
  REQUIRE(mat[1].size() == 1);
  CHECK(mat[1][0] == doctest::Approx(150.0));
}

// NOLINTEND(bugprone-unchecked-optional-access)
