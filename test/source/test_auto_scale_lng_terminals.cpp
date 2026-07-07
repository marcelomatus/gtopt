// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_auto_scale_lng_terminals.cpp
 * @brief     Unit tests for PlanningLP::auto_scale_lng_terminals
 * @date      2026-05-12
 *
 * Tests:
 *   1. auto_scale=false kill switch → no scales produced
 *   2. Empty lng_terminal_array → no-op
 *   3. LngTerminal with emax ≤ 1000 → no scale (sub-threshold)
 *   4. LngTerminal with emax = 50000 → scale = 100 (10^ceil(log10(50)))
 *   5. LngTerminal with infinity-sentinel emax → no scale
 *   6. LngTerminal without emax → skipped
 *   7. LngTerminal with explicit VariableScale entry → skipped
 *   8. Multiple LNG terminals → multiple scale entries
 *   9. Idempotent: empty re-call is no-op
 *  10. Stage-indexed emax (vector) → max element used
 */

#include <doctest/doctest.h>
#include <gtopt/lng_terminal.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/variable_scale.hpp>

using namespace gtopt;
// NOLINTBEGIN(readability-redundant-casting)

namespace
{

/// Build a minimal Planning for LNG auto-scale testing.
[[nodiscard]] Planning make_lng_planning(const Array<LngTerminal>& terminals,
                                         bool auto_scale = true)
{
  Planning planning;
  planning.system.lng_terminal_array = terminals;
  planning.options.model_options.auto_scale = auto_scale;
  return planning;
}

/// Count VariableScale entries matching a given class_name.
[[nodiscard]] auto count_scales(const PlanningOptions& opts,
                                const LPClassName& class_name) -> size_t
{
  return std::ranges::count_if(opts.variable_scales,
                               [&](const VariableScale& vs)
                               { return vs.class_name == class_name; });
}

}  // namespace

// The constructor uses SolverRegistry to get solver_infinity, but the
// static method accepts any double.  We pass a value large enough to
// distinguish real data from sentinels.
constexpr double kTestInfinity = 1e30;

