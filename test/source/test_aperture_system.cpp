// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_aperture_system.cpp
 * @brief     Prototype tests for the SDDP `aperture_system` feature.
 *
 * The aperture_system lets the SDDP backward pass solve a separate, simplified
 * system (the "aperture system") per aperture, while the forward pass keeps the
 * full-detail system.  See `docs`/the plan and `system_file_loader.hpp`.
 *
 * This file covers:
 *   - the `aperture_system_file` data-model fields parse/round-trip through
 *     daw::json on `Phase`, `SddpOptions`, and `CascadeLevel`, and nest
 *     correctly inside a full `Planning` JSON;
 *   - the `resolve_aperture_system_file` precedence helper
 *     (Phase override → global default → none); and
 *   - `PlanningLP` build-time wiring: a global file builds aperture
 *     `SystemLP`s into the parallel `SystemKind::aperture` registry (with the
 *     cross-phase link tightened), monolithic mode builds none, and a
 *     per-phase override builds only that phase's aperture system.
 *
 * The end-to-end cut-recursion correctness gate (full SDDP solve, aperture ==
 * forward ⇒ identical bounds) lives in `test_aperture_lp.cpp`; the file-loader
 * branch tests live in `test_system_file_loader.cpp`.
 */

#include <cstdlib>
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

using namespace gtopt;
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
  // Honour the project $TMPDIR convention (never hardcode /tmp).
  const char* const td = std::getenv("TMPDIR");
  const auto tmp =
      (td != nullptr && *td != '\0' ? fs::path(td) : fs::temp_directory_path())
      / "gtopt_aperture_system_build_test.json";
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

  SUBCASE("monolithic method → aperture systems are not built")
  {
    // Aperture systems are an SDDP backward-pass construct; build_aperture
    // _systems short-circuits for the monolithic method even when a file is
    // set, so no second LP is created.
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::monolithic;
    planning.options.sddp_options.aperture_system_file = OptName {tmp.string()};
    PlanningLP planning_lp(std::move(planning));
    CHECK(planning_lp.aperture_system(SceneIndex {0}, PhaseIndex {0})
          == nullptr);
  }

  SUBCASE("per-phase aperture_system_file builds only that phase")
  {
    // No global default; only phase index 1 carries an override.  The
    // resolution chain must build an aperture system for phase 1 and leave
    // phases 0 and 2 without one.
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    REQUIRE(planning.simulation.phase_array.size() == 3);
    planning.simulation.phase_array[1].aperture_system_file =
        OptName {tmp.string()};

    PlanningLP planning_lp(std::move(planning));
    CHECK(planning_lp.aperture_system(SceneIndex {0}, PhaseIndex {0})
          == nullptr);
    CHECK(planning_lp.aperture_system(SceneIndex {0}, PhaseIndex {1})
          != nullptr);
    CHECK(planning_lp.aperture_system(SceneIndex {0}, PhaseIndex {2})
          == nullptr);
  }

  fs::remove(tmp);
}

// The end-to-end cut-recursion correctness gate ("aperture_system equivalence:
// aperture==forward reproduces baseline") lives in `test_aperture_lp.cpp`,
// next to the `make_2phase_aperture_planning` fixture it reuses: it runs a full
// SDDP solve with and without an identical aperture system and asserts the two
// produce the same LB/UB trajectory.  The file-loader branch tests live in
// `test_system_file_loader.cpp`.
//
// Still-open follow-ups (deferred by design, see the feature plan): a
// genuinely-SIMPLIFIED (non-identical but state-topology-preserving) aperture
// system that converges to a different-but-valid bound, and applying the loaded
// file's `model_options` (single-bus / no-Kirchhoff backward model).

// NOLINTEND(bugprone-unchecked-optional-access)
