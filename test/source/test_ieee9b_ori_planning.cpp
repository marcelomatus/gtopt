/**
 * @file      test_ieee9b_ori_planning.cpp
 * @brief     Integration test for IEEE 9-bus original base case
 * @date      2026-02-22
 * @copyright BSD-3-Clause
 *
 * Integration test for the IEEE 9-bus original base case.  The system has:
 *   - 9 buses, 3 simple thermal generators (no solar profile), 3 demand buses,
 *     and 9 transmission lines.
 *   - A single 1-hour block (1 stage, 1 scenario).
 *   - Original IEEE 9-bus loads: 125 MW at b5, 100 MW at b7, 90 MW at b9.
 *   - All generators are thermal with costs 20, 35, 30 $/MWh respectively.
 *   - No generator_profile_array (no solar/renewable profiles).
 *
 * Numerical solution (DC OPF with Kirchhoff constraints, scale_objective=1000):
 *   - Line congestion at b4→b5 forces load shedding at d1 (bus b5).
 *   - Objective (scaled) ≈ 55.184, corresponding to ≈ 55184 $/h unscaled.
 *   - Total generation ≈ 268.1 MW (less than 315 MW requested).
 *   - fail_sol[d1] ≈ 46.9 MW (unserved at bus b5).
 *   - LMP at b5 = demand_fail_cost (1000 $/MWh) — congested bus.
 */

#include <filesystem>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

#include "test_csv_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
using gtopt::test_helpers::read_uid_values;

// clang-format off
static constexpr std::string_view ieee9b_ori_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_9b_ori",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"},
      {"uid": 4, "name": "b4"}, {"uid": 5, "name": "b5"}, {"uid": 6, "name": "b6"},
      {"uid": 7, "name": "b7"}, {"uid": 8, "name": "b8"}, {"uid": 9, "name": "b9"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin": 0,  "pmax": 270, "gcost": 30, "capacity": 270}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]]},
      {"uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]]},
      {"uid": 3, "name": "d3", "bus": "b9", "lmax":  [[90.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576,  "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625,  "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586,  "tmax_ab": 270, "tmax_ba": 270},
      {"uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,    "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008,  "tmax_ab": 250, "tmax_ba": 250}
    ]
  }
})";
// clang-format on

TEST_CASE("IEEE 9-bus original - JSON parse and structure check")
{
  using namespace gtopt;
  auto planning = parse_planning_json(ieee9b_ori_json);

  CHECK(planning.system.name == "ieee_9b_ori");
  CHECK(planning.system.bus_array.size() == 9);
  CHECK(planning.system.generator_array.size() == 3);
  CHECK(planning.system.demand_array.size() == 3);
  CHECK(planning.system.line_array.size() == 9);
  CHECK(planning.system.generator_profile_array.empty());

  // Single block, 1 stage, 1 scenario
  CHECK(planning.simulation.block_array.size() == 1);
  CHECK(planning.simulation.stage_array.size() == 1);
  CHECK(planning.simulation.scenario_array.size() == 1);
}

