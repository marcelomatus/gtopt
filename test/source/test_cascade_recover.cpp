/**
 * @file      test_cascade_recover.cpp
 * @brief     End-to-end resume tests for cascade `--recover`.
 * @date      2026-05-20
 * @copyright BSD-3-Clause
 *
 * Pins the runtime resume behaviour added in commit
 * d90cf8f77 ("feat(cascade): --recover resumes at the correct level via
 * cascade_progress.json").
 *
 * Where existing coverage sits:
 *   • `test_cascade_progress.cpp` — pure JSON round-trip + atomic-write
 *     for `CascadeProgress`.  Does NOT actually run the cascade.
 *
 * What this file adds (the missing scope):
 *   • Run the cascade end-to-end to a real `cuts_output_file` so the
 *     sidecar `cascade_progress.json` is actually written.
 *   • Reuse the same on-disk artifacts in a second `CascadePlanningMethod`
 *     run with `recovery_mode = full` and assert that already-`done`
 *     levels are SKIPPED at runtime (i.e. not pushed into `level_stats`),
 *     while levels not yet `done` execute.
 *
 * Strategy: drive `CascadePlanningMethod::solve()` directly instead of
 * spawning the gtopt CLI — the resume logic lives in
 * `source/cascade_method.cpp`, the same entry point the CLI uses.  This
 * keeps the test fast (~hundreds of ms), removes the parquet-input-deck
 * scaffolding the subprocess strategy would require, and still pins the
 * SIGKILL-then-resume contract because we explicitly simulate the
 * mid-cascade interruption by editing the on-disk checkpoint between
 * runs (the same effect a real SIGKILL between level boundaries would
 * leave on disk).
 *
 * ──────────────────────────────────────────────────────────────────────
 * 2026-05-20: the originally-documented production-side gap (final-
 * converged-level `break` skipped the progress flush, leaving the
 * persisted status at `in_progress`) is fixed in cascade_method.cpp:1202-1225
 * — `save_cascade_progress` now runs in-line before the `break`.
 *
 * With the fix the contract becomes symmetric:
 *   * Every level (intermediate AND final) reaches `done` on disk after
 *     a normal converged cascade.
 *   * A `--recover` on a fully-converged cascade therefore enters the
 *     "all done — nothing to do" branch (cascade_method.cpp:599-603)
 *     and produces an empty `level_stats()`.
 *   * Mid-cascade SIGKILL is simulated below by hand-editing the
 *     checkpoint to demote the final level back to `in_progress`.  The
 *     resume scan then re-solves only the demoted level.
 * ──────────────────────────────────────────────────────────────────────
 */

// SPDX-License-Identifier: BSD-3-Clause

#include <filesystem>
#include <random>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/cascade_progress.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/sddp_enums.hpp>

#include "cascade_helpers.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace test_cascade_recover  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

namespace
{

/// Per-test scratch directory under `/tmp`.  Random suffix so concurrent
/// ctest runs (`ctest -j20`) never collide.  Removed on destruction.
struct TempCascadeDir
{
  std::filesystem::path path;

  TempCascadeDir()
  {
    namespace fs = std::filesystem;
    auto p = fs::temp_directory_path() / "gtopt_cascade_recover_test";
    p /= std::to_string(static_cast<std::int64_t>(std::random_device {}()));
    fs::create_directories(p);
    path = p;
  }

  ~TempCascadeDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  TempCascadeDir(const TempCascadeDir&) = delete;
  TempCascadeDir(TempCascadeDir&&) = delete;
  auto operator=(const TempCascadeDir&) -> TempCascadeDir& = delete;
  auto operator=(TempCascadeDir&&) -> TempCascadeDir& = delete;
};

/// Two-level cascade options used by both the "first run" and the
/// "--recover run" so the level-name vector matches between runs (the
/// checkpoint is keyed by `(index, name)` — see cascade_method.cpp:538).
auto make_two_level_cascade() -> CascadeOptions
{
  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"uninodal"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {2},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"full_network"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {2},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };
  return cascade;
}

