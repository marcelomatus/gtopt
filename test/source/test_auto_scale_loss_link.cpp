// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for `PlanningLP::auto_scale_loss_link` — a power-of-10 row scale
// for the line-loss linking constraint computed from `median(R/V²)`.
//
// Mirrors the structure of the `auto_scale_theta` tests in
// `test_planning.cpp`.

#include <doctest/doctest.h>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

constexpr Simulation make_one_block_sim()
{
  return Simulation {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
}

}  // namespace

TEST_CASE("PlanningLP - auto_scale_loss_link picks median R/V² as power of 10")
{
  // Three lines: (R, V) so R/V² spans 1e-6, 1e-5, 1e-4 → median = 1e-5
  //   l1: R=4.84,    V=220 → R/V² = 1.0e-4
  //   l2: R=0.484,   V=220 → R/V² = 1.0e-5  (median)
  //   l3: R=0.0484,  V=220 → R/V² = 1.0e-6
  // Expected scale = 10^round(−log10(1e-5)) = 10^5 = 100000
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
      {
          .uid = Uid {3},
          .name = "b3",
      },
      {
          .uid = Uid {4},
          .name = "b4",
      },
  };
  const Array<Generator> gen_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> dem_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {4},
          .capacity = 80.0,
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .resistance = 4.84,
          .reactance = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "l2",
          .bus_a = Uid {2},
          .bus_b = Uid {3},
          .voltage = 220.0,
          .resistance = 0.484,
          .reactance = 0.10,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {3},
          .name = "l3",
          .bus_a = Uid {3},
          .bus_b = Uid {4},
          .voltage = 220.0,
          .resistance = 0.0484,
          .reactance = 0.20,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "AutoScaleLossLinkTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  Planning planning {
      .options =
          {
              .demand_fail_cost = 1000.0,
          },
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  REQUIRE(planning.options.model_options.scale_loss_link.has_value());
  CHECK(*planning.options.model_options.scale_loss_link
        == doctest::Approx(1.0e5));
}

TEST_CASE("PlanningLP - auto_scale_loss_link respects explicit override")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  const Array<Generator> gen_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> dem_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 80.0,
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .resistance = 0.484,
          .reactance = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "ExplicitScaleLossLinkTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  ModelOptions mo;
  mo.scale_loss_link = 7.0;
  Planning planning {
      .options =
          {
              .demand_fail_cost = 1000.0,
              .model_options = mo,
          },
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  REQUIRE(planning.options.model_options.scale_loss_link.has_value());
  CHECK(*planning.options.model_options.scale_loss_link
        == doctest::Approx(7.0));
}

TEST_CASE("PlanningLP - auto_scale_loss_link skipped when auto_scale=false")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .resistance = 0.484,
          .reactance = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "NoAutoScaleTest",
      .bus_array = bus_array,
      .line_array = line_array,
  };

  ModelOptions mo;
  mo.auto_scale = false;
  Planning planning {
      .options =
          {
              .model_options = mo,
          },
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  CHECK_FALSE(planning.options.model_options.scale_loss_link.has_value());
}

TEST_CASE("PlanningLP - auto_scale_loss_link no-op when no resistance data")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  // No resistance set → loop finds nothing → option stays unset.
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "NoResistanceTest",
      .bus_array = bus_array,
      .line_array = line_array,
  };

  Planning planning {
      .options = {},
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  CHECK_FALSE(planning.options.model_options.scale_loss_link.has_value());
}

TEST_CASE("PlanningLP - auto_scale_loss_link skipped under single_bus")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {1},
          .voltage = 220.0,
          .resistance = 0.484,
          .reactance = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "SingleBusLossTest",
      .bus_array = bus_array,
      .line_array = line_array,
  };

  Planning planning {
      .options =
          {
              .use_single_bus = true,
          },
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  CHECK_FALSE(planning.options.model_options.scale_loss_link.has_value());
}

TEST_CASE("PlanningLP - auto_scale_loss_link respects --no-scale precondition")
{
  // `--no-scale` (in main_options.hpp) sets `auto_scale=false` AND
  // `scale_loss_link=1.0`.  Verify both knobs are honoured: explicit
  // 1.0 short-circuits the auto-compute (covered by the override test
  // already), and `auto_scale=false` alone also short-circuits even
  // when scale_loss_link is unset.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .resistance = 0.484,
          .reactance = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "NoScaleCombo",
      .bus_array = bus_array,
      .line_array = line_array,
  };

  // Mirror what main_options.hpp does for --no-scale:
  ModelOptions mo;
  mo.auto_scale = false;
  mo.scale_loss_link = 1.0;  // explicit no-op set by --no-scale path
  Planning planning {
      .options =
          {
              .model_options = mo,
          },
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  // Explicit value preserved, NOT overwritten.
  REQUIRE(planning.options.model_options.scale_loss_link.has_value());
  CHECK(*planning.options.model_options.scale_loss_link
        == doctest::Approx(1.0));
}

TEST_CASE(
    "PlanningLP - auto_scale_loss_link no lift when median R/V² already O(1)")
{
  // Per-unit data: V defaults to 1.0, R = 0.5 → R/V² = 0.5 → already O(1).
  // round(−log10(0.5)) = round(0.301) = 0  → scale = 1.0 → option stays unset
  // (we treat scale == 1.0 as "no lift needed").
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .resistance = 0.5,
          .reactance = 0.1,
          .tmax_ba = 1.0,
          .tmax_ab = 1.0,
          .capacity = 1.0,
      },
  };

  const System system {
      .name = "PuRangeTest",
      .bus_array = bus_array,
      .line_array = line_array,
  };

  Planning planning {
      .options = {},
      .simulation = make_one_block_sim(),
      .system = system,
  };

  PlanningLP::auto_scale_loss_link(planning);

  CHECK_FALSE(planning.options.model_options.scale_loss_link.has_value());
}