TEST_CASE("auto_scale_lng_terminals — auto_scale=false kill switch")
{
  auto planning = make_lng_planning(
      {
          {
              .uid = Uid {1},
              .name = "lng1",
              .emax = static_cast<double>(50'000),
          },
      },
      /*auto_scale=*/false);

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE("auto_scale_lng_terminals — empty terminal array is no-op")
{
  auto planning = make_lng_planning({});

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE("auto_scale_lng_terminals — emax ≤ 1000 is sub-threshold")
{
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_small",
          .emax = static_cast<double>(500),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE("auto_scale_lng_terminals — emax = 1000 is sub-threshold")
{
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_exact_1000",
          .emax = static_cast<double>(1000),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  // emax == 1000 is NOT > 1000, so it's skipped
  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE("auto_scale_lng_terminals — emax = 50000 → scale = 100")
{
  // emax = 50000, raw = 50000/1000 = 50
  // scale = 10^ceil(log10(50)) = 10^ceil(1.699..) = 10^2 = 100
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng1",
          .emax = static_cast<double>(50'000),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  REQUIRE(count_scales(planning.options, LngTerminal::class_name) == 1);
  const auto& scale = planning.options.variable_scales.back();
  CHECK(scale.uid == Uid {1});
  CHECK(scale.name == "lng1");
  CHECK(scale.variable == "energy");
  CHECK(scale.scale == doctest::Approx(100.0));
}

TEST_CASE("auto_scale_lng_terminals — emax = 2e6 → scale = 1000")
{
  // emax = 2,000,000, raw = 2000000/1000 = 2000
  // scale = 10^ceil(log10(2000)) = 10^ceil(3.301..) = 10^4 = 10000
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_big",
          .emax = static_cast<double>(2'000'000),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  REQUIRE(count_scales(planning.options, LngTerminal::class_name) == 1);
  CHECK(planning.options.variable_scales.back().scale
        == doctest::Approx(10'000.0));
}

TEST_CASE("auto_scale_lng_terminals — infinity-sentinel emax skipped")
{
  // emax = 1e30 is a sentinel for "no bound" — must not produce a scale
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_inf",
          .emax = static_cast<double>(1e30),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE(
    "auto_scale_lng_terminals — emax sentinel near solver_infinity is "
    "skipped")
{
  // Use a custom infinity value: emax >= 5e4 is treated as infinity
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_sentinel",
          .emax = static_cast<double>(1e20),
      },
  });

  // solver_infinity=5e4 makes 1e20 >= 5e4 → treated as infinity
  PlanningLP::auto_scale_lng_terminals(planning, /*solver_infinity=*/5e4);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE("auto_scale_lng_terminals — no emax field → skipped")
{
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_no_emax",
          // emax left unset
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 0);
}

TEST_CASE("auto_scale_lng_terminals — explicit VariableScale entry → skipped")
{
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng1",
          .emax = static_cast<double>(100'000),
      },
  });
  // Pre-populate with explicit scale for uid=1
  planning.options.variable_scales.push_back(VariableScale {
      .class_name {LngTerminal::class_name},
      .variable {"energy"},
      .uid = Uid {1},
      .scale = 42.0,
      .name {"lng1"},
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  // Should still have exactly 1 scale entry (the explicit one, not 2)
  CHECK(count_scales(planning.options, LngTerminal::class_name) == 1);
  CHECK(planning.options.variable_scales[0].scale == doctest::Approx(42.0));
}

TEST_CASE(
    "auto_scale_lng_terminals — multiple terminals produce multiple "
    "scales")
{
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_a",
          .emax = static_cast<double>(5'000),  // raw=5 → scale=10
      },
      {
          .uid = Uid {2},
          .name = "lng_b",
          .emax = static_cast<double>(500'000),  // raw=500 → scale=1000
      },
      {
          .uid = Uid {3},
          .name = "lng_c",
          .emax = static_cast<double>(50'000),  // raw=50 → scale=100
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  CHECK(count_scales(planning.options, LngTerminal::class_name) == 3);

  // Verify UIDs and scales
  bool found_1 = false;
  bool found_2 = false;
  bool found_3 = false;
  for (const auto& vs : planning.options.variable_scales) {
    if (vs.uid == Uid {1}) {
      found_1 = true;
      CHECK(vs.scale == doctest::Approx(10.0));
    }
    if (vs.uid == Uid {2}) {
      found_2 = true;
      CHECK(vs.scale == doctest::Approx(1000.0));
    }
    if (vs.uid == Uid {3}) {
      found_3 = true;
      CHECK(vs.scale == doctest::Approx(100.0));
    }
  }
  CHECK(found_1);
  CHECK(found_2);
  CHECK(found_3);
}

TEST_CASE("auto_scale_lng_terminals — idempotent on empty re-call")
{
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng1",
          .emax = static_cast<double>(50'000),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);
  const auto count_after_first =
      count_scales(planning.options, LngTerminal::class_name);

  // Second call: has_entry now finds the first call's entry
  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);
  CHECK(count_scales(planning.options, LngTerminal::class_name)
        == count_after_first);
}

TEST_CASE("auto_scale_lng_terminals — stage-indexed emax uses max element")
{
  // emax as a 2-D (per-(stage, block)) schedule: scale uses the max of
  // ALL block entries across all stages.  Mirrors the per-stage list
  // form (encoded as Mx1 inner here).
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_vector",
          .emax =
              std::vector<std::vector<Real>> {
                  {
                      10'000,
                  },
                  {
                      50'000,
                  },
                  {
                      30'000,
                  },
              },
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  REQUIRE(count_scales(planning.options, LngTerminal::class_name) == 1);
  // max = 50000, raw = 50000/1000 = 50
  // scale = 10^ceil(log10(50)) = 100
  CHECK(planning.options.variable_scales.back().scale
        == doctest::Approx(100.0));
}

TEST_CASE("auto_scale_lng_terminals — emax exactly at threshold boundary")
{
  // emax = 1001 → raw = 1.001, scale = 10^ceil(log10(1.001)) = 10
  auto planning = make_lng_planning({
      {
          .uid = Uid {1},
          .name = "lng_boundary",
          .emax = static_cast<double>(1'001),
      },
  });

  PlanningLP::auto_scale_lng_terminals(planning, kTestInfinity);

  REQUIRE(count_scales(planning.options, LngTerminal::class_name) == 1);
  CHECK(planning.options.variable_scales.back().scale == doctest::Approx(10.0));
}

// NOLINTEND(readability-redundant-casting)