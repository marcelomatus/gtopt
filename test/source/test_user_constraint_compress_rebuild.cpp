// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_user_constraint_compress_rebuild.cpp
 * @brief     User constraints resolve under the compress rebuild path
 *            (SDDP / cascade / write_out), not just monolithic.
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * Regression for: a user constraint that references an element output
 * attribute — e.g. `generator('g1').generation` — resolved under
 * `method=monolithic` but FAILED under `method=sddp` / `method=cascade`
 * with "unknown attribute 'generation' on generator 'g1'".
 *
 * Root cause: the production solve path frees the per-cell AMPL
 * variable/metadata registry after the initial flatten
 * (`LpMatrixOptions::release_ampl_after_flatten` →
 * `SimulationLP::release_ampl_cell`).  Under `low_memory=compress`
 * (the SDDP / cascade default) a later rebuild flatten — driven by
 * `write_out()` via `SystemLP::rebuild_collections_if_needed()` — re-runs
 * `UserConstraintLP::add_to_lp`, which resolves element columns through
 * that registry.  The rebuild ran as a "silent" pass that skipped AMPL
 * registration, so the registry was empty and resolution threw.
 * Monolithic forces `low_memory=off`, never rebuilds, and so never hit it.
 *
 * The fix replays the AMPL variable/metadata registry on the rebuild pass
 * (`SystemContext::set_replay_ampl_registry`).  These tests drive the
 * exact production path: `low_memory=compress` + `release_ampl_after_flatten`
 * + a generator user constraint + `write_out()`.
 */

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions.
namespace uc_compress_rebuild_test
{
namespace
{

// clang-format off

/// Single-bus, single-stage system.  `uc_body` is spliced into
/// `user_constraint_array` verbatim so each test varies the constraint(s).
auto make_json(std::string_view uc_body) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": true,
        "demand_fail_cost": 1000,
        "scale_objective": 1
      }}
    }},
    "simulation": {{
      "block_array": [ {{ "uid": 1, "duration": 1 }} ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "uc_compress_rebuild",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100,
           "gcost": 20, "capacity": 100 }},
        {{ "uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100,
           "gcost": 30, "capacity": 100 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1", "lmax": [ [ 90.0 ] ] }}
      ],
      "user_constraint_array": [ {} ]
    }}
  }})",
      uc_body);
}

// clang-format on

/// Build under `low_memory=compress` + `release_ampl_after_flatten` (the
/// production SDDP/cascade path), solve, and `write_out()` — the rebuild
/// flatten that `write_out` triggers is where the resolution used to throw.
/// Returns the temp output dir so callers can inspect emitted files.
[[nodiscard]] auto build_solve_writeout(std::string_view uc_body,
                                        std::string_view tag)
    -> std::filesystem::path
{
  const auto tmpdir = std::filesystem::temp_directory_path() / std::string(tag);
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  LpMatrixOptions flat_opts;
  flat_opts.low_memory_mode = LowMemoryMode::compress;
  // The production solve path (gtopt_lp_runner) sets this so the per-cell
  // AMPL registry is freed after flatten — the precondition for the bug.
  flat_opts.release_ampl_after_flatten = true;

  Planning base;
  base.merge(parse_planning_json(make_json(uc_body)));
  base.options.output_directory = tmpdir.string();

  PlanningLP planning_lp(std::move(base), flat_opts);
  auto res = planning_lp.resolve();
  REQUIRE(res.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());

  // The defect surfaced here: rebuild_collections_if_needed() re-resolved
  // the user constraint against a registry that release_ampl_cell() had
  // freed, throwing "unknown attribute 'generation'".
  auto& sys = systems.front().front();
  CHECK_NOTHROW(sys.write_out());

  return tmpdir;
}

}  // namespace
}  // namespace uc_compress_rebuild_test

TEST_CASE(  // NOLINT
    "User constraint - generator.generation resolves under compress rebuild")
{
  using namespace uc_compress_rebuild_test;

  // The canonical failing case: a single generator output reference.
  const auto tmpdir = build_solve_writeout(
      R"({ "uid": 1, "name": "gen_cap",
           "expression": "generator('g1').generation <= 80" })",
      "gtopt_uc_compress_gen");

  // The UserConstraint dual output is emitted only if the constraint was
  // assembled into the LP during the rebuild (i.e. it resolved).
  const auto dual_file =
      tmpdir / "UserConstraint" / "constraint_dual_s0_p0.csv";
  CHECK(std::filesystem::exists(dual_file));

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "User constraint - sum() over generators resolves under compress rebuild")
{
  using namespace uc_compress_rebuild_test;

  // `sum(generator(all)...)` additionally exercises the element-metadata
  // registry (register_ampl_element_metadata), which release_ampl_cell()
  // also frees and the rebuild must replay.
  const auto tmpdir = build_solve_writeout(
      R"({ "uid": 1, "name": "sys_cap",
           "expression": "sum(generator(all).generation) <= 150" })",
      "gtopt_uc_compress_sum");

  const auto dual_file =
      tmpdir / "UserConstraint" / "constraint_dual_s0_p0.csv";
  CHECK(std::filesystem::exists(dual_file));

  std::filesystem::remove_all(tmpdir);
}