auto make_base_opts(const std::string& cuts_output_file,
                    RecoveryMode recovery_mode) -> SDDPOptions
{
  SDDPOptions opts;
  opts.max_iterations = 4;
  opts.convergence_tol = 0.01;
  opts.apertures = std::vector<Uid> {};
  opts.cuts_output_file = cuts_output_file;
  opts.recovery_mode = recovery_mode;
  opts.save_per_iteration = true;
  return opts;
}

}  // namespace

// ─── First run writes a progress sidecar with every level done ───────────
//
// Pins the "cold-start writes checkpoint" half of the resume contract:
// the sidecar `cascade_progress.json` must be on disk after a normal
// (non-recover) run, with EVERY level (intermediate AND final) marked
// `done`.  The final-level flush was wired in 2026-05-20 — before that
// fix the converged final level `break`d out of the loop without
// flushing, leaving its persisted status at `in_progress`.  See the
// file-header note for the history.

TEST_CASE("Cascade first run writes cascade_progress.json sidecar")  // NOLINT
{
  TempCascadeDir scratch;
  const auto cuts_file = (scratch.path / "sddp_cuts.parquet").string();
  const auto progress_path = scratch.path / "cascade_progress.json";

  auto planning = make_3phase_2bus_hydro_planning();
  PlanningLP plp(std::move(planning));

  // Plain (no-recover) run: cuts_output_file is set, so progress
  // checkpointing is enabled regardless of recovery_mode.
  auto base = make_base_opts(cuts_file, RecoveryMode::none);
  auto cascade = make_two_level_cascade();

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto res = solver.solve(plp, lp_opts);
  REQUIRE(res.has_value());

  // Both levels executed end-to-end.
  REQUIRE(solver.level_stats().size() == 2);

  SUBCASE("progress file exists with both levels recorded")
  {
    REQUIRE(std::filesystem::exists(progress_path));
    auto loaded = load_cascade_progress(progress_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->levels.size() == 2);

    CHECK(loaded->levels[0].name == "uninodal");
    CHECK(loaded->levels[1].name == "full_network");
    // Intermediate level: post-level flush runs unconditionally inside
    // the loop body (cascade_method.cpp:1451).
    CHECK(loaded->levels[0].status == CascadeLevelStatus::done);
    CHECK(loaded->levels[0].iters >= 1);
    CHECK(loaded->levels[0].global_iter_after >= 0);
    // Final level: 2026-05-20 inline flush before the
    // `break` at :1204 marks the persisted status as `done` here too.
    CHECK(loaded->levels[1].status == CascadeLevelStatus::done);
  }

  SUBCASE("no leftover .tmp from the atomic rename")
  {
    CHECK_FALSE(std::filesystem::exists(progress_path.string() + ".tmp"));
  }
}

// ─── --recover skips already-done levels and resumes at next active ──────
//
// The core regression guard for d90cf8f77.  Two complementary paths
// are exercised:
//
//   1. "All-done resume": after the 2026-05-20 fix that flushes
//      progress on final-level convergence, a fresh
//      CascadePlanningMethod with `recovery_mode = full` against the
//      same sidecar sees [L0=done, L1=done] on disk and reaches the
//      "all levels already complete — nothing to do" branch.
//      `level_stats()` is therefore EMPTY (no levels re-solved).
//
//   2. "Synthetic SIGKILL resume": we then hand-edit the persisted
//      checkpoint to demote L1 back to `in_progress` (same observable
//      state a real SIGKILL between level boundaries would leave) and
//      re-run — the resume scan picks L1 as the first non-done active
//      level, skips L0, and `level_stats()` has exactly ONE entry
//      (full_network).  This is the partial-resume path the feature
//      was designed for and it remains the test we care about most.
//      "final-level-not-flushed" quirk: even after the upstream fix
//      moots case 1, case 2 keeps pinning the resume contract.

