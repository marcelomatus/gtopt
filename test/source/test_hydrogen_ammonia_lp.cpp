// SPDX-License-Identifier: BSD-3-Clause
//
// Smoke integration test for the Carrier + HydrogenNode +
// HydrogenStorage + AmmoniaNode + AmmoniaStorage type-safe
// registration path.
//
// Builds a complete ``System`` with all four new arrays, runs LP
// assembly via ``SystemLP``, and verifies:
//   * the System parses + holds the H₂ and NH₃ arrays,
//   * ``HydrogenStorageLP`` and ``AmmoniaStorageLP`` are registered
//     in the LP element collections,
//   * the LP assembles + solves (status optimal) from the Bus side
//     machinery alone.
//
// The full carrier-side LP (electrolyser + Haber-Bosch + cracker /
// fuel-cell converters on HydrogenNodeLP / AmmoniaNodeLP balance
// rows) lands when those carrier-balance LP classes are added — see
// the H₂ + NH₃ plan for the next milestone.

#include <doctest/doctest.h>
#include <gtopt/ammonia_node.hpp>
#include <gtopt/ammonia_storage_lp.hpp>
#include <gtopt/hydrogen_node.hpp>
#include <gtopt/hydrogen_storage_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_hydrogen_ammonia_lp_smoke
{

Simulation make_chrono_4h_simulation()
{
  return {
      .block_array =
          {
              {.uid = Uid {0}, .duration = 1.0},
              {.uid = Uid {1}, .duration = 1.0},
              {.uid = Uid {2}, .duration = 1.0},
              {.uid = Uid {3}, .duration = 1.0},
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
              {.uid = Uid {0}},
          },
  };
}

}  // namespace test_hydrogen_ammonia_lp_smoke

TEST_CASE(
    "HydrogenNode + HydrogenStorage + AmmoniaNode + AmmoniaStorage register "
    "on a System with a real Bus LP")
// NOLINT
{
  using namespace test_hydrogen_ammonia_lp_smoke;

  System sys;
  sys.name = "h2_nh3_smoke";
  sys.bus_array = {
      {.uid = Uid {1}, .name = "b1"},
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

  // Hydrogen carrier — salt cavern (~200 GWh_LHV).
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
          // ACTIVE: finp/fout now stamped into HydrogenNodeLP balance row.
          .hydrogen_node = SingleId {Uid {11}},
          .input_efficiency = 0.95,
          .output_efficiency = 0.99,
          // annual_loss intentionally unset: with eini == efin and no
          // external source/sink, any self-discharge makes SoC drift below
          // efin and the LP becomes primal-infeasible.
          .emin = 50000.0,
          .emax = 200000.0,
          .eini = 100000.0,
          .efin = 100000.0,  // Same as eini — no external source/sink yet,
                             // so the LP can't move H₂ in/out of the cavern.
          .capacity = 200000.0,  // ≥ eini so SoC fits inside the cap.
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  // Ammonia carrier — 60 kt refrigerated tank (~310 GWh_LHV).
  sys.ammonia_node_array = {
      {
          {
              .uid = Uid {22},
              .name = "nh3_node",
          },
      },
  };
  sys.ammonia_storage_array = {
      {
          .uid = Uid {1},
          .name = "nh3_tank",
          // ACTIVE: finp/fout now stamped into AmmoniaNodeLP balance row.
          .ammonia_node = SingleId {Uid {22}},
          // annual_loss intentionally unset (same rationale as H₂ above).
          .emin = 0.0,
          .emax = 310000.0,
          .eini = 155000.0,
          .efin = 155000.0,  // Same as eini — no external source/sink yet.
          .capacity = 310000.0,
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

  // Both new storage element types must be registered.
  const auto& h2_elems = system_lp.elements<HydrogenStorageLP>();
  REQUIRE(h2_elems.size() == 1);
  CHECK(h2_elems.front().uid() == Uid {1});

  const auto& nh3_elems = system_lp.elements<AmmoniaStorageLP>();
  REQUIRE(nh3_elems.size() == 1);
  CHECK(nh3_elems.front().uid() == Uid {1});

  // LP solves from the Bus side.  Both storages inactive on this
  // milestone, so they contribute no rows/cols.
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
