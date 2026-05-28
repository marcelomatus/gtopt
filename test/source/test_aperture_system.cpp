// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_aperture_system.cpp
 * @brief     Prototype tests for the SDDP `aperture_system` feature.
 *
 * The aperture_system lets the SDDP backward pass solve a separate, simplified
 * system (the "aperture system") per aperture, while the forward pass keeps the
 * full-detail system.  See `docs`/the plan and `system_file_loader.hpp`.
 *
 * This file currently covers the landed foundation:
 *   - the `aperture_system_file` data-model fields parse/round-trip through
 *     daw::json on `Phase`, `SddpOptions`, and `CascadeLevel`, and nest
 *     correctly inside a full `Planning` JSON; and
 *   - the `resolve_aperture_system_file` precedence helper
 *     (Phase override → global default → none).
 *
 * The deeper two-LP-per-cell cases (no-op equivalence, exact single-bus
 * reduction, simplified-but-feasible cut, dual cut-install) require the
 * second LP cache + parallel state registry + dual cut-install, which are not
 * yet wired; they are scaffolded below as commented TODOs so the intended
 * behaviour is recorded next to the tests that will assert it.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/cascade_options.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_cascade_options.hpp>
#include <gtopt/json/json_phase.hpp>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/system_file_loader.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE("aperture_system_file: data-model bindings round-trip")
{
  SUBCASE("Phase")
  {
    const std::string_view js =
        R"({"uid": 1, "first_stage": 0, "count_stage": 4,
            "aperture_system_file": "laja_reduced.json"})";
    const auto ph = daw::json::from_json<Phase>(js);
    REQUIRE(ph.aperture_system_file.has_value());
    CHECK(*ph.aperture_system_file == "laja_reduced.json");

    // to_json → from_json identity preserves the field.
    const std::string round = daw::json::to_json(ph);
    const auto rt = daw::json::from_json<Phase>(std::string_view {round});
    REQUIRE(rt.aperture_system_file.has_value());
    CHECK(*rt.aperture_system_file == "laja_reduced.json");
  }

  SUBCASE("Phase without the field stays unset")
  {
    const std::string_view js =
        R"({"uid": 1, "first_stage": 0, "count_stage": 4})";
    const auto ph = daw::json::from_json<Phase>(js);
    CHECK_FALSE(ph.aperture_system_file.has_value());
  }

  SUBCASE("SddpOptions")
  {
    const std::string_view js =
        R"({"aperture_system_file": "global_reduced.json"})";
    const auto so = daw::json::from_json<SddpOptions>(js);
    REQUIRE(so.aperture_system_file.has_value());
    CHECK(*so.aperture_system_file == "global_reduced.json");

    const std::string round = daw::json::to_json(so);
    const auto rt = daw::json::from_json<SddpOptions>(std::string_view {round});
    REQUIRE(rt.aperture_system_file.has_value());
    CHECK(*rt.aperture_system_file == "global_reduced.json");
  }

  SUBCASE("CascadeLevel")
  {
    const std::string_view js =
        R"({"name": "L0", "system_file": "fwd.json",
            "aperture_system_file": "bwd.json"})";
    const auto cl = daw::json::from_json<CascadeLevel>(js);
    REQUIRE(cl.system_file.has_value());
    CHECK(*cl.system_file == "fwd.json");
    REQUIRE(cl.aperture_system_file.has_value());
    CHECK(*cl.aperture_system_file == "bwd.json");

    const std::string round = daw::json::to_json(cl);
    const auto rt =
        daw::json::from_json<CascadeLevel>(std::string_view {round});
    REQUIRE(rt.aperture_system_file.has_value());
    CHECK(*rt.aperture_system_file == "bwd.json");
  }
}

TEST_CASE("aperture_system_file: CascadeLevel.merge overlay wins")
{
  CascadeLevel base {.aperture_system_file = OptName {"base.json"}};
  base.merge(CascadeLevel {.aperture_system_file = OptName {"overlay.json"}});
  REQUIRE(base.aperture_system_file.has_value());
  CHECK(*base.aperture_system_file == "overlay.json");

  // Unset overlay leaves the base value intact.
  CascadeLevel keep {.aperture_system_file = OptName {"keep.json"}};
  keep.merge(CascadeLevel {});
  REQUIRE(keep.aperture_system_file.has_value());
  CHECK(*keep.aperture_system_file == "keep.json");
}

TEST_CASE("aperture_system_file: nests inside a full Planning JSON")
{
  const auto* const js = R"({
    "options": {
      "sddp_options": {"aperture_system_file": "global_reduced.json"}
    },
    "simulation": {
      "phase_array": [
        {"uid": 1, "first_stage": 0, "count_stage": 2,
         "aperture_system_file": "phase1_reduced.json"},
        {"uid": 2, "first_stage": 2, "count_stage": 2}
      ]
    }
  })";
  const auto planning = parse_planning_json(js);

  REQUIRE(planning.options.sddp_options.aperture_system_file.has_value());
  CHECK(*planning.options.sddp_options.aperture_system_file
        == "global_reduced.json");

  REQUIRE(planning.simulation.phase_array.size() == 2);
  REQUIRE(planning.simulation.phase_array[0].aperture_system_file.has_value());
  CHECK(*planning.simulation.phase_array[0].aperture_system_file
        == "phase1_reduced.json");
  CHECK_FALSE(
      planning.simulation.phase_array[1].aperture_system_file.has_value());
}

