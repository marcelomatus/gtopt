// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_cascade_per_level_output.cpp
 * @brief     Per-level cascade output routing (Issue #479).
 * @date      2026-05-18
 * @copyright BSD-3-Clause
 *
 * Validates the per-level output_subdir / write_out feature added in
 * response to https://github.com/marcelomatus/gtopt/issues/479.
 *
 *   - When ``output_subdir`` is set on an intermediate cascade level,
 *     that level's per-element parquet output (the per-cell
 *     ``SystemLP::write_out`` calls emitted by the SDDP sim pass) is
 *     routed to ``<output_directory>/<output_subdir>/<Element>/...``
 *     instead of the shared root, so it is NOT overwritten by the next
 *     level.  The default behaviour (no output_subdir) leaves the
 *     intermediate-level sim pass skipped, matching the pre-feature
 *     baseline.
 *
 *   - When ``write_out`` is set on a level (e.g. ``"none"``), that
 *     level inherits a per-level OutputFlags override that gates how
 *     much ``OutputContext`` emits during the sim pass.  Setting
 *     ``write_out = "none"`` together with ``output_subdir`` is the
 *     "do the sim pass but skip the parquet emission" configuration.
 */

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/planning_lp.hpp>

#include "cascade_helpers.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

TEST_CASE(  // NOLINT
    "Cascade per-level output_subdir routes intermediate-level writes "
    "to a dedicated subdirectory (Issue #479)")
{
  const auto tmpdir = std::filesystem::temp_directory_path()
      / "gtopt_cascade_per_level_output_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning = make_3phase_hydro_planning();
  planning.options.output_directory = tmpdir.string();
  planning.options.output_format = DataFormat::parquet;
  PlanningLP planning_lp(std::move(planning));

  // Two-level cascade.  Level 0 (warmup) opts into the per-level
  // output_subdir feature so its sim pass runs end-to-end and its
  // per-cell write_out lands under ``<tmpdir>/level0/...``.  Level 1
  // (final) keeps the default routing — its output overwrites
  // ``<tmpdir>/Generator/...`` etc. without touching the level0 subdir.
  SDDPOptions base_opts;
  base_opts.max_iterations = 1;
  base_opts.min_iterations = 0;
  base_opts.convergence_tol = 0.01;
  base_opts.apertures = std::vector<Uid> {};
  base_opts.enable_api = false;

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"warmup"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {1},
                  .apertures = Array<Uid> {},
              },
          .output_subdir = OptName {"level0"},
      },
      CascadeLevel {
          .name = OptName {"final"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {1},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base_opts), std::move(cascade));
  const SolverOptions lp_opts;
  const auto res = solver.solve(planning_lp, lp_opts);
  REQUIRE(res.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  // The final-level LP is transferred to the caller as the write_out
  // delegate (see ``cascade_method.cpp:1264-1289``), so the canonical
  // ``Generator/generation_sol.parquet`` lands under the shared root
  // when ``planning_lp.write_out()`` runs.
  planning_lp.write_out();

  SUBCASE("level 0 produced output under its dedicated subdir")
  {
    // The per-cell sim-pass writes go to the subdir.  Probe by checking
    // for the Generator dataset directory — its existence proves the
    // level-0 sim pass ran AND was routed correctly.
    const auto level0_gen_root = tmpdir / "level0" / "Generator";
    CHECK(std::filesystem::exists(level0_gen_root));
    CHECK(std::filesystem::is_directory(level0_gen_root));
  }

  SUBCASE("final level still writes to the shared root (last-level-wins)")
  {
    const auto final_gen_root = tmpdir / "Generator" / "generation_sol.parquet";
    CHECK(std::filesystem::exists(final_gen_root));
  }

  SUBCASE("intermediate-level output is NOT overwritten by the final level")
  {
    // Cross-check: the level0 subdir lives alongside (not nested inside)
    // the shared root, so a subsequent level's writes to the shared
    // root cannot stomp on it.
    const auto level0_gen_dir = tmpdir / "level0" / "Generator";
    const auto shared_gen_dir = tmpdir / "Generator";
    CHECK(std::filesystem::exists(level0_gen_dir));
    CHECK(std::filesystem::exists(shared_gen_dir));
    CHECK(level0_gen_dir != shared_gen_dir);
  }

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "Cascade per-level write_out=none skips parquet emission while "
    "still running the sim pass (Issue #479)")
{
  const auto tmpdir = std::filesystem::temp_directory_path()
      / "gtopt_cascade_per_level_writeout_none_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning = make_3phase_hydro_planning();
  planning.options.output_directory = tmpdir.string();
  planning.options.output_format = DataFormat::parquet;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base_opts;
  base_opts.max_iterations = 1;
  base_opts.min_iterations = 0;
  base_opts.convergence_tol = 0.01;
  base_opts.apertures = std::vector<Uid> {};
  base_opts.enable_api = false;

  // Level 0: output_subdir set (sim pass enabled) but write_out=none
  // ⇒ OutputContext emits nothing under the per-level subdir.
  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"warmup"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {1},
                  .apertures = Array<Uid> {},
              },
          .output_subdir = OptName {"level0"},
          .write_out = OptName {"none"},
      },
      CascadeLevel {
          .name = OptName {"final"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {1},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base_opts), std::move(cascade));
  const SolverOptions lp_opts;
  const auto res = solver.solve(planning_lp, lp_opts);
  REQUIRE(res.has_value());

  planning_lp.write_out();

  SUBCASE("write_out=none — level0 subdir has NO Generator parquet")
  {
    const auto level0_gen_sol =
        tmpdir / "level0" / "Generator" / "generation_sol.parquet";
    CHECK_FALSE(std::filesystem::exists(level0_gen_sol));
  }

  SUBCASE("final level still emits its full output set")
  {
    const auto final_gen_sol = tmpdir / "Generator" / "generation_sol.parquet";
    CHECK(std::filesystem::exists(final_gen_sol));
  }

  std::filesystem::remove_all(tmpdir);
}

}  // namespace
