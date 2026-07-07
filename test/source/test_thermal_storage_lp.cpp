// SPDX-License-Identifier: BSD-3-Clause
//
// Integration tests for ThermalNodeLP + ThermalStorageLP wiring:
//   * Smoke: System registers ThermalNode + ThermalStorage, LP
//     assembles and solves, bus-side dispatch cost matches.
//   * Balance row: ThermalStorageLP stamps finp (−1) / fout (+1) into
//     the ThermalNodeLP balance row.  With no external source/sink at
//     the node and no charge/discharge cost, the LP collapses
//     finp = fout = 0 and the dual is undetermined; we just verify
//     the cols are clamped at zero.

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/thermal_node.hpp>
#include <gtopt/thermal_storage_lp.hpp>

using namespace gtopt;

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
  // ThermalStorage is now ACTIVE: finp/fout cols are stamped into the
  // ThermalNodeLP balance row (−1 charge / +1 discharge).  With no
  // other element on the thermal node, the balance row sum must equal
  // zero in every block, so the LP will set finp[b] = fout[b] = 0 in
  // every block.  eini = efin = 200 satisfies the SoC carry trivially.
  sys.thermal_storage_array = {
      {
          .uid = Uid {1},
          .name = "tes1",
          .thermal_node = SingleId {Uid {7}},
          .input_efficiency = 0.98,
          .output_efficiency = 0.98,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 200.0,
          .efin = 200.0,
          .capacity = 500.0,  // Installed energy capacity (MWh_th) — must
                              // be ≥ eini so the SoC fits.
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

  // The balance row at the thermal node forces finp[b] = fout[b] for
  // every block.  With no external source/sink and no charge/discharge
  // cost, the LP collapses both to zero.
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& finps = tes.finp_cols_at(scenarios[0], stages[0]);
  const auto& fouts = tes.fout_cols_at(scenarios[0], stages[0]);
  const auto sol = li.get_col_sol();
  for (const auto& [buid, col] : finps) {
    CHECK(sol[col] == doctest::Approx(0.0).epsilon(1e-6));
  }
  for (const auto& [buid, col] : fouts) {
    CHECK(sol[col] == doctest::Approx(0.0).epsilon(1e-6));
  }
}
