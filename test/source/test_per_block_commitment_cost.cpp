// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_per_block_commitment_cost.cpp
 * @brief     Per-block Commitment.startup_cost / shutdown_cost read tests
 * @date      2026-06-20
 * @copyright BSD-3-Clause
 *
 * `Commitment.startup_cost` and `Commitment.shutdown_cost` were upgraded
 * from per-stage (`OptTRealFieldSched`) to per-(stage, block)
 * (`OptTBRealFieldSched`), mirroring the Fuel.price / Turbine
 * .production_factor upgrade.  These tests pin that the LP build reads
 * the per-block sample applied to the startup (v) / shutdown (w)
 * objective coefficients — a startup forced into a high-cost block
 * costs more than one forced into a low-cost block — and that the scalar
 * form still broadcasts to every block (back-compat).
 *
 * Fixture: a single chronological stage with two unit-duration blocks
 * (uids 0, 1), one scenario, one generator.  `fixed_status` pins the u
 * variable per block so the startup / shutdown EVENT lands in a known
 * block; with `scale_objective = 1` and `gcost = 0` the raw objective is
 * exactly the per-event cost at that block.
 */

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace per_block_commitment_cost_test  // NOLINT
{

// One chronological stage, two unit-duration blocks (uids 0, 1).
[[nodiscard]] Simulation make_pbcc_two_block_simulation()
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
      .scenario_array = {{.uid = Uid {0}}},
  };
}

// Minimal one-bus, one-generator system with a commitment.  The caller
// supplies the commitment (with whatever startup/shutdown cost shape and
// fixed_status it wants).
[[nodiscard]] System make_pbcc_system(Commitment commitment)
{
  System sys;
  sys.name = "PerBlockCommitmentCost";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 0.0}};
  sys.generator_array = {{.uid = Uid {1},
                          .name = "g1",
                          .bus = Uid {1},
                          .pmin = 0.0,
                          .pmax = 100.0,
                          .gcost = 0.0,
                          .capacity = 100.0}};
  sys.commitment_array = {std::move(commitment)};
  return sys;
}

[[nodiscard]] double solve_raw_obj(const System& sys)
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;  // read raw $ directly
  const PlanningOptionsLP options(popts);
  const auto simulation = make_pbcc_two_block_simulation();
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);
  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  return li.get_obj_value_raw();
}

// Inline 2-D [[stage][block]] matrix helper.
[[nodiscard]] OptTBRealFieldSched matrix2d(Real b0, Real b1)
{
  return OptTBRealFieldSched {std::vector<std::vector<Real>> {{b0, b1}}};
}

}  // namespace per_block_commitment_cost_test

// ── Per-block startup_cost: a startup in a high-cost block costs more ──

TEST_CASE("Commitment.startup_cost per-block: cost depends on the block")
{
  using namespace per_block_commitment_cost_test;  // NOLINT

  // Startup cost cheap in block 0 ($100) and dear in block 1 ($900).
  // fixed_status forces a single startup EVENT in one chosen block:
  //   - off in block 0, on in block 1 → startup happens in block 1
  //     → raw objective = startup_cost[block1] = 900.
  //   - off in block 0, on in block 0 (... no, fixed pins u) we instead
  //     start in block 0 by being off at t<0 (initial off) and on in
  //     both blocks → startup happens in block 0 → cost = 100.
  const auto su = matrix2d(100.0, 900.0);

  SUBCASE("startup forced into the high-cost block (block 1)")
  {
    // u: off in block 0, on in block 1 → v[1] = 1 (startup in block 1).
    Commitment c {
        .uid = Uid {1},
        .name = "g1_uc",
        .generator = Uid {1},
        .startup_cost = su,
        .initial_status = 0.0,
        .relax = true,
        .fixed_status = matrix2d(0.0, 1.0),
    };
    const auto obj = solve_raw_obj(make_pbcc_system(std::move(c)));
    CHECK(obj == doctest::Approx(900.0));
  }

  SUBCASE("startup forced into the low-cost block (block 0)")
  {
    // initial off, u on in BOTH blocks → v[0] = 1 (startup in block 0),
    // no shutdown → raw objective = startup_cost[block0] = 100.
    Commitment c {
        .uid = Uid {1},
        .name = "g1_uc",
        .generator = Uid {1},
        .startup_cost = su,
        .initial_status = 0.0,
        .relax = true,
        .fixed_status = matrix2d(1.0, 1.0),
    };
    const auto obj = solve_raw_obj(make_pbcc_system(std::move(c)));
    CHECK(obj == doctest::Approx(100.0));
  }
}

// ── Per-block shutdown_cost: cost depends on the block of the event ──

TEST_CASE("Commitment.shutdown_cost per-block: cost depends on the block")
{
  using namespace per_block_commitment_cost_test;  // NOLINT

  // Shutdown cheap in block 0 ($10), dear in block 1 ($500).
  // initial ON; u on in block 0, off in block 1 → w[1] = 1 (shutdown in
  // block 1) → raw objective = shutdown_cost[block1] = 500.
  Commitment c {
      .uid = Uid {1},
      .name = "g1_uc",
      .generator = Uid {1},
      .shutdown_cost = matrix2d(10.0, 500.0),
      .initial_status = 1.0,
      .relax = true,
      .fixed_status = matrix2d(1.0, 0.0),
  };
  const auto obj = solve_raw_obj(make_pbcc_system(std::move(c)));
  CHECK(obj == doctest::Approx(500.0));
}

// ── Scalar back-compat: a scalar startup_cost broadcasts to all blocks ──

TEST_CASE("Commitment.startup_cost scalar broadcasts to every block")
{
  using namespace per_block_commitment_cost_test;  // NOLINT

  // A scalar startup cost must produce the SAME objective regardless of
  // which block the startup event lands in (legacy per-stage behaviour).
  constexpr double scalar_cost = 333.0;

  // Startup in block 1.
  Commitment c1 {
      .uid = Uid {1},
      .name = "g1_uc",
      .generator = Uid {1},
      .startup_cost = scalar_cost,
      .initial_status = 0.0,
      .relax = true,
      .fixed_status = matrix2d(0.0, 1.0),
  };
  const auto obj_b1 = solve_raw_obj(make_pbcc_system(std::move(c1)));

  // Startup in block 0.
  Commitment c0 {
      .uid = Uid {1},
      .name = "g1_uc",
      .generator = Uid {1},
      .startup_cost = scalar_cost,
      .initial_status = 0.0,
      .relax = true,
      .fixed_status = matrix2d(1.0, 1.0),
  };
  const auto obj_b0 = solve_raw_obj(make_pbcc_system(std::move(c0)));

  CHECK(obj_b1 == doctest::Approx(scalar_cost));
  CHECK(obj_b0 == doctest::Approx(scalar_cost));
  CHECK(obj_b1 == doctest::Approx(obj_b0));
}
