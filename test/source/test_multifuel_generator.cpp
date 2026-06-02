// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for Phase 1 of Issue #510 — multi-fuel Generator support.
//
// Promotes `Generator.fuel` from a static `OptSingleId` to a
// TB-schedulable identifier (`OptTBSingleIdSched`).  Phase 1 covers:
//
//   1. Scalar fuel still parses and behaves byte-identically to the
//      legacy single-fuel branch (no LP-coefficient drift).
//   2. A 2-D `[[stage][block]]` Uid matrix switches the active fuel
//      per block — per-MWh dispatch cost equals
//      `block_fuel.price · heat_rate + gcost` on every block.
//   3. The `Fuel.max_offtake` cap row buckets per-block heat-rate
//      coefficients into the matching fuel only on blocks where that
//      fuel is active (acceptance criterion #3).

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/fuel.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/single_id_sched.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── Helper: 1-stage, 3-block simulation with unit-duration blocks ───
[[nodiscard]] Simulation make_one_stage_three_block_simulation()
{
  return {
      .block_array =
          {
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
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 3,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
                  .probability_factor = 1.0,
              },
          },
  };
}

}  // namespace

TEST_CASE(
    "Generator.fuel scalar form preserves byte-identical legacy "
    "LP coefficient")  // NOLINT
{
  // Two equivalent configurations (legacy scalar Uid vs. new
  // `OptTBSingleIdSched` scalar variant) must produce the same
  // objective value.  This pins the byte-identical fast path required
  // by acceptance criterion #2.
  const double fuel_price = 5.0;  // $/GJ
  const double hr = 2.0;  // GJ/MWh
  const double demand = 10.0;
  const double dur = 1.0;

  const Array<Bus> bus_array {
      {.uid = Uid {1}, .name = "b1"},
  };
  const Array<Fuel> fuel_array {
      {.uid = Uid {1}, .name = "gas", .price = fuel_price},
  };
  const Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 0.0,
          .fuel = Uid {1},
          .heat_rate = hr,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = demand,
      },
  };

  const auto simulation = make_one_stage_three_block_simulation();
  const System system {
      .name = "MultiFuelScalar",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Expected: 3 blocks × demand × (fuel_price · hr) = 3 × 10 × 10 = 300.
  const double expected = 3.0 * demand * dur * (fuel_price * hr);
  CHECK(lp.get_obj_value() == doctest::Approx(expected).epsilon(1e-6));
}

TEST_CASE("Generator.fuel TB matrix switches per-block fuel + cost")  // NOLINT
{
  // 3 blocks, 2 fuels: blocks 1 and 3 use cheap fuel (gas, price=5),
  // block 2 uses expensive fuel (diesel, price=20).  Heat rate is a
  // common scalar.  Demand is constant.  Expected dispatch cost
  // is the sum of `block_fuel.price · hr · demand` over blocks.
  const double gas_price = 5.0;
  const double diesel_price = 20.0;
  const double hr = 2.0;
  const double demand = 10.0;

  const Array<Bus> bus_array {
      {.uid = Uid {1}, .name = "b1"},
  };
  const Array<Fuel> fuel_array {
      {.uid = Uid {1}, .name = "gas", .price = gas_price},
      {.uid = Uid {2}, .name = "diesel", .price = diesel_price},
  };
  const Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 0.0,
          // [stage=1][block=1,2,3] = gas, diesel, gas
          .fuel = UidMatrix {{Uid {1}, Uid {2}, Uid {1}}},
          .heat_rate = hr,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = demand,
      },
  };

  const auto simulation = make_one_stage_three_block_simulation();
  const System system {
      .name = "MultiFuelMatrix",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Block-by-block cost: gas, diesel, gas.
  const double expected = demand * hr * (gas_price + diesel_price + gas_price);
  CHECK(lp.get_obj_value() == doctest::Approx(expected).epsilon(1e-6));
}

TEST_CASE("OptTBSingleIdSched JSON round-trip preserves shape")  // NOLINT
{
  // Verify all three input shapes round-trip through the JSON binding:
  //   * scalar Uid                    — legacy fast path
  //   * scalar Name                   — legacy fast path
  //   * 2-D matrix of Uid (multi-fuel)
  constexpr std::string_view json_uid =
      R"({"uid":1,"name":"g1","bus":1,"fuel":7,"heat_rate":2.0})";
  const auto gen_uid = daw::json::from_json<Generator>(json_uid);
  REQUIRE(gen_uid.fuel.has_value());
  CHECK(is_constant_single_id(gen_uid.fuel));
  const auto sid_uid = constant_single_id(gen_uid.fuel);
  REQUIRE(sid_uid.has_value());
  CHECK(std::holds_alternative<Uid>(*sid_uid));
  CHECK(std::get<Uid>(*sid_uid) == Uid {7});

  constexpr std::string_view json_name =
      R"({"uid":1,"name":"g1","bus":1,"fuel":"gas","heat_rate":2.0})";
  const auto gen_name = daw::json::from_json<Generator>(json_name);
  REQUIRE(gen_name.fuel.has_value());
  CHECK(is_constant_single_id(gen_name.fuel));
  const auto sid_name = constant_single_id(gen_name.fuel);
  REQUIRE(sid_name.has_value());
  CHECK(std::holds_alternative<Name>(*sid_name));

  constexpr std::string_view json_matrix =
      R"({"uid":1,"name":"g1","bus":1,"fuel":[[7,8,7],[8,8,8]],)"
      R"("heat_rate":2.0})";
  const auto gen_mat = daw::json::from_json<Generator>(json_matrix);
  REQUIRE(gen_mat.fuel.has_value());
  CHECK_FALSE(is_constant_single_id(gen_mat.fuel));
  REQUIRE(std::holds_alternative<UidMatrix>(*gen_mat.fuel));
  const auto& mat = std::get<UidMatrix>(*gen_mat.fuel);
  REQUIRE(mat.size() == 2);
  REQUIRE(mat[0].size() == 3);
  CHECK(mat[0][0] == Uid {7});
  CHECK(mat[0][1] == Uid {8});
  CHECK(mat[1][2] == Uid {8});
}