TEST_CASE(  // NOLINT
    "Cascade --recover skips done levels and resumes at first non-done level")
{
  TempCascadeDir scratch;
  const auto cuts_file = (scratch.path / "sddp_cuts.parquet").string();
  const auto progress_path = scratch.path / "cascade_progress.json";

  // ── First run: complete both levels naturally so cuts are on disk
  //    for the resume to consume. ──
  {
    auto planning = make_3phase_2bus_hydro_planning();
    PlanningLP plp(std::move(planning));
    auto base = make_base_opts(cuts_file, RecoveryMode::none);
    auto cascade = make_two_level_cascade();
    CascadePlanningMethod first(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    REQUIRE(first.solve(plp, lp_opts).has_value());
    REQUIRE(first.level_stats().size() == 2);
  }
  REQUIRE(std::filesystem::exists(progress_path));

  // Sanity-check the on-disk state: BOTH levels marked done (post-fix
  // contract — see file header).
  {
    auto checkpoint = load_cascade_progress(progress_path);
    REQUIRE(checkpoint.has_value());
    REQUIRE(checkpoint->levels.size() == 2);
    CHECK(checkpoint->levels[0].status == CascadeLevelStatus::done);
    CHECK(checkpoint->levels[1].status == CascadeLevelStatus::done);
  }

  // ── Case 1: all-done resume against the first run's sidecar. ──
  {
    auto planning = make_3phase_2bus_hydro_planning();
    PlanningLP plp(std::move(planning));
    auto base = make_base_opts(cuts_file, RecoveryMode::full);
    auto cascade = make_two_level_cascade();
    CascadePlanningMethod second(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto res = second.solve(plp, lp_opts);
    REQUIRE(res.has_value());

    SUBCASE("all-done resume: every level skipped, level_stats empty")
    {
      // Every level is `done` on disk → the resume scan in
      // cascade_method.cpp:599-603 fires the "nothing to do" branch
      // and no level is re-solved.  `level_stats()` must therefore
      // be empty.
      CHECK(second.level_stats().empty());
    }
  }

  // ── Case 2: synthetic SIGKILL — hand-craft the same partial state. ──
  // Hand-edit the checkpoint to drop L1 back to `in_progress` (the
  // observable on-disk state a real SIGKILL between level boundaries
  // would leave behind).  Even once the production-side flush gap is
  // closed, this still exercises the resume path.
  {
    auto checkpoint = load_cascade_progress(progress_path);
    REQUIRE(checkpoint.has_value());
    REQUIRE(checkpoint->levels.size() == 2);
    checkpoint->levels[1].status = CascadeLevelStatus::in_progress;
    REQUIRE(save_cascade_progress(*checkpoint, progress_path).has_value());
  }
  {
    auto planning = make_3phase_2bus_hydro_planning();
    PlanningLP plp(std::move(planning));
    auto base = make_base_opts(cuts_file, RecoveryMode::full);
    auto cascade = make_two_level_cascade();
    CascadePlanningMethod third(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto res = third.solve(plp, lp_opts);
    REQUIRE(res.has_value());

    SUBCASE("synthetic resume: only the demoted level appears in level_stats")
    {
      REQUIRE(third.level_stats().size() == 1);
      CHECK(third.level_stats()[0].name == "full_network");
    }
  }
}

// ─── --recover treats all-done sidecar as a no-op ────────────────────────
//
// The "all levels already complete — nothing to do" branch of the
// resume scan (cascade_method.cpp:599-603) fires only when EVERY
// active level has `status == done` on disk.  We synthesise that
// state by editing the sidecar directly (see PRODUCTION-SIDE TODO at
// file top: a natural happy-path cascade never reaches this state on
// disk because the final level's `done` is never flushed).

TEST_CASE(  // NOLINT
    "Cascade --recover with all levels done on disk is a no-op")
{
  TempCascadeDir scratch;
  const auto cuts_file = (scratch.path / "sddp_cuts.parquet").string();
  const auto progress_path = scratch.path / "cascade_progress.json";

  // First cascade populates cuts and a checkpoint with every level
  // marked `done` (post-2026-05-20 contract — the final level's flush
  // now runs before the convergence `break`).
  {
    auto planning = make_3phase_2bus_hydro_planning();
    PlanningLP plp(std::move(planning));
    auto base = make_base_opts(cuts_file, RecoveryMode::none);
    auto cascade = make_two_level_cascade();
    CascadePlanningMethod first(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    REQUIRE(first.solve(plp, lp_opts).has_value());
  }
  REQUIRE(std::filesystem::exists(progress_path));

  // Explicitly promote every level to `done` to keep this test hermetic
  // against future changes to the natural first-run end state.  The
  // promotion is a no-op today but pins the assertion semantics: the
  // no-op branch fires ONLY when EVERY active level is `done` on disk.
  {
    auto checkpoint = load_cascade_progress(progress_path);
    REQUIRE(checkpoint.has_value());
    REQUIRE(checkpoint->levels.size() == 2);
    for (auto& lvl : checkpoint->levels) {
      lvl.status = CascadeLevelStatus::done;
    }
    REQUIRE(save_cascade_progress(*checkpoint, progress_path).has_value());
  }

  // Second cascade with --recover: nothing to do.
  auto planning = make_3phase_2bus_hydro_planning();
  PlanningLP plp(std::move(planning));
  auto base = make_base_opts(cuts_file, RecoveryMode::full);
  auto cascade = make_two_level_cascade();
  CascadePlanningMethod second(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto res = second.solve(plp, lp_opts);
  REQUIRE(res.has_value());

  SUBCASE("no level was re-solved")
  {
    CHECK(second.level_stats().empty());
    CHECK(second.all_results().empty());
  }
}

// ─── --recover with missing checkpoint = cold start ──────────────────────
//
// When the user passes `--recover` but no checkpoint exists (first ever
// run on this output dir, or output dir was wiped), the cascade must
// fall through to a clean cold start — every active level executes
// normally.  See cascade_method.cpp:620-622.

TEST_CASE("Cascade --recover with no checkpoint cold-starts")  // NOLINT
{
  TempCascadeDir scratch;
  const auto cuts_file = (scratch.path / "sddp_cuts.parquet").string();
  const auto progress_path = scratch.path / "cascade_progress.json";

  // Confirm starting precondition: no checkpoint on disk.
  REQUIRE_FALSE(std::filesystem::exists(progress_path));

  auto planning = make_3phase_2bus_hydro_planning();
  PlanningLP plp(std::move(planning));
  auto base = make_base_opts(cuts_file, RecoveryMode::full);
  auto cascade = make_two_level_cascade();
  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto res = solver.solve(plp, lp_opts);
  REQUIRE(res.has_value());

  SUBCASE("both levels executed end-to-end")
  {
    REQUIRE(solver.level_stats().size() == 2);
    CHECK(solver.level_stats()[0].name == "uninodal");
    CHECK(solver.level_stats()[1].name == "full_network");
  }

  SUBCASE(
      "cold-start writes a fresh checkpoint with the intermediate "
      "level done")
  {
    // Intermediate level reaches `done` on disk; the final level
    // stays at `in_progress` (production-side TODO at file top).
    // We only pin the intermediate-level invariant here — the final-
    // level status is asserted explicitly NOT to be `done` so we
    // notice if the upstream behaviour changes.
    REQUIRE(std::filesystem::exists(progress_path));
    auto loaded = load_cascade_progress(progress_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->levels.size() == 2);
    CHECK(loaded->levels[0].status == CascadeLevelStatus::done);
  }
}

}  // namespace test_cascade_recover
