/**
 * @file      test_ieee14_planning.cpp
 * @brief     Integration test for IEEE 14-bus system
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 *
 * Integration test for the IEEE 14-bus system case.  The system has:
 *   - 14 buses, 5 generators (G3 at bus 3 is a solar plant), 11 demand buses,
 *     and 20 transmission lines.
 *   - A 24-hour evaluation period (1 block per hour, 1 stage, 1 scenario).
 *   - Demand peak between hours 18-23 with a profile similar to the IEEE 9-bus
 *     case.
 *   - Total peak demand <= 90 % of total installed generation capacity.
 *   - Line limits on l1_2 and l1_5 (the only two lines leaving bus 1) are set
 *     so that their combined capacity equals the cost-optimal dispatch of G1,
 *     forcing both lines to saturate simultaneously at the evening peak.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

// clang-format off
static constexpr std::string_view ieee14_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": true,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [
      {"uid": 1,  "duration": 1},
      {"uid": 2,  "duration": 1},
      {"uid": 3,  "duration": 1},
      {"uid": 4,  "duration": 1},
      {"uid": 5,  "duration": 1},
      {"uid": 6,  "duration": 1},
      {"uid": 7,  "duration": 1},
      {"uid": 8,  "duration": 1},
      {"uid": 9,  "duration": 1},
      {"uid": 10, "duration": 1},
      {"uid": 11, "duration": 1},
      {"uid": 12, "duration": 1},
      {"uid": 13, "duration": 1},
      {"uid": 14, "duration": 1},
      {"uid": 15, "duration": 1},
      {"uid": 16, "duration": 1},
      {"uid": 17, "duration": 1},
      {"uid": 18, "duration": 1},
      {"uid": 19, "duration": 1},
      {"uid": 20, "duration": 1},
      {"uid": 21, "duration": 1},
      {"uid": 22, "duration": 1},
      {"uid": 23, "duration": 1},
      {"uid": 24, "duration": 1}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 24, "active": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ]
  },
  "system": {
    "name": "ieee_14",
    "bus_array": [
      {"uid": 1,  "name": "b1"},
      {"uid": 2,  "name": "b2"},
      {"uid": 3,  "name": "b3"},
      {"uid": 4,  "name": "b4"},
      {"uid": 5,  "name": "b5"},
      {"uid": 6,  "name": "b6"},
      {"uid": 7,  "name": "b7"},
      {"uid": 8,  "name": "b8"},
      {"uid": 9,  "name": "b9"},
      {"uid": 10, "name": "b10"},
      {"uid": 11, "name": "b11"},
      {"uid": 12, "name": "b12"},
      {"uid": 13, "name": "b13"},
      {"uid": 14, "name": "b14"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1",  "bus": "b1", "pmin": 0, "pmax": 260, "gcost": 20, "capacity": 260},
      {"uid": 2, "name": "g2",  "bus": "b2", "pmin": 0, "pmax": 130, "gcost": 35, "capacity": 130},
      {"uid": 3, "name": "g3",  "bus": "b3", "pmin": 0, "pmax": 130, "gcost": 0,  "capacity": 130},
      {"uid": 4, "name": "g6",  "bus": "b6", "pmin": 0, "pmax": 100, "gcost": 40, "capacity": 100},
      {"uid": 5, "name": "g8",  "bus": "b8", "pmin": 0, "pmax": 80,  "gcost": 45, "capacity": 80}
    ],
    "demand_array": [
      {
        "uid": 1, "name": "d2", "bus": "b2",
        "lmax": [[22.5, 21.6, 21.2, 21.2, 21.6, 23.4, 26.1, 30.6, 33.3, 34.7, 35.6, 36.0,
                  36.5, 36.0, 35.1, 34.2, 34.7, 35.6, 39.2, 41.9, 43.7, 45.0, 40.1, 37.4]]
      },
      {
        "uid": 2, "name": "d3", "bus": "b3",
        "lmax": [[54.0, 51.8, 50.8, 50.8, 51.8, 56.2, 62.6, 73.4, 79.9, 83.2, 85.3, 86.4,
                  87.5, 86.4, 84.2, 82.1, 83.2, 85.3, 93.9, 100.4, 104.8, 108.0, 96.1, 89.6]]
      },
      {
        "uid": 3, "name": "d4", "bus": "b4",
        "lmax": [[45.0, 43.2, 42.3, 42.3, 43.2, 46.8, 52.2, 61.2, 66.6, 69.3, 71.1, 72.0,
                  72.9, 72.0, 70.2, 68.4, 69.3, 71.1, 78.3, 83.7, 87.3, 90.0, 80.1, 74.7]]
      },
      {
        "uid": 4, "name": "d5", "bus": "b5",
        "lmax": [[11.0, 10.6, 10.3, 10.3, 10.6, 11.4, 12.8, 15.0, 16.3, 16.9, 17.4, 17.6,
                  17.8, 17.6, 17.2, 16.7, 16.9, 17.4, 19.1, 20.5, 21.3, 22.0, 19.6, 18.3]]
      },
      {
        "uid": 5, "name": "d6", "bus": "b6",
        "lmax": [[19.0, 18.2, 17.9, 17.9, 18.2, 19.8, 22.0, 25.8, 28.1, 29.3, 30.0, 30.4,
                  30.8, 30.4, 29.6, 28.9, 29.3, 30.0, 33.1, 35.3, 36.9, 38.0, 33.8, 31.5]]
      },
      {
        "uid": 6, "name": "d9", "bus": "b9",
        "lmax": [[34.0, 32.6, 31.9, 31.9, 32.6, 35.4, 39.4, 46.2, 50.3, 52.4, 53.7, 54.4,
                  55.1, 54.4, 53.0, 51.7, 52.4, 53.7, 59.2, 63.2, 65.9, 68.0, 60.5, 56.4]]
      },
      {
        "uid": 7, "name": "d10", "bus": "b10",
        "lmax": [[11.0, 10.6, 10.3, 10.3, 10.6, 11.4, 12.8, 15.0, 16.3, 16.9, 17.4, 17.6,
                  17.8, 17.6, 17.2, 16.7, 16.9, 17.4, 19.1, 20.5, 21.3, 22.0, 19.6, 18.3]]
      },
      {
        "uid": 8, "name": "d11", "bus": "b11",
        "lmax": [[6.0, 5.8, 5.6, 5.6, 5.8, 6.2, 7.0, 8.2, 8.9, 9.2, 9.5, 9.6,
                  9.7, 9.6, 9.4, 9.1, 9.2, 9.5, 10.4, 11.2, 11.6, 12.0, 10.7, 9.9]]
      },
      {
        "uid": 9, "name": "d12", "bus": "b12",
        "lmax": [[7.0, 6.7, 6.6, 6.6, 6.7, 7.3, 8.1, 9.5, 10.3, 10.8, 11.1, 11.2,
                  11.3, 11.2, 10.9, 10.6, 10.8, 11.1, 12.2, 13.0, 13.6, 14.0, 12.5, 11.6]]
      },
      {
        "uid": 10, "name": "d13", "bus": "b13",
        "lmax": [[15.0, 14.4, 14.1, 14.1, 14.4, 15.6, 17.4, 20.4, 22.2, 23.1, 23.7, 24.0,
                  24.3, 24.0, 23.4, 22.8, 23.1, 23.7, 26.1, 27.9, 29.1, 30.0, 26.7, 24.9]]
      },
      {
        "uid": 11, "name": "d14", "bus": "b14",
        "lmax": [[17.0, 16.3, 15.9, 15.9, 16.3, 17.7, 19.7, 23.1, 25.2, 26.2, 26.9, 27.2,
                  27.5, 27.2, 26.5, 25.8, 26.2, 26.9, 29.6, 31.6, 33.0, 34.0, 30.3, 28.2]]
      }
    ],
    "line_array": [
      {"uid": 1,  "name": "l1_2",   "bus_a": "b1",  "bus_b": "b2",  "reactance": 0.05917, "tmax_ab": 150, "tmax_ba": 150},
      {"uid": 2,  "name": "l1_5",   "bus_a": "b1",  "bus_b": "b5",  "reactance": 0.22304, "tmax_ab": 75,  "tmax_ba": 75},
      {"uid": 3,  "name": "l2_3",   "bus_a": "b2",  "bus_b": "b3",  "reactance": 0.19797, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 4,  "name": "l2_4",   "bus_a": "b2",  "bus_b": "b4",  "reactance": 0.17632, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 5,  "name": "l2_5",   "bus_a": "b2",  "bus_b": "b5",  "reactance": 0.17388, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 6,  "name": "l3_4",   "bus_a": "b3",  "bus_b": "b4",  "reactance": 0.17103, "tmax_ab": 60,  "tmax_ba": 60},
      {"uid": 7,  "name": "l4_5",   "bus_a": "b4",  "bus_b": "b5",  "reactance": 0.04211, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 8,  "name": "l4_7",   "bus_a": "b4",  "bus_b": "b7",  "reactance": 0.20912, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 9,  "name": "l4_9",   "bus_a": "b4",  "bus_b": "b9",  "reactance": 0.55618, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 10, "name": "l5_6",   "bus_a": "b5",  "bus_b": "b6",  "reactance": 0.25202, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 11, "name": "l6_11",  "bus_a": "b6",  "bus_b": "b11", "reactance": 0.19890, "tmax_ab": 35,  "tmax_ba": 35},
      {"uid": 12, "name": "l6_12",  "bus_a": "b6",  "bus_b": "b12", "reactance": 0.25581, "tmax_ab": 25,  "tmax_ba": 25},
      {"uid": 13, "name": "l6_13",  "bus_a": "b6",  "bus_b": "b13", "reactance": 0.13027, "tmax_ab": 50,  "tmax_ba": 50},
      {"uid": 14, "name": "l7_8",   "bus_a": "b7",  "bus_b": "b8",  "reactance": 0.17615, "tmax_ab": 50,  "tmax_ba": 50},
      {"uid": 15, "name": "l7_9",   "bus_a": "b7",  "bus_b": "b9",  "reactance": 0.11001, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 16, "name": "l9_10",  "bus_a": "b9",  "bus_b": "b10", "reactance": 0.08450, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 17, "name": "l9_14",  "bus_a": "b9",  "bus_b": "b14", "reactance": 0.27038, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 18, "name": "l10_11", "bus_a": "b10", "bus_b": "b11", "reactance": 0.19207, "tmax_ab": 30,  "tmax_ba": 30},
      {"uid": 19, "name": "l12_13", "bus_a": "b12", "bus_b": "b13", "reactance": 0.19988, "tmax_ab": 20,  "tmax_ba": 20},
      {"uid": 20, "name": "l13_14", "bus_a": "b13", "bus_b": "b14", "reactance": 0.34802, "tmax_ab": 30,  "tmax_ba": 30}
    ],
    "generator_profile_array": [
      {
        "uid": 1,
        "name": "gp_solar",
        "generator": "g3",
        "profile": [[[
          0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.05, 0.15, 0.35, 0.55, 0.75, 0.9,
          1.0, 0.95, 0.85, 0.7, 0.5, 0.3,
          0.1, 0.02, 0.0, 0.0, 0.0, 0.0
        ]]]
      }
    ]
  }
})";
// clang-format on

TEST_CASE("IEEE 14-bus - JSON parse and structure check")
{
  auto planning = daw::json::from_json<Planning>(ieee14_json);

  CHECK(planning.system.name == "ieee_14");
  CHECK(planning.system.bus_array.size() == 14);
  CHECK(planning.system.generator_array.size() == 5);
  CHECK(planning.system.demand_array.size() == 11);
  CHECK(planning.system.line_array.size() == 20);
  CHECK(planning.system.generator_profile_array.size() == 1);

  // Verify 24-hour simulation period (same as ieee_9b)
  CHECK(planning.simulation.block_array.size() == 24);
  CHECK(planning.simulation.stage_array.size() == 1);
  CHECK(planning.simulation.scenario_array.size() == 1);

  // Verify solar generator (g3) profile name
  CHECK(planning.system.generator_profile_array[0].name == "gp_solar");
}

TEST_CASE("IEEE 14-bus - LP solve")
{
  Planning base;
  base.merge(daw::json::from_json<Planning>(ieee14_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}