TEST_CASE("resolve_aperture_system_file: Phase → global → none precedence")
{
  const OptName phase_file {"phase.json"};
  const OptName global_file {"global.json"};
  const OptName unset {};
  const OptName blank {""};

  SUBCASE("phase override wins over global")
  {
    const auto r = resolve_aperture_system_file(phase_file, global_file);
    REQUIRE(r.has_value());
    CHECK(*r == "phase.json");
  }

  SUBCASE("falls back to global when phase unset")
  {
    const auto r = resolve_aperture_system_file(unset, global_file);
    REQUIRE(r.has_value());
    CHECK(*r == "global.json");
  }

  SUBCASE("neither set → empty (use regular forward system)")
  {
    CHECK_FALSE(resolve_aperture_system_file(unset, unset).has_value());
  }

  SUBCASE("empty string is treated as unset (does not shadow global)")
  {
    const auto r = resolve_aperture_system_file(blank, global_file);
    REQUIRE(r.has_value());
    CHECK(*r == "global.json");

    CHECK_FALSE(resolve_aperture_system_file(blank, blank).has_value());
  }
}

TEST_CASE("aperture_system build: parallel SystemLP + state registry")
{
  namespace fs = std::filesystem;

  // Serialise a copy of the base planning to use as the (reduced) aperture
  // system file.  Using the same content makes this the no-op-equivalence
  // base: the aperture system is structurally identical to the forward one.
  const auto base = make_3phase_hydro_planning();
  const std::string planning_json = daw::json::to_json(base);
  const auto tmp =
      fs::temp_directory_path() / "gtopt_aperture_system_build_test.json";
  {
    std::ofstream out(tmp);
    out << planning_json;
  }

  SUBCASE("global aperture_system_file builds aperture systems")
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    planning.options.sddp_options.aperture_system_file = OptName {tmp.string()};

    PlanningLP planning_lp(std::move(planning));

    // A distinct aperture SystemLP exists for the cell.
    auto* ap = planning_lp.aperture_system(SceneIndex {0}, PhaseIndex {0});
    REQUIRE(ap != nullptr);
    CHECK(ap != &planning_lp.system(SceneIndex {0}, PhaseIndex {0}));
    CHECK(ap->kind() == SystemKind::aperture);

    // The reservoir state variable is registered in the *parallel* aperture
    // registry, separate from (and non-colliding with) the forward one.
    const auto& ap_vars = planning_lp.simulation().state_variables(
        SceneIndex {0}, PhaseIndex {0}, SystemKind::aperture);
    const auto& fwd_vars = planning_lp.simulation().state_variables(
        SceneIndex {0}, PhaseIndex {0}, SystemKind::forward);
    CHECK_FALSE(ap_vars.empty());
    CHECK_FALSE(fwd_vars.empty());

    // The aperture cross-phase state link was tightened in the *aperture*
    // registry: the phase-0 aperture reservoir producer carries a dependent
    // variable landing in phase 1.  This is the foundation the (future) cut
    // recursion relies on to remap dependent columns into the aperture LP.
    bool ap_link_to_phase1 = false;
    for (const auto& [key, svar] : ap_vars) {
      for (const auto& dep : svar.dependent_variables()) {
        if (dep.scene_index() == SceneIndex {0}
            && dep.phase_index() == PhaseIndex {1})
        {
          ap_link_to_phase1 = true;
        }
      }
    }
    CHECK(ap_link_to_phase1);
  }

  SUBCASE("no aperture_system_file → aperture_system() is null")
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    PlanningLP planning_lp(std::move(planning));
    CHECK(planning_lp.aperture_system(SceneIndex {0}, PhaseIndex {0})
          == nullptr);
    // The aperture registry stays empty when no file is in effect.
    CHECK(planning_lp.simulation()
              .state_variables(
                  SceneIndex {0}, PhaseIndex {0}, SystemKind::aperture)
              .empty());
  }

  fs::remove(tmp);
}

// ── Deep two-LP-per-cell cases (pending implementation) ──────────────────────
//
// These require Tasks 3–6 (PlanningLP second LP cache, parallel α/state
// registry under a `SystemKind` discriminator, backward-pass clone-source
// switch, and dual cut-install with a forward→aperture column remap).  They
// are intentionally left as documented TODOs so the contracts are recorded:
//
//   TEST_CASE("aperture_system identical to forward reproduces baseline cut")
//     // Run A: no aperture_system.  Run B: aperture_system == forward JSON.
//     // CHECK identical LB/UB trajectory and identical installed cut rows.
//
//   TEST_CASE("single-bus aperture_system matches when network is non-binding")
//     // 2-bus forward, single-bus aperture reduction, no congestion.
//     // CHECK cut coeff on reservoir state == full-network cut coeff.
//
//   TEST_CASE("simplified aperture_system yields a feasible, bounded cut")
//     // Aggregated-thermal aperture system.  CHECK all apertures feasible,
//     // cut installed on BOTH forward & aperture LP of the source phase,
//     // LB finite and <= UB.

// NOLINTEND(bugprone-unchecked-optional-access)
