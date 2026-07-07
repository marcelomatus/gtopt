// SPDX-License-Identifier: BSD-3-Clause
//
// End-to-end integration tests for the naming-dialects registry: prove
// that `parse_planning_json` actually invokes the alias-canonicalize
// hook and that the result is structurally identical to the canonical
// form.
//
// These tests are the safety net before refactoring the AMPL resolver
// (`source/element_column_resolver.cpp`) to also consult the registry.
// A regression in `parse_planning_json` (e.g. someone silently drops
// the `canonicalize_json_keys(...)` call) would otherwise pass every
// unit test we ship today.

#include <filesystem>
#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/names_registry.hpp>

using namespace gtopt;

namespace
{

// clang-format off
constexpr std::string_view kCanonicalPlanning = R"json(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        {
          "uid": 1,
          "duration": 1
        }
      ],
      "stage_array": [
        {
          "uid": 1,
          "first_block": 0,
          "count_block": 1,
          "active": 1
        }
      ],
      "scenario_array": [
        {
          "uid": 1,
          "probability_factor": 1
        }
      ]
    },
    "system": {
      "name": "naming_dialects_e2e",
      "bus_array": [
        {
          "uid": 1,
          "name": "b1"
        },
        {
          "uid": 2,
          "name": "b2"
        }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "g1",
          "bus": "b1",
          "pmin": 0,
          "pmax": 300,
          "gcost": 20
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d2",
          "bus": "b2",
          "lmax": [
            [
              150.0
            ]
          ]
        }
      ],
      "line_array": [
        {
          "uid": 1,
          "name": "l1_2",
          "bus_a": "b1",
          "bus_b": "b2",
          "reactance": 0.02,
          "tmax_ab": 300,
          "tmax_ba": 300
        }
      ]
    }
  }
)json";

// Same Planning, but every aliasable field on Generator / Demand /
// Line uses the §11 'modern' alias instead of the canonical key.
// Note: top-level `demand_fail_cost` stays canonical because the
// canonicalize hook also accepts canonical (no change required); the
// alias coverage in this fixture is on the per-element side.
constexpr std::string_view kAliasedPlanning = R"json(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "value_of_lost_load": 1000
      }
    },
    "simulation": {
      "block_array": [
        {
          "uid": 1,
          "duration": 1
        }
      ],
      "stage_array": [
        {
          "uid": 1,
          "first_block": 0,
          "count_block": 1,
          "active": 1
        }
      ],
      "scenario_array": [
        {
          "uid": 1,
          "probability_factor": 1
        }
      ]
    },
    "system": {
      "name": "naming_dialects_e2e",
      "bus_array": [
        {
          "uid": 1,
          "name": "b1"
        },
        {
          "uid": 2,
          "name": "b2"
        }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "g1",
          "bus": "b1",
          "min_power": 0,
          "max_power": 300,
          "marginal_cost": 20
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d2",
          "bus": "b2",
          "max_demand": [
            [
              150.0
            ]
          ]
        }
      ],
      "line_array": [
        {
          "uid": 1,
          "name": "l1_2",
          "bus_a": "b1",
          "bus_b": "b2",
          "reactance": 0.02,
          "max_flow_ab": 300,
          "max_flow_ba": 300
        }
      ]
    }
  }
)json";
// clang-format on

}  // namespace

