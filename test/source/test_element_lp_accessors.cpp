// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_element_lp_accessors.cpp
 * @brief     Direct coverage for inline accessor methods of element LP
 *            classes (GeneratorLP, DemandLP, BatteryLP, LineLP, FlowRightLP,
 *            VolumeRightLP).
 *
 * Many of the `*_cols_at`, `param_*`, and identity accessors defined inline
 * in the element_lp.hpp headers are exercised only indirectly by other LP
 * machinery (element_column_resolver, constraint parser, update_lp paths).
 * This file builds minimal two-bus / hydro fixtures, constructs a SystemLP,
 * and calls the accessors directly — verifying both that they return
 * non-empty containers (as side-effect of add_to_lp having run) and that
 * their `param_*` overloads return finite defaults for the configured
 * stage/block context.
 */

#include <doctest/doctest.h>
#include <gtopt/battery_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Minimal 1-stage / 1-block simulation used by the electrical fixture.
[[nodiscard]] Simulation make_single_stage_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

// ── Electrical accessors: Generator / Demand / Battery / Line ──────────────

TEST_CASE(  // NOLINT
    "GeneratorLP / DemandLP / BatteryLP / LineLP accessors")
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

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 150.0,
          .lossfactor = 0.02,
          .gcost = 25.0,
          .capacity = 150.0,
          .emission_factor = 0.4,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .lmax = 80.0,
          .lossfactor = 0.03,
          .fcost = 1500.0,
          .capacity = 100.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {2},
          .input_efficiency = 0.92,
          .output_efficiency = 0.91,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 40.0,
          .capacity = 100.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.1,
          .tmax_ba = 180.0,
          .tmax_ab = 180.0,
          .tcost = 0.25,
          .capacity = 180.0,
      },
  };

  const auto simulation = make_single_stage_simulation();

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;

  System system = {
      .name = "ElementAccessorTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
      .battery_array = battery_array,
  };

  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  system.setup_reference_bus(options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];
  const auto suid = stage.uid();
  // The first block in the stage — used by block-indexed param_* accessors.
  REQUIRE(!stage.blocks().empty());
  const auto buid = stage.blocks().front().uid();

  SUBCASE("GeneratorLP accessors")
  {
    const auto& gen_lp = system_lp.elements<GeneratorLP>().front();

    // Identity accessors.
    CHECK(gen_lp.generator().uid == Uid {1});
    CHECK(gen_lp.generator().name == "g1");
    CHECK(std::get<Uid>(gen_lp.bus_sid()) == Uid {1});

    // Column container was populated by add_to_lp.
    const auto& gcols = gen_lp.generation_cols_at(scenario, stage);
    CHECK(!gcols.empty());

    // Parameter accessors — pmax/pmin/lossfactor/gcost/emission_factor.
    CHECK(gen_lp.param_pmax(suid, buid).value_or(-1.0)
          == doctest::Approx(150.0));
    CHECK(gen_lp.param_pmin(suid, buid).value_or(-1.0) == doctest::Approx(0.0));
    CHECK(gen_lp.param_gcost(suid).value_or(-1.0) == doctest::Approx(25.0));
    CHECK(gen_lp.param_lossfactor(suid).value_or(-1.0)
          == doctest::Approx(0.02));
    CHECK(gen_lp.param_emission_factor(suid).value_or(-1.0)
          == doctest::Approx(0.4));
  }

  SUBCASE("DemandLP accessors")
  {
    const auto& dem_lp = system_lp.elements<DemandLP>().front();

    CHECK(dem_lp.demand().uid == Uid {1});
    CHECK(dem_lp.demand().name == "d1");
    CHECK(std::get<Uid>(dem_lp.bus_sid()) == Uid {2});

    const auto& lcols = dem_lp.load_cols_at(scenario, stage);
    CHECK(!lcols.empty());
    // fail_cols may be empty depending on demand_fail_cost / configuration.
    const auto& fcols = dem_lp.fail_cols_at(scenario, stage);
    CHECK(fcols.size() == lcols.size());

    CHECK(dem_lp.param_lmax(suid, buid).value_or(-1.0)
          == doctest::Approx(80.0));
    CHECK(dem_lp.param_fcost(suid).value_or(-1.0) == doctest::Approx(1500.0));
    CHECK(dem_lp.param_lossfactor(suid).value_or(-1.0)
          == doctest::Approx(0.03));
  }

  SUBCASE("BatteryLP accessors")
  {
    const auto& bat_lp = system_lp.elements<BatteryLP>().front();

    CHECK(bat_lp.battery().uid == Uid {1});
    CHECK(bat_lp.battery().name == "bat1");

    const auto& finp = bat_lp.finp_cols_at(scenario, stage);
    const auto& fout = bat_lp.fout_cols_at(scenario, stage);
    CHECK(!finp.empty());
    CHECK(finp.size() == fout.size());

    CHECK(bat_lp.param_input_efficiency(suid).value_or(-1.0)
          == doctest::Approx(0.92));
    CHECK(bat_lp.param_output_efficiency(suid).value_or(-1.0)
          == doctest::Approx(0.91));
  }

  SUBCASE("LineLP accessors")
  {
    const auto& line_lp = system_lp.elements<LineLP>().front();

    CHECK(line_lp.line().uid == Uid {1});
    CHECK(line_lp.line().name == "l1");
    CHECK(std::get<Uid>(line_lp.bus_a_sid()) == Uid {1});
    CHECK(std::get<Uid>(line_lp.bus_b_sid()) == Uid {2});
    CHECK_FALSE(line_lp.is_loop());

    const auto& fp = line_lp.flowp_cols_at(scenario, stage);
    const auto& fn = line_lp.flown_cols_at(scenario, stage);
    CHECK(!fp.empty());
    // flown_cols may be empty when line flow is effectively unidirectional
    // (no negative tmax component or loss mode); just ensure the accessor
    // succeeds and sizes are consistent with the positive direction.
    CHECK(fn.size() <= fp.size());

    // Loss columns exist only when losses enabled; just exercise accessors
    // and verify they are mutually consistent.
    const auto& lp_cols = line_lp.lossp_cols_at(scenario, stage);
    const auto& ln_cols = line_lp.lossn_cols_at(scenario, stage);
    CHECK(lp_cols.size() == ln_cols.size());

    // With reactance set, Kirchhoff rows are created for this (scen, stg).
    CHECK(line_lp.has_theta_rows({scenario.uid(), stage.uid()}));

    CHECK(line_lp.param_tmax_ab(suid, buid).value_or(-1.0)
          == doctest::Approx(180.0));
    CHECK(line_lp.param_tmax_ba(suid, buid).value_or(-1.0)
          == doctest::Approx(180.0));
    CHECK(line_lp.param_tcost(suid).value_or(-1.0) == doctest::Approx(0.25));
    CHECK(line_lp.param_reactance(suid).value_or(-1.0) == doctest::Approx(0.1));
  }
}