TEST_CASE("IEEE 9-bus original - LP solve")
{
  Planning base;
  base.merge(parse_planning_json(ieee9b_ori_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}

TEST_CASE("IEEE 9-bus original - solution correctness")
{
  // True DC-OPF optimum (with adaptive theta_max — see
  // `PlanningLP::auto_scale_theta` and `model_options.theta_max`):
  //
  //   g1 = 250 MW (max, gcost=20),
  //   g3 =  55 MW (gcost=30),
  //   g2 =  10 MW (pmin, gcost=35).
  //   total_gen = 315 MW = total_load (no shedding).
  //   obj = (250·20 + 10·35 + 55·30) / 1000 = 7.0.
  //
  // Historical note (pre-adaptive-theta_max): the hardcoded ±2π bound
  // on θ secretly capped flows below `tmax` whenever `tmax · x_τ > 2π`
  // (`f_l1_4 = 250 MW · 0.0576 = 14.4 rad ≫ 2π`).  That artifact
  // forced load shedding at b5 and pushed the objective to ~55.18.
  // The adaptive bound `Σ_l tmax_l · x_τ_l` removes the artifact and
  // the true unconstrained-by-θ optimum is recovered.
  const auto out_dir =
      std::filesystem::temp_directory_path() / "gtopt_ieee9b_correctness";
  std::filesystem::create_directories(out_dir);

  Planning base;
  base.merge(parse_planning_json(ieee9b_ori_json));
  base.options.output_directory = out_dir.string();

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp_interface = systems.front().front().linear_interface();

  planning_lp.write_out();

  SUBCASE("objective value — true DC-OPF optimum")
  {
    // 250·$20 + 10·$35 + 55·$30 = $7000, scaled by 1000 → 7.0.
    CHECK(lp_interface.get_obj_value_raw()
          == doctest::Approx(7.0).epsilon(1e-4));
  }

  SUBCASE("generation balance equals served demand (no shedding)")
  {
    const auto gen =
        read_uid_values(out_dir / "Generator" / "generation_sol", 3);
    const auto load = read_uid_values(out_dir / "Demand" / "load_sol", 3);
    REQUIRE(gen.size() == 3);
    REQUIRE(load.size() == 3);

    const double total_gen = gen[0] + gen[1] + gen[2];
    const double total_load = load[0] + load[1] + load[2];

    CHECK(total_gen == doctest::Approx(total_load).epsilon(1e-4));
    // All requested 315 MW served — no theta-bound artifact.
    CHECK(total_load == doctest::Approx(315.0).epsilon(1e-4));
  }

  SUBCASE("no load shedding at any bus")
  {
    const auto fail = read_uid_values(out_dir / "Demand" / "fail_sol", 3);
    REQUIRE(fail.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
      CAPTURE(i);
      CHECK(fail[i] == doctest::Approx(0.0).epsilon(1e-4));
    }
  }

  SUBCASE("all three demands fully served")
  {
    const auto load = read_uid_values(out_dir / "Demand" / "load_sol", 3);
    REQUIRE(load.size() == 3);
    CHECK(load[0] == doctest::Approx(125.0).epsilon(1e-4));  // d1 at b5
    CHECK(load[1] == doctest::Approx(100.0).epsilon(1e-4));  // d2 at b7
    CHECK(load[2] == doctest::Approx(90.0).epsilon(1e-4));  // d3 at b9
  }

  SUBCASE("bus LMPs reflect dispatch order, not artifact")
  {
    const auto lmp = read_uid_values(out_dir / "Bus" / "balance_dual", 9);
    REQUIRE(lmp.size() == 9);

    // All LMPs must be positive (power has value everywhere).
    for (size_t b = 0; b < 9; ++b) {
      CAPTURE(b);
      CHECK(lmp[b] > 0.0);
    }
    // No bus's LMP should be the demand_fail_cost penalty
    // (1000 $/MWh) — that would indicate load shedding, which the
    // adaptive theta_max removes on this fixture.
    for (size_t b = 0; b < 9; ++b) {
      CAPTURE(b);
      CHECK(lmp[b] < 100.0);  // bounded by max gcost (45) + scale slack
    }
  }

  SUBCASE("all generators within bounds")
  {
    const auto gen =
        read_uid_values(out_dir / "Generator" / "generation_sol", 3);
    REQUIRE(gen.size() == 3);

    // g1: pmin=10, pmax=250
    CHECK(gen[0] >= 10.0 - 1e-4);
    CHECK(gen[0] <= 250.0 + 1e-4);
    // g2: pmin=10, pmax=300
    CHECK(gen[1] >= 10.0 - 1e-4);
    CHECK(gen[1] <= 300.0 + 1e-4);
    // g3: pmin=0, pmax=270
    CHECK(gen[2] >= 0.0 - 1e-4);
    CHECK(gen[2] <= 270.0 + 1e-4);
  }

  std::filesystem::remove_all(out_dir);
}
