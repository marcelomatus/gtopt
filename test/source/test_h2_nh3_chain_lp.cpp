// SPDX-License-Identifier: BSD-3-Clause
//
// End-to-end H₂ / NH₃ chain integration test.  Builds the full
// long-term-storage topology:
//
//   ┌──────────┐ electrolyser  ┌──────────┐ Haber-Bosch ┌──────────┐
//   │  Bus b1  ├──────────────►│  H₂ node ├────────────►│ NH₃ node │
//   │  +g1     │◄──── fuel cell┤          │◄─── cracker │          │
//   │  +d1     │               │  +H₂ TES │             │ +NH₃ TES │
//   └──────────┘               └──────────┘             └──────────┘
//
// Forces the cracker → fuel cell path by:
//   * making the bus-side generator EXPENSIVE ($100/MWh),
//   * pre-loading the NH₃ tank (eini = 400 MWh_LHV),
//   * forcing the NH₃ tank to drain (efin = 0).
//
// Net efficiency NH₃ → electric: 0.5 × 0.5 = 0.25 (cracker × fuel
// cell).  400 MWh_LHV of NH₃ ⇒ 100 MWh of electricity supplied to
// the bus.  Demand is 50 MW × 4 h = 200 MWh, so the remaining 100
// MWh comes from g1 → cost = 100 × $100 = $10 000 → 10.0 after scale.

#include <doctest/doctest.h>
#include <gtopt/ammonia_node_lp.hpp>
#include <gtopt/ammonia_storage_lp.hpp>
#include <gtopt/carrier_converter_lp.hpp>
#include <gtopt/hydrogen_node_lp.hpp>
#include <gtopt/hydrogen_storage_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_h2_nh3_chain_lp
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

}  // namespace test_h2_nh3_chain_lp

TEST_CASE("Full H₂ / NH₃ chain: NH₃ tank drains via cracker → fuel cell")
// NOLINT
{
  using namespace test_h2_nh3_chain_lp;

  System sys;
  sys.name = "h2_nh3_chain";

  // ── Electric side ───────────────────────────────────────────────
  sys.bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,  // 50 MW × 4 h = 200 MWh total demand.
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1_backup",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 200.0,
          .gcost = 100.0,  // Expensive backup so the LP prefers NH₃.
          .capacity = 200.0,
      },
  };

  // ── Hydrogen carrier ────────────────────────────────────────────
  sys.hydrogen_node_array = {
      {{.uid = Uid {11}, .name = "h2_node"}},
  };
  sys.hydrogen_storage_array = {
      {
          .uid = Uid {1},
          .name = "h2_buffer",
          .hydrogen_node = SingleId {Uid {11}},
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 0.0,
          .efin = 0.0,  // pure pass-through, no SoC carry.
          .capacity = 1000.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  // ── Ammonia carrier ─────────────────────────────────────────────
  sys.ammonia_node_array = {
      {{.uid = Uid {22}, .name = "nh3_node"}},
  };
  sys.ammonia_storage_array = {
      {
          .uid = Uid {1},
          .name = "nh3_tank",
          .ammonia_node = SingleId {Uid {22}},
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 400.0,
          .efin = 0.0,  // Must drain entirely.
          .capacity = 1000.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  // ── Four converters: all carrier pairings on the chain ──────────
  sys.carrier_converter_array = {
      {
          .uid = Uid {1},
          .name = "electrolyser",
          .type = "electrolyser",
          .from_carrier = Carrier::Electric,
          .to_carrier = Carrier::Hydrogen,
          .from_node = SingleId {Uid {1}},
          .to_node = SingleId {Uid {11}},
          .efficiency = 0.7,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "fuel_cell",
          .type = "fuel_cell",
          .from_carrier = Carrier::Hydrogen,
          .to_carrier = Carrier::Electric,
          .from_node = SingleId {Uid {11}},
          .to_node = SingleId {Uid {1}},
          .efficiency = 0.5,
          .capacity = 200.0,
      },
      {
          .uid = Uid {3},
          .name = "haber_bosch",
          .type = "haber_bosch",
          .from_carrier = Carrier::Hydrogen,
          .to_carrier = Carrier::Ammonia,
          .from_node = SingleId {Uid {11}},
          .to_node = SingleId {Uid {22}},
          .efficiency = 0.7,
          .capacity = 200.0,
      },
      {
          .uid = Uid {4},
          .name = "cracker",
          .type = "nh3_cracker",
          .from_carrier = Carrier::Ammonia,
          .to_carrier = Carrier::Hydrogen,
          .from_node = SingleId {Uid {22}},
          .to_node = SingleId {Uid {11}},
          .efficiency = 0.5,
          .capacity = 200.0,
      },
  };

  // ── Build LP, solve ─────────────────────────────────────────────
  const auto simulation = make_chrono_4h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 10000.0;
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

  // Expected cost:
  //   * 400 MWh of NH₃ released → 200 MWh of H₂ via cracker (η = 0.5)
  //     → 100 MWh of electric via fuel cell (η = 0.5).
  //   * Remaining 200 − 100 = 100 MWh of demand met by g1 @ $100/MWh
  //     = $10 000.  Scaled by 1000 → 10.0.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(10.0).epsilon(0.01));

  // Locate the four converters by name.
  const auto& cc_elems = system_lp.elements<CarrierConverterLP>();
  REQUIRE(cc_elems.size() == 4);

  const CarrierConverterLP* cracker = nullptr;
  const CarrierConverterLP* fuel_cell = nullptr;
  const CarrierConverterLP* electrolyser = nullptr;
  const CarrierConverterLP* haber_bosch = nullptr;
  for (const auto& cc : cc_elems) {
    if (cc.uid() == Uid {1}) {
      electrolyser = &cc;
    } else if (cc.uid() == Uid {2}) {
      fuel_cell = &cc;
    } else if (cc.uid() == Uid {3}) {
      haber_bosch = &cc;
    } else if (cc.uid() == Uid {4}) {
      cracker = &cc;
    }
  }
  REQUIRE(cracker != nullptr);
  REQUIRE(fuel_cell != nullptr);
  REQUIRE(electrolyser != nullptr);
  REQUIRE(haber_bosch != nullptr);

  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto sol = li.get_col_sol();

  const auto col_sum = [&](const CarrierConverterLP& cc)
  {
    double total = 0.0;
    const auto& cols = cc.input_cols_at(scenarios[0], stages[0]);
    for (const auto& [buid, col] : cols) {
      total += sol[col];
    }
    return total;
  };

  // The optimal LP path uses the cracker (consumes 400 MWh NH₃) and
  // the fuel cell (consumes 200 MWh H₂ to produce 100 MWh electric).
  CHECK(col_sum(*cracker) == doctest::Approx(400.0).epsilon(0.01));
  CHECK(col_sum(*fuel_cell) == doctest::Approx(200.0).epsilon(0.01));

  // The electrolyser + Haber-Bosch path is "available but unused" —
  // round-trip through NH₃ back to electric loses energy at every
  // step, so the LP leaves both at zero.
  CHECK(col_sum(*electrolyser) == doctest::Approx(0.0).epsilon(0.01));
  CHECK(col_sum(*haber_bosch) == doctest::Approx(0.0).epsilon(0.01));
}
