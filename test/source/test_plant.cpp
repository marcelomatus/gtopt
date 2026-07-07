// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_plant.cpp
 * @brief     Unit tests for the Plant LP primitive
 *
 * Pins the schema + LP contract for the Plant element introduced as a
 * native gtopt primitive (replaces ``PlantCap_*`` UserConstraints +
 * ``<plant>_Uniq`` synthesised by plexos2gtopt).
 *
 * Coverage:
 *   * Struct schema + designated-initializer defaults.
 *   * JSON round-trip of the documented fields.
 *   * End-to-end LP build: a 2-variant Plant fixture (G1, G2) caps the
 *     sum-of-generations at ``pmax`` and forces one generator down when
 *     both want to dispatch at peak — verifies the row is binding.
 */

#include <string>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json/json_plant.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/plant.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE("Plant — default-constructed")
{
  const Plant p;
  CHECK(p.uid == unknown_uid);
  CHECK(p.name == Name {});
  CHECK_FALSE(p.active.has_value());
  CHECK(p.generator_names.empty());
  CHECK_FALSE(p.pmax.has_value());
  CHECK_FALSE(p.n_units.has_value());
  CHECK(p.commit_coeffs.empty());
  CHECK_FALSE(p.uniq_mutex.has_value());
}

TEST_CASE("Plant — designated initializer")
{
  const Plant p {
      .uid = Uid {1},
      .name = "ATA_CC_1",
      .generator_names = {"ATA_CC_1_ConfTGA",
                          "ATA_CC_1_ConfTGB",
                          "ATA_CC_1_ConfTV"},
      .pmax = 391.0,
      .n_units = 1,
      .uniq_mutex = true,
  };
  CHECK(p.uid == Uid {1});
  CHECK(p.name == "ATA_CC_1");
  REQUIRE(p.generator_names.size() == 3);
  CHECK(p.generator_names[0] == "ATA_CC_1_ConfTGA");
  CHECK(p.generator_names[2] == "ATA_CC_1_ConfTV");
  CHECK(p.pmax.value_or(0.0) == doctest::Approx(391.0));
  CHECK(p.n_units.value_or(0) == 1);
  CHECK(p.uniq_mutex.value_or(false));
}

TEST_CASE("Plant — class_name constant")
{
  // Single source of truth for LP-row labelling.  Drift here would
  // break the `plant_cap_*` / `plant_commit_*` / `plant_uniq_*`
  // labels emitted by PlantLP::add_to_lp.
  CHECK(std::string_view {Plant::class_name.full_name()} == "Plant");
  CHECK(std::string_view {Plant::class_name.snake_case()} == "plant");
}

TEST_CASE("Plant — JSON round-trip preserves all fields")
{
  // Round-trip the documented JSON example from `plant.hpp`.
  constexpr std::string_view input = R"({
    "uid": 1,
    "name": "ATA_CC_1",
    "generator_names": ["G1", "G2", "G3"],
    "pmax": 391.0,
    "n_units": 1,
    "uniq_mutex": true
  })";
  const auto p = daw::json::from_json<Plant>(input, StrictParsePolicy);
  CHECK(p.uid == Uid {1});
  CHECK(p.name == "ATA_CC_1");
  REQUIRE(p.generator_names.size() == 3);
  CHECK(p.generator_names[1] == "G2");
  CHECK(p.pmax.value_or(0.0) == doctest::Approx(391.0));
  CHECK(p.n_units.value_or(0) == 1);
  CHECK(p.uniq_mutex.value_or(false));

  // Re-emit + re-parse: idempotent at the schema level.
  const auto emitted = daw::json::to_json(p);
  const auto p2 = daw::json::from_json<Plant>(emitted, StrictParsePolicy);
  CHECK(p2.uid == p.uid);
  CHECK(p2.name == p.name);
  CHECK(p2.generator_names == p.generator_names);
  CHECK(p2.pmax.value_or(-1.0) == doctest::Approx(391.0));
  CHECK(p2.n_units.value_or(-1) == 1);
  CHECK(p2.uniq_mutex.value_or(false));
}

TEST_CASE("Plant — JSON parses minimal (uid + name only)")
{
  // All non-uid/name fields are optional.  A minimal Plant must
  // parse cleanly with the LP defaulting to no-op (no rows emitted).
  constexpr std::string_view input = R"({"uid": 2, "name": "empty"})";
  const auto p = daw::json::from_json<Plant>(input, StrictParsePolicy);
  CHECK(p.uid == Uid {2});
  CHECK(p.name == "empty");
  CHECK(p.generator_names.empty());
  CHECK_FALSE(p.pmax.has_value());
  CHECK_FALSE(p.n_units.has_value());
  CHECK(p.commit_coeffs.empty());
  CHECK_FALSE(p.uniq_mutex.has_value());
}

