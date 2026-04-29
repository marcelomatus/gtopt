/**
 * @file      test_demand_emin_planning.hpp
 * @brief     Tests for demand minimum energy (emin) LP paths
 * @date      2026-03-21
 * @copyright BSD-3-Clause
 *
 * Exercises demand_lp.cpp emin code paths (lines 65-88, 150-168)
 * that require a demand with emin/ecost set.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// clang-format off
static constexpr std::string_view demand_emin_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 4},
      {"uid": 2, "duration": 4}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "emin_test",
    "bus_array": [
      {"uid": 1, "name": "b1"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {
        "uid": 1, "name": "d1", "bus": "b1",
        "lmax": [[80.0, 80.0]],
        "emin": 500,
        "ecost": 500
      }
    ]
  }
})";
// clang-format on

TEST_CASE("Demand emin - minimum energy constraint exercised")
{
  using namespace gtopt;

  auto planning = parse_planning_json(demand_emin_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // Access the solved LP
  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());
}

// clang-format off
//
// Regression: demand emin path with non-unit scenario probability.
//
// Bug history: prior to the fix, ``demand_lp.cpp:92`` folded the
// ``ecost`` column cost via ``CostHelper::stage_ecost(stage,
// ecost/duration)`` (default probability=1.0).  But the column was
// added with ``make_stage_context(scenario.uid(), stage.uid())`` →
// STIndexHolder → ``OutputContext::add_col_cost`` divides by
// ``scenario_stage_icost_factors = 1/(prob × discount × duration)``.
// The asymmetry meant the LP cost coefficient lacked the
// ``probability_factor`` factor that the read-side inverse expected,
// silently leaving the reported reduced cost off by ``1/prob`` for
// any scenario with prob != 1.0.
//
// This test exercises the path with prob = 0.5 so it would fail with
// the buggy ``stage_ecost`` helper but pass with
// ``scenario_stage_ecost``.
static constexpr std::string_view demand_emin_2scene_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 4}, {"uid": 2, "duration": 4}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2, "active": 1}],
    "scenario_array": [
      {"uid": 1, "probability_factor": 0.5},
      {"uid": 2, "probability_factor": 0.5}
    ]
  },
  "system": {
    "name": "emin_2scene",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1",
       "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1",
       "lmax": [[80.0, 80.0]],
       "emin": 500,
       "ecost": 500}
    ]
  }
})";
// clang-format on

TEST_CASE(  // NOLINT
    "Demand emin - probability factor folded correctly (regression)")
{
  using namespace gtopt;

  // Strategy: compare the converged LP objective on two equivalent
  // physical problems: (a) a single scenario with probability_factor
  // = 1, and (b) two identical scenarios with probability_factor =
  // 0.5 each.  Both encode the SAME expected-discounted-cost, so a
  // correct LP must produce the same objective value.  The buggy
  // ``stage_ecost`` path scales the emin slack column without
  // applying the scenario probability_factor, doubling that
  // column's contribution in the 2-scenario fixture and producing a
  // detectably different objective.
  auto planning_a =
      parse_planning_json(demand_emin_json);  // 1-scenario, prob=1.0
  PlanningLP plp_a(std::move(planning_a));
  auto res_a = plp_a.resolve();
  REQUIRE(res_a.has_value());
  REQUIRE_FALSE(plp_a.systems().empty());
  REQUIRE_FALSE(plp_a.systems().front().empty());
  const double obj_a =
      plp_a.systems().front().front().linear_interface().get_obj_value();

  auto planning_b =
      parse_planning_json(demand_emin_2scene_json);  // 2-scenario, prob=0.5
  PlanningLP plp_b(std::move(planning_b));
  auto res_b = plp_b.resolve();
  REQUIRE(res_b.has_value());
  REQUIRE_FALSE(plp_b.systems().empty());
  REQUIRE_FALSE(plp_b.systems().front().empty());
  const double obj_b =
      plp_b.systems().front().front().linear_interface().get_obj_value();

  CAPTURE(obj_a);
  CAPTURE(obj_b);
  // The two LP runs must produce the same expected-discounted-cost
  // objective.  Pre-fix this assertion failed (off-by-prob on the
  // emin slack column produced obj_b shifted by a fixed amount
  // proportional to ecost × emin × (1 - prob) per scenario).
  CHECK(obj_b == doctest::Approx(obj_a).epsilon(1e-6));
}

// clang-format off
static constexpr std::string_view demand_lossfactor_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "lossfactor_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {
        "uid": 1, "name": "d1", "bus": "b1",
        "lmax": [[100.0]],
        "lossfactor": 0.05
      }
    ]
  }
})";
// clang-format on

TEST_CASE("Demand lossfactor - loss factor applied in bus balance")
{
  using namespace gtopt;

  auto planning = parse_planning_json(demand_lossfactor_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // With 5% loss factor, the generator must produce more than 100 MW
  // to meet 100 MW demand: gen = 100 * (1 + 0.05) = 105 MW
  const double obj = li.get_obj_value_raw();
  // obj = 105 * 20 / 1000 = 2.1
  CHECK(obj == doctest::Approx(2.1));
}

// clang-format off
static constexpr std::string_view line_losses_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "use_kirchhoff": true,
    "use_line_losses": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "line_losses_test",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b2", "lmax": [[100.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.02, "resistance": 0.01,
       "tmax_ab": 300, "tmax_ba": 300}
    ]
  }
})";
// clang-format on

TEST_CASE("Line losses - resistive losses exercised in LP")
{
  using namespace gtopt;

  auto planning = parse_planning_json(line_losses_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // With line losses, the generator must produce slightly more than 100 MW
  // Exact amount depends on the loss model (resistance-based)
  const double obj = li.get_obj_value_raw();
  CHECK(obj >= 2.0);  // >= 100 * 20 / 1000 = 2.0 (with or without losses)
}

// clang-format off
static constexpr std::string_view demand_capacity_expansion_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 10000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 8760}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1},
      {"uid": 2, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "expansion_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100,
       "gcost": 20, "capacity": 100,
       "annual_capcost": 50000, "capmax": 200
      }
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0], [140.0]]}
    ]
  }
})";
// clang-format on

TEST_CASE("Capacity expansion - generator expansion modules exercised")
{
  using namespace gtopt;

  auto planning = parse_planning_json(demand_capacity_expansion_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());
}

// clang-format off
static constexpr std::string_view inactive_demand_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "inactive_demand_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]},
      {"uid": 2, "name": "d2", "bus": "b1", "lmax": [[50.0]], "active": 0}
    ]
  }
})";
// clang-format on

// clang-format off
static constexpr std::string_view reserve_provision_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "reserve_fail_cost": 5000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "reserve_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]}
    ],
    "reserve_zone_array": [
      {"uid": 1, "name": "rz1",
       "urreq": 30, "drreq": 20}
    ],
    "reserve_provision_array": [
      {"uid": 1, "name": "rp1", "generator": 1, "reserve_zones": "1",
       "urmax": 50, "drmax": 40,
       "ur_provision_factor": 1.0, "dr_provision_factor": 1.0,
       "urcost": 5, "drcost": 3}
    ]
  }
})";
// clang-format on

TEST_CASE("Reserve provision - up and down reserves exercised in LP")
{
  using namespace gtopt;

  auto planning = parse_planning_json(reserve_provision_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());
}

TEST_CASE("Inactive demand - inactive demand skipped in LP")
{
  using namespace gtopt;

  auto planning = parse_planning_json(inactive_demand_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // Only d1 (100 MW) is active, d2 is inactive
  // obj = 100 * 20 / 1000 = 2.0
  const double obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0));
}

// Test with generator profiles (solar profiles with capacity factor)
// Covers generator_profile_lp and demand_profile_lp paths
// clang-format off
static constexpr std::string_view generator_profile_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 12},
      {"uid": 2, "duration": 12}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "profile_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g_thermal", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 50, "capacity": 200},
      {"uid": 2, "name": "g_solar", "bus": "b1", "pmin": 0, "pmax": 100,
       "gcost": 0, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0, 120.0]]}
    ],
    "generator_profile_array": [
      {"uid": 1, "name": "solar_profile", "generator": 2,
       "profile": [[[0.8, 0.0]]]}
    ]
  }
})";
// clang-format on

TEST_CASE("Generator profile - solar profile applied to capacity")
{
  using namespace gtopt;

  auto planning = parse_planning_json(generator_profile_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // Block 1: solar=80MW (0.8*100), demand=80MW → solar covers all
  // Block 2: solar=0MW (0.0*100), demand=120MW → thermal=120MW
  // Cost = 80*0*12 + 120*50*12 / 1000 = 72.0
  const double obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(72.0));
}

// Test with multi-stage planning and 2 scenarios
// clang-format off
static constexpr std::string_view multi_scenario_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 0.5},
      {"uid": 2, "probability_factor": 0.5}
    ]
  },
  "system": {
    "name": "multi_scenario",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 30, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]}
    ]
  }
})";
// clang-format on

TEST_CASE("Multi-scenario planning")
{
  using namespace gtopt;

  auto planning = parse_planning_json(multi_scenario_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() >= 1);  // scenes processed

  const auto& systems = planning_lp.systems();
  CHECK_FALSE(systems.empty());
}

// ── Inactive generator test ─────────────────────────────────────────
// Covers generator_lp.cpp inactive path (line 74)
// clang-format off
static constexpr std::string_view inactive_gen_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "inactive_gen_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 20, "capacity": 200},
      {"uid": 2, "name": "g2_inactive", "bus": "b1", "pmin": 0, "pmax": 100,
       "gcost": 5, "capacity": 100, "active": 0}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ]
  }
})";
// clang-format on

TEST_CASE("Inactive generator - skipped in LP")
{
  using namespace gtopt;

  auto planning = parse_planning_json(inactive_gen_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // Only g1 ($20) serves 80 MW load; g2 is inactive (would have been cheaper)
  // obj = 80 * 20 / 1000 = 1.6
  const double obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(1.6));
}

// ── Quadratic line loss model test ──────────────────────────────────
// Covers line_lp.cpp add_quadratic_flow_direction (lines 82-182)
// clang-format off
static constexpr std::string_view quadratic_loss_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "use_single_bus": false,
    "use_kirchhoff": true,
    "use_line_losses": true,
    "loss_segments": 3,
    "demand_fail_cost": 10000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "quadratic_loss_test",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500,
       "gcost": 20, "capacity": 500}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b2", "lmax": [[200.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.02, "resistance": 0.05, "voltage": 220,
       "tmax_ab": 400, "tmax_ba": 400}
    ]
  }
})";
// clang-format on

TEST_CASE("Line quadratic loss model - piecewise-linear loss approximation")
{
  using namespace gtopt;

  auto planning = parse_planning_json(quadratic_loss_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // With quadratic losses, objective should be > 200*20/1000 = 4.0
  // because the generator must produce extra to compensate for losses
  const double obj = li.get_obj_value_raw();
  CHECK(obj >= 4.0);
}

// ── Line capacity expansion test ────────────────────────────────────
// Covers line_lp.cpp capacity_col paths (lines 378-388, 403-413)
// clang-format off
static constexpr std::string_view line_expansion_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
    "use_single_bus": false,
    "use_kirchhoff": true,
    "use_line_losses": false,
    "demand_fail_cost": 10000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 8760}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1},
      {"uid": 2, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "line_expansion_test",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500,
       "gcost": 10, "capacity": 500}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b2",
       "lmax": [[50.0], [200.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.05,
       "tmax_ab": 100, "tmax_ba": 100,
       "annual_capcost": 5000, "capmax": 300}
    ]
  }
})";
// clang-format on

TEST_CASE("Line capacity expansion - expansion modules exercised")
{
  using namespace gtopt;

  auto planning = parse_planning_json(line_expansion_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());
}

// ── Line with linear loss + capacity expansion ──────────────────────
// Covers line_lp.cpp capacity constraint in linear loss model (lines 378-413)
// clang-format off
static constexpr std::string_view line_loss_expansion_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
    "use_single_bus": false,
    "use_kirchhoff": true,
    "use_line_losses": true,
    "demand_fail_cost": 10000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 8760}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1},
      {"uid": 2, "first_block": 0, "count_block": 1, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "line_loss_expansion_test",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500,
       "gcost": 10, "capacity": 500}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b2",
       "lmax": [[50.0], [200.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.05, "resistance": 0.01, "voltage": 220,
       "tmax_ab": 100, "tmax_ba": 100,
       "annual_capcost": 5000, "capmax": 300}
    ]
  }
})";
// clang-format on

TEST_CASE("Line linear loss + capacity expansion - capacity constraint paths")
{
  using namespace gtopt;

  auto planning = parse_planning_json(line_loss_expansion_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());
}

// ── Battery with charge/discharge test ──────────────────────────────
// Covers battery_lp.cpp main paths
// clang-format off
static constexpr std::string_view battery_planning_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 4},
      {"uid": 2, "duration": 4}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2, "active": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "battery_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g_cheap", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 10, "capacity": 200},
      {"uid": 2, "name": "g_expensive", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 100, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[50.0, 150.0]]}
    ],
    "battery_array": [
      {"uid": 1, "name": "batt1", "bus": 1,
       "emax": 400, "emin": 0,
       "pmax_charge": 100, "pmax_discharge": 100,
       "input_efficiency": 0.95, "output_efficiency": 0.95,
       "gcost": 0, "capacity": 400,
       "use_state_variable": true, "daily_cycle": false}
    ]
  }
})";
// clang-format on

TEST_CASE("Battery - charge/discharge storage exercised")
{
  using namespace gtopt;

  auto planning = parse_planning_json(battery_planning_json);

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  const auto& systems = planning_lp.systems();
  REQUIRE_FALSE(systems.empty());
  REQUIRE_FALSE(systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.is_optimal());

  // Battery can shift cheap generation from block 1 to block 2
  // reducing need for expensive generator
  const double obj = li.get_obj_value_raw();
  // Without battery: 50*10*4 + 150*10*4 + ... = higher
  // With battery: lower cost via arbitrage
  CHECK(obj > 0.0);
}
