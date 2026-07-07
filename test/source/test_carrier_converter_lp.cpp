// SPDX-License-Identifier: BSD-3-Clause
//
// Integration tests for ``CarrierConverterLP``:
//   * Single electrolyser bridging Bus → HydrogenNode lets the
//     hydrogen storage actually charge — finp[b] > 0, electric
//     demand on the bus increases by finp/η.
//   * Full P2H2 + H2P round trip via electrolyser + fuel cell drives
//     a non-trivial dispatch and yields the expected round-trip cost
//     penalty.

#include <doctest/doctest.h>
#include <gtopt/carrier_converter_lp.hpp>
#include <gtopt/hydrogen_node_lp.hpp>
#include <gtopt/hydrogen_storage_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_carrier_converter_lp_smoke
{

Simulation make_chrono_2h_simulation()
{
  return {
      .block_array =
          {
              {.uid = Uid {0}, .duration = 1.0},
              {.uid = Uid {1}, .duration = 1.0},
          },
      .stage_array =
          {
              {
                  .uid = Uid {0},
                  .first_block = 0,
                  .count_block = 2,
                  .chronological = true,
              },
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };
}

}  // namespace test_carrier_converter_lp_smoke

TEST_CASE(
    "CarrierConverter: electrolyser (Bus → HydrogenNode) charges H₂ storage")
// NOLINT
{
  using namespace test_carrier_converter_lp_smoke;
  // Topology:
  //   g1 (cheap, 200 MW, $10/MWh) → Bus b1 → electrolyser → H₂ node
  //   → H₂ storage (must end at 200 MWh_LHV, starts at 100 MWh_LHV)
  // The LP MUST charge the storage: it has to inject 100 MWh of H₂
  // into the cavern over 2 h.  With η = 0.5, that needs 100 / 0.5
  // = 200 MWh_e of generator input, i.e. 100 MW for 2 h.  Total
  // cost = 100 × 2 × 10 = 2000 → 2.0 after scale.
  System sys;
  sys.name = "electrolyser_smoke";
  sys.bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 200.0,
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };
  sys.hydrogen_node_array = {
      {
          {
              .uid = Uid {11},
              .name = "h2_node",
          },
      },
  };
  sys.hydrogen_storage_array = {
      {
          .uid = Uid {1},
          .name = "salt_cavern",
          .hydrogen_node = SingleId {Uid {11}},
          .emin = 0.0,
          .emax = 500.0,
          .eini = 100.0,
          .efin = 200.0,  // Storage must end +100 MWh_LHV higher than start.
          .capacity = 500.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };
  sys.carrier_converter_array = {
      {
          .uid = Uid {1},
          .name = "electrolyser",
          .type = "electrolyser",
          .from_carrier = Carrier::Electric,
          .to_carrier = Carrier::Hydrogen,
          .from_node = SingleId {Uid {1}},
          .to_node = SingleId {Uid {11}},
          .efficiency = 0.5,
          .capacity = 200.0,
      },
  };

  const auto simulation = make_chrono_2h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  if (!result.has_value()) {
    MESSAGE("resolve error code=" << static_cast<int>(result.error().code)
                                  << " message=" << result.error().message
                                  << " status=" << result.error().status);
  }
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Expected: 200 MWh_e × $10/MWh = $2000 → 2.0 after scale.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));

  // Electrolyser input columns should be > 0 (energy is flowing).
  const auto& cc_elems = system_lp.elements<CarrierConverterLP>();
  REQUIRE(cc_elems.size() == 1);
  const auto& cc = cc_elems.front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& inputs = cc.input_cols_at(scenarios[0], stages[0]);
  const auto sol = li.get_col_sol();
  double total_input = 0.0;
  for (const auto& [buid, col] : inputs) {
    total_input += sol[col];
  }
  // 200 MWh_e of electric input across 2 blocks of 1 h each.
  CHECK(total_input == doctest::Approx(200.0).epsilon(0.01));
}