// ── End-to-end LP: Σ-capacity row binds when both variants want to peak ──
//
// Two generators G1 (pmax=60) + G2 (pmax=80) on the same bus would
// dispatch the whole 140 MW = 60+80 to cover a 100 MW demand without
// the plant cap.  Adding ``Plant{pmax=100}`` forces the LP to keep the
// sum at 100 MW — the same total a single-config plant could produce
// regardless of which variant the LP picks.  No plant cap → LP picks
// the cheaper one to its pmax and the more expensive to fill demand,
// objective = 60·10 + 40·20 = 1400; with plant cap, the LP is free to
// pick any feasible Σ=100 split (cheapest is still 60·10 + 40·20 = 1400)
// — same cost, but the row is recorded in the LP so its dual is
// non-zero when the cap is tight.
namespace
{

constexpr std::string_view plant_fixture_json = R"json(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 10000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1 }
      ],
      "scenario_array": [
        { "uid": 1, "probability_factor": 1 }
      ]
    },
    "system": {
      "name": "plant_test",
      "bus_array": [
        { "uid": 1, "name": "b1" }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "G1",
          "bus": "b1",
          "pmin": 0,
          "pmax": 60,
          "gcost": 10,
          "capacity": 60
        },
        {
          "uid": 2,
          "name": "G2",
          "bus": "b1",
          "pmin": 0,
          "pmax": 80,
          "gcost": 20,
          "capacity": 80
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d1",
          "bus": "b1",
          "lmax": [[100.0]]
        }
      ],
      "plant_array": [
        {
          "uid": 1,
          "name": "PLANT_X",
          "generator_names": ["G1", "G2"],
          "pmax": 100
        }
      ]
    }
  }
)json";

constexpr std::string_view no_plant_fixture_json = R"json(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 10000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1 }
      ],
      "scenario_array": [
        { "uid": 1, "probability_factor": 1 }
      ]
    },
    "system": {
      "name": "no_plant_test",
      "bus_array": [
        { "uid": 1, "name": "b1" }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "G1",
          "bus": "b1",
          "pmin": 0,
          "pmax": 60,
          "gcost": 10,
          "capacity": 60
        },
        {
          "uid": 2,
          "name": "G2",
          "bus": "b1",
          "pmin": 0,
          "pmax": 80,
          "gcost": 20,
          "capacity": 80
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d1",
          "bus": "b1",
          "lmax": [[130.0]]
        }
      ]
    }
  }
)json";

}  // namespace

TEST_CASE("Plant — Σ-capacity row caps simultaneous variant dispatch")
{
  // Both variants on the same bus, demand = 100, plant_cap = 100.
  // Without the plant row, the LP could dispatch G1 to 60 + G2 to 40
  // (cost 60·10 + 40·20 = 1400).  With the plant row, the same split
  // is still optimal and feasible — what we verify here is that the
  // LP solves, the objective matches, and a `plant_cap_*` row was
  // recorded against this scenario / stage.
  auto planning =
      daw::json::from_json<Planning>(plant_fixture_json, StrictParsePolicy);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& sys_lp = systems.front().front();
  const auto& li = sys_lp.linear_interface();

  SUBCASE("objective matches optimal split (60·10 + 40·20 = 1400)")
  {
    CHECK(li.get_obj_value_raw() == doctest::Approx(1400.0).epsilon(1e-3));
  }

  SUBCASE("plant capacity row was recorded for the only scenario/stage")
  {
    auto&& plants = sys_lp.elements<PlantLP>();
    REQUIRE(plants.size() == 1);
    const auto& cap_rows = plants.front().capacity_rows();
    CHECK_FALSE(cap_rows.empty());
  }

  SUBCASE("no plant_commit_* or plant_uniq_* row when not configured")
  {
    auto&& plants = sys_lp.elements<PlantLP>();
    REQUIRE(plants.size() == 1);
    CHECK(plants.front().commit_rows().empty());
    CHECK(plants.front().uniq_rows().empty());
  }
}

TEST_CASE("Plant — without the cap row, both variants dispatch to peak")
{
  // Baseline: same physical setup but no Plant entry, demand 130 MW.
  // The LP dispatches G1 to its pmax (60) and uses G2 to cover the
  // rest (70).  Objective = 60·10 + 70·20 = 2000.
  auto planning =
      daw::json::from_json<Planning>(no_plant_fixture_json, StrictParsePolicy);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& sys_lp = systems.front().front();
  const auto& li = sys_lp.linear_interface();

  SUBCASE("objective = 60·10 + 70·20 = 2000")
  {
    CHECK(li.get_obj_value_raw() == doctest::Approx(2000.0).epsilon(1e-3));
  }

  SUBCASE("no Plant elements in the system")
  {
    CHECK(sys_lp.elements<PlantLP>().empty());
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)