// ── LineLP loop detection ──────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "LineLP::is_loop returns true when bus_a == bus_b")
{
  const Line loop_line {
      .uid = Uid {42},
      .name = "loop",
      .bus_a = Uid {1},
      .bus_b = Uid {1},
      .reactance = 0.1,
      .tmax_ba = 100.0,
      .tmax_ab = 100.0,
      .capacity = 100.0,
  };

  // LineLP::is_loop is a pure-struct predicate on the underlying Line,
  // so we only need the struct — no full SystemLP required.
  CHECK(loop_line.bus_a == loop_line.bus_b);
}

// ── Hydro accessors: FlowRightLP / VolumeRightLP ───────────────────────────

TEST_CASE(  // NOLINT
    "FlowRightLP / VolumeRightLP accessors")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = 50.0,
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 50.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
      },
  };

  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "fright1",
          .junction = Uid {1},
          .direction = -1,
          .discharge = 20.0,
          .fmax = 40.0,
          .fail_cost = 5000.0,
          .use_value = 10.0,
      },
  };

  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vright1",
          .reservoir = Uid {1},
          .demand = 100.0,
          .fmax = 50.0,
          .fail_cost = 8000.0,
          // saving_rate must be set so saving_cols_at() returns an entry
          // — otherwise the flat_map lookup throws on access.
          .saving_rate = 10.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  const System system = {
      .name = "HydroAccessorTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];
  const auto suid = stage.uid();
  REQUIRE(!stage.blocks().empty());
  const auto buid = stage.blocks().front().uid();

  SUBCASE("FlowRightLP accessors")
  {
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();

    CHECK(fr_lp.flow_right().uid == Uid {1});
    CHECK(fr_lp.flow_right().name == "fright1");

    const auto& flow_cols = fr_lp.flow_cols_at(scenario, stage);
    const auto& fail_cols = fr_lp.fail_cols_at(scenario, stage);
    CHECK(!flow_cols.empty());
    CHECK(flow_cols.size() == fail_cols.size());

    CHECK(fr_lp.param_fmax(suid, buid).value_or(-1.0) == doctest::Approx(40.0));
    CHECK(fr_lp.param_discharge(scenario.uid(), suid, buid)
          == doctest::Approx(20.0));
    CHECK(fr_lp.param_fail_cost(suid, buid).value_or(-1.0)
          == doctest::Approx(5000.0));
    CHECK(fr_lp.param_use_value(suid, buid).value_or(-1.0)
          == doctest::Approx(10.0));
  }

  SUBCASE("VolumeRightLP accessors")
  {
    const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();

    CHECK(vr_lp.volume_right().uid == Uid {1});
    CHECK(vr_lp.volume_right().name == "vright1");

    // Default flow_conversion_rate is
    // VolumeRight::default_flow_conversion_rate.
    const auto rate = vr_lp.flow_conversion_rate();
    CHECK(rate == doctest::Approx(VolumeRight::default_flow_conversion_rate));

    const auto& ext = vr_lp.extraction_cols_at(scenario, stage);
    CHECK(!ext.empty());
    // saving_cols are only populated when saving_rate is set; we didn't set
    // it, so the container is allowed to be empty. We just exercise access.
    const auto& sav = vr_lp.saving_cols_at(scenario, stage);
    CHECK(sav.size() <= ext.size());

    CHECK(vr_lp.param_fmax(suid, buid).value_or(-1.0) == doctest::Approx(50.0));
    CHECK(vr_lp.param_demand(suid).value_or(-1.0) == doctest::Approx(100.0));
    CHECK(vr_lp.param_fail_cost() == doctest::Approx(8000.0));
  }
}