TEST_CASE(  // NOLINT
    "parse_planning_json — aliased keys produce same Planning as canonical")
{
  const auto canonical = parse_planning_json(kCanonicalPlanning);
  const auto aliased = parse_planning_json(kAliasedPlanning);

  SUBCASE("system / element counts match")
  {
    CHECK(canonical.system.name == aliased.system.name);
    CHECK(canonical.system.bus_array.size() == aliased.system.bus_array.size());
    CHECK(canonical.system.generator_array.size()
          == aliased.system.generator_array.size());
    CHECK(canonical.system.demand_array.size()
          == aliased.system.demand_array.size());
    CHECK(canonical.system.line_array.size()
          == aliased.system.line_array.size());
  }

  SUBCASE("Generator alias keys populate the canonical struct members")
  {
    REQUIRE(!aliased.system.generator_array.empty());
    const auto& g_alias = aliased.system.generator_array[0];
    const auto& g_canon = canonical.system.generator_array[0];

    CHECK(g_alias.uid == g_canon.uid);
    CHECK(g_alias.name == g_canon.name);
    CHECK(g_alias.pmin.has_value());
    CHECK(g_alias.pmax.has_value());
    CHECK(g_alias.gcost.has_value());
    CHECK(g_canon.pmin.has_value());
    CHECK(g_canon.pmax.has_value());
    CHECK(g_canon.gcost.has_value());
  }

  SUBCASE("Demand alias keys populate the canonical struct members")
  {
    REQUIRE(!aliased.system.demand_array.empty());
    const auto& d_alias = aliased.system.demand_array[0];
    const auto& d_canon = canonical.system.demand_array[0];

    CHECK(d_alias.uid == d_canon.uid);
    CHECK(d_alias.lmax.has_value());
    CHECK(d_canon.lmax.has_value());
  }

  SUBCASE("Line alias keys populate the canonical struct members")
  {
    REQUIRE(!aliased.system.line_array.empty());
    const auto& l_alias = aliased.system.line_array[0];
    const auto& l_canon = canonical.system.line_array[0];

    CHECK(l_alias.uid == l_canon.uid);
    CHECK(l_alias.tmax_ab.has_value());
    CHECK(l_alias.tmax_ba.has_value());
    CHECK(l_canon.tmax_ab.has_value());
    CHECK(l_canon.tmax_ba.has_value());
  }

  SUBCASE("model_options alias keys populate the canonical option fields")
  {
    // The aliased fixture used `value_of_lost_load` instead of
    // `demand_fail_cost`; both should land in the same option.
    CHECK(canonical.options.model_options.demand_fail_cost.has_value());
    CHECK(aliased.options.model_options.demand_fail_cost.has_value());
    if (canonical.options.model_options.demand_fail_cost.has_value()
        && aliased.options.model_options.demand_fail_cost.has_value())
    {
      CHECK(
          *canonical.options.model_options.demand_fail_cost
          == doctest::Approx(*aliased.options.model_options.demand_fail_cost));
    }
  }

  SUBCASE("byte-equal Generator JSON round-trip — strongest equivalence")
  {
    // `Planning` has no deducible json_data_contract (it's parsed via
    // json_member_list applied to a Planning ctor, not to_json-able),
    // but individual elements *are* round-trippable.  Serialising
    // both sides' Generator[0] and comparing the strings byte-for-byte
    // proves the alias parse produced the same value, not just the
    // same shape.  A regression that drops the canonicalize hook
    // would cause the alias-side parse to throw (StrictParsePolicy
    // rejects unknown keys) and this subcase would never execute.
    REQUIRE(!aliased.system.generator_array.empty());
    REQUIRE(!canonical.system.generator_array.empty());
    const auto alias_gen =
        daw::json::to_json(aliased.system.generator_array[0]);
    const auto canon_gen =
        daw::json::to_json(canonical.system.generator_array[0]);
    CHECK(alias_gen == canon_gen);
  }

  SUBCASE("byte-equal Line JSON round-trip")
  {
    REQUIRE(!aliased.system.line_array.empty());
    REQUIRE(!canonical.system.line_array.empty());
    const auto alias_line = daw::json::to_json(aliased.system.line_array[0]);
    const auto canon_line = daw::json::to_json(canonical.system.line_array[0]);
    CHECK(alias_line == canon_line);
  }

  SUBCASE("byte-equal Demand JSON round-trip")
  {
    REQUIRE(!aliased.system.demand_array.empty());
    REQUIRE(!canonical.system.demand_array.empty());
    const auto alias_dem = daw::json::to_json(aliased.system.demand_array[0]);
    const auto canon_dem = daw::json::to_json(canonical.system.demand_array[0]);
    CHECK(alias_dem == canon_dem);
  }
}

TEST_CASE(  // NOLINT
    "parse_planning_json — canonical keys round-trip through the hook")
{
  // Sanity guard: the canonicalize hook must be a no-op on already-
  // canonical input.  Confirms the hook doesn't drop or rewrite any
  // canonical key (would mean an alias accidentally collides with a
  // canonical name).
  const auto first = parse_planning_json(kCanonicalPlanning);
  REQUIRE(!first.system.generator_array.empty());
  REQUIRE(!first.system.demand_array.empty());
  REQUIRE(!first.system.line_array.empty());

  CHECK(first.system.generator_array[0].pmax.has_value());
  CHECK(first.system.generator_array[0].gcost.has_value());
  CHECK(first.system.demand_array[0].lmax.has_value());
  CHECK(first.system.line_array[0].tmax_ab.has_value());
}

TEST_CASE("NamesRegistry — load from shipped on-disk file")  // NOLINT
{
  // Bypass the singleton + compiled-in fallback path; force a file
  // read of the shipped `share/gtopt/naming_dialects.json`. This
  // catches install-path / find-file regressions that the
  // singleton-only test in test_names_registry.cpp would mask
  // (because the singleton falls back to the compiled-in copy when
  // the file path is unreachable).
  const auto path = find_names_file();
  REQUIRE(path.has_value());
  REQUIRE(std::filesystem::exists(*path));

  const NamesRegistry r {*path};

  SUBCASE("source_path is set when constructed from a file")
  {
    REQUIRE(r.source_path().has_value());
    CHECK(*r.source_path() == *path);
  }

  SUBCASE("file-loaded registry resolves the shipped Generator aliases")
  {
    CHECK(r.canonical_for("marginal_cost").value_or("") == "gcost");
    CHECK(r.canonical_for("max_power").value_or("") == "pmax");
  }

  SUBCASE("file-loaded registry resolves the shipped Demand aliases")
  {
    CHECK(r.canonical_for("max_demand").value_or("") == "lmax");
  }

  SUBCASE("file-loaded registry resolves the shipped Line aliases")
  {
    CHECK(r.canonical_for("max_flow_ab").value_or("") == "tmax_ab");
  }

  SUBCASE("file-loaded registry resolves the §11 option renames")
  {
    CHECK(r.canonical_for("value_of_lost_load").value_or("")
          == "demand_fail_cost");
    CHECK(r.canonical_for("copper_plate").value_or("") == "use_single_bus");
  }
}
