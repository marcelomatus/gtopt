// SPDX-License-Identifier: BSD-3-Clause
//
// Smoke integration test for the Carrier + ThermalNode +
// ThermalStorage type-safe registration path.  Builds a complete
// ``System`` with the new arrays, runs LP assembly via ``SystemLP``,
// and verifies:
//   * the System parses + holds ThermalNode + ThermalStorage arrays,
//   * ``ThermalStorageLP`` is registered in the LP element collections,
//   * the LP assembles without crashing,
//   * the bus-only side (Bus + Generator + Demand) solves to the
//     expected dispatch cost on its own — i.e. the new TES path does
//     not break the existing LP topology.
//
// The full thermal-side LP (solar field generator + power-block
// converter on a ThermalNode-aware balance row) lands when
// ``ThermalNodeLP`` is added — see the CSP plan for the next milestone.
// Until then the ThermalStorage element is held back from LP assembly
// by leaving its ``active`` field unset on a non-default-active
// timeline; if that escape hatch is needed in the future, set
// ``active = false`` explicitly.

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/thermal_node.hpp>
#include <gtopt/thermal_storage_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace test_thermal_storage_lp_smoke
{

Simulation make_chrono_4h_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {0},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {3},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {0},
                  .first_block = 0,
                  .count_block = 4,
                  .chronological = true,
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

}  // namespace test_thermal_storage_lp_smoke

TEST_CASE(
    "ThermalNode + ThermalStorage register on a System with a real Bus LP")
// NOLINT
{
  using namespace test_thermal_storage_lp_smoke;
  System sys;
  sys.name = "csp_smoke";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 200.0,
          .gcost = 25.0,
          .capacity = 200.0,
      },
  };
  sys.thermal_node_array = {
      {
          {
              .uid = Uid {7},
              .name = "tn1",
          },
      },
  };
  // ThermalStorage element is held INACTIVE on this milestone — the
  // thermal-side balance row (ThermalNodeLP) doesn't exist yet, so an
  // active TES would leave finp/fout columns unbound and the LP would
  // be dual-infeasible.  Marking it inactive exercises the parse +
  // registration path without polluting the LP.
  sys.thermal_storage_array = {
      {
          .uid = Uid {1},
          .name = "tes1",
          .active = false,
          .thermal_node = SingleId {Uid {7}},
          .input_efficiency = 0.98,
          .output_efficiency = 0.98,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 200.0,
          .efin = 200.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  const auto simulation = make_chrono_4h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  // The system must have one ThermalStorageLP element registered.
  const auto& tes_elems = system_lp.elements<ThermalStorageLP>();
  REQUIRE(tes_elems.size() == 1);
  const auto& tes = tes_elems.front();
  CHECK(tes.uid() == Uid {1});

  // LP must assemble + solve from the Bus-side machinery.  The
  // ThermalStorage is inactive (see above) and therefore contributes
  // no rows/cols.
  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  if (!result.has_value()) {
    MESSAGE("resolve error code=" << static_cast<int>(result.error().code)
                                  << " message=" << result.error().message
                                  << " status=" << result.error().status);
  }
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Generator g1 supplies 50 MW × 4 h × $25/MWh = $5000, scaled by
  // 1000 → 5.0.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(5.0).epsilon(0.01));
}
