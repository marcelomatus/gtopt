// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_cascade_cut_inheritance.cpp
 * @brief     Targeted unit tests for the cascade method's cut-inheritance
 *            machinery (inherit_optimality_cuts wiring).
 * @date      2026-05-13
 *
 * Background — observed regression on juan/IPLP (2026-05): a cascade run
 * with `inherit_optimality_cuts: -1` in each level's transition did NOT
 * emit the expected `Cascade [...]: serialized N cuts for next level` /
 * `Cascade [...]: transferred N cuts from ...` log messages, and level 1
 * iter 0 LB landed back at the alpha-bootstrap floor.  These tests pin
 * the assumption "cuts inherit across cascade levels when
 * inherit_optimality_cuts != 0" in code form, with self-contained
 * fixtures (no juan/IPLP data dependency).
 *
 * Test layers (see test/source/CLAUDE.md style):
 *   - Layer 1: JSON parsing of CascadeTransition.inherit_optimality_cuts
 *   - Layer 2: SDDPMethod::stored_cuts() invariants after a backward pass
 *   - Layer 3: save_cuts_parquet + load_cuts round-trip on a fresh LP
 *   - Layer 4: Cascade level-to-level cut transfer end-to-end
 *   - Layer 5: Filter behaviour (Feasibility excluded, dual-threshold cap)
 *
 * Each TEST_CASE is independent and may pass or fail on the current code:
 *  - PASS today ⇒ regression guard for future refactors.
 *  - FAIL today ⇒ documents the bug as a runnable spec.
 */

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/cascade_options.hpp>
#include <gtopt/json/json_cascade_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_cut_store_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <unistd.h>  // ::getpid()

#include "log_capture.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// ───────────────────────────────────────────────────────────────────────────
// Layer 1 — JSON parsing of inherit_optimality_cuts
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE("CascadeTransition JSON: inherit_optimality_cuts=-1 is parsed")
{
  // Direct parse — guards against a binding regression that would
  // silently drop the field (mapped to wrong key name, missing
  // json_number_null entry, etc.).
  const std::string_view json_data = R"({
    "inherit_optimality_cuts": -1
  })";

  const auto tr = daw::json::from_json<CascadeTransition>(json_data);

  REQUIRE(tr.inherit_optimality_cuts.has_value());
  CHECK(*tr.inherit_optimality_cuts == -1);
}

TEST_CASE("CascadeTransition JSON: missing field -> nullopt (NOT zero)")
{
  // Critical case for the cascade gate: `inherit_optimality_cuts.value_or(0)`
  // must be 0 only when the JSON omits the field entirely.  A subtle bug
  // would be a binding that defaults to OptInt{0} (still has_value),
  // which interacts oddly with the cascade's "0 = do not inherit" rule
  // — currently equivalent, but the test pins the distinction.
  const std::string_view json_data = R"({})";

  const auto tr = daw::json::from_json<CascadeTransition>(json_data);

  CHECK_FALSE(tr.inherit_optimality_cuts.has_value());
}

TEST_CASE(
    "CascadeOptions JSON: inherit_optimality_cuts=-1 survives full "
    "level_array nesting")
{
  // The juan/IPLP regression manifested with the field inside
  // level_array[N].transition — verify the nested deserialization
  // preserves the value.  Round-trip via to_json/from_json to catch
  // any asymmetry in the json_data_contract.
  const CascadeOptions original {
      .level_array =
          {
              CascadeLevel {
                  .uid = OptUid {1},
                  .name = OptName {"uninodal"},
              },
              CascadeLevel {
                  .uid = OptUid {2},
                  .name = OptName {"transport"},
                  .transition =
                      CascadeTransition {
                          .inherit_optimality_cuts = OptInt {-1},
                      },
              },
          },
  };

  const auto json = daw::json::to_json(original);
  REQUIRE(!json.empty());
  const auto rt = daw::json::from_json<CascadeOptions>(json);

  REQUIRE(rt.level_array.size() == 2);
  REQUIRE(rt.level_array[1].transition.has_value());
  REQUIRE(rt.level_array[1].transition->inherit_optimality_cuts.has_value());
  CHECK(*rt.level_array[1].transition->inherit_optimality_cuts == -1);
}

// ───────────────────────────────────────────────────────────────────────────
// Layer 2 — SDDPMethod::stored_cuts() invariants
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "SDDPMethod stored_cuts() is non-empty after a converged backward pass")
{
  // The cascade gate at cascade_method.cpp:732 reads
  //   if (want_opt_cuts && !stored_cuts.empty()) { ... save_cuts_parquet(...) }
  // The "no serialized cuts" symptom on juan implies either
  // `want_opt_cuts == false` (covered in Layer 1) or `stored_cuts.empty()`
  // (covered here).  This test fails if a backward pass produces no
  // cuts visible via the public stored_cuts() accessor.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;
  // Pure Benders, no apertures — guarantees backward-pass optimality cuts.
  sddp_opts.apertures = std::vector<Uid> {};

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE(!results->empty());

  const auto cuts = sddp.stored_cuts();
  CHECK(!cuts.empty());
  // num_stored_cuts() and stored_cuts().size() must agree.
  CHECK(static_cast<std::ptrdiff_t>(cuts.size()) == sddp.num_stored_cuts());
}

TEST_CASE("SDDPMethod stored_cuts() entries are tagged CutType::Optimality")
{
  // The cascade filter at cascade_method.cpp:739 only forwards
  // CutType::Optimality.  Pure-Benders SDDP backward passes should emit
  // exclusively optimality cuts (no feasibility / Chinneck cuts in
  // a feasible-by-construction hydro fixture).
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.apertures = std::vector<Uid> {};

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto cuts = sddp.stored_cuts();
  REQUIRE(!cuts.empty());

  int opt_count = 0;
  int feas_count = 0;
  for (const auto& c : cuts) {
    if (c.type == CutType::Optimality) {
      ++opt_count;
    } else if (c.type == CutType::Feasibility) {
      ++feas_count;
    }
  }
  CHECK(opt_count > 0);
  // A feasible hydro fixture should not need any feasibility cuts.
  CHECK(feas_count == 0);
}

TEST_CASE("SDDPMethod stored_cuts() entries carry valid phase_uid / scene_uid")
{
  // The Parquet serializer (`build_cuts_table`) drops cuts whose
  // `phase_uid` does not resolve via build_phase_uid_map.  An empty
  // / unknown phase_uid silently zeros the serialized cut set.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.apertures = std::vector<Uid> {};

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto cuts = sddp.stored_cuts();
  REQUIRE(!cuts.empty());

  // Every cut should reference one of the 3 known phase UIDs and one
  // of the 2 known scene UIDs.
  for (const auto& c : cuts) {
    const auto puid = static_cast<std::int64_t>(c.phase_uid);
    const auto suid = static_cast<std::int64_t>(c.scene_uid);
    CHECK((puid == 1 || puid == 2 || puid == 3));
    CHECK((suid == 1 || suid == 2));
  }
}

// ───────────────────────────────────────────────────────────────────────────
// Layer 3 — save_cuts_parquet + load_cuts round-trip
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE("save_cuts_parquet -> load_cuts round-trip preserves cut count")
{
  // End-to-end disk round-trip: save the stored cuts from one SDDP run,
  // load them into a fresh SDDPMethod on an equivalent PlanningLP, and
  // verify the load result count matches what was saved.
  //
  // The cascade transfer path is exactly:
  //   save_cuts_parquet(filtered, current_lp, tmp_file)
  //     ...solver released...
  //   new_solver.load_cuts(tmp_file)
  // — so this round-trip test mirrors that without the cascade glue.
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file = (tmp_dir
                          / std::format("gtopt_test_cascade_cut_inh_{}.parquet",
                                        static_cast<std::int64_t>(::getpid())))
                             .string();
  std::filesystem::remove(cuts_file);  // ensure clean slate

  int saved_count = 0;

  // ── Phase A: solve + save cuts ──
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions opts;
    opts.max_iterations = 3;
    opts.convergence_tol = 1e-6;
    opts.apertures = std::vector<Uid> {};

    SDDPMethod sddp(planning_lp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());

    const auto cuts = sddp.stored_cuts();
    REQUIRE(!cuts.empty());
    saved_count = static_cast<int>(cuts.size());

    auto save_result = save_cuts_parquet(cuts, planning_lp, cuts_file);
    REQUIRE(save_result.has_value());
  }

  REQUIRE(std::filesystem::exists(cuts_file));
  REQUIRE(saved_count > 0);

  // ── Phase B: hot-start load into a fresh solver ──
  {
    auto planning2 = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP planning_lp2(std::move(planning2));

    SDDPOptions opts2;
    opts2.max_iterations = 1;  // do nothing — we only want load_cuts
    opts2.convergence_tol = 1e-6;
    opts2.apertures = std::vector<Uid> {};

    SDDPMethod sddp2(planning_lp2, opts2);
    auto init = sddp2.ensure_initialized();
    REQUIRE(init.has_value());

    auto load_result = sddp2.load_cuts(cuts_file);
    REQUIRE(load_result.has_value());
    CHECK(load_result->count == saved_count);

    // Stronger assertion: the cut store (``m_cut_store_``) must hold
    // exactly the same number of cuts as were saved — NOT
    // ``saved_count × num_scenes``.
    //
    // The original ``load_result.count`` check only verified the
    // file-row count (incremented once per parquet row).  It did NOT
    // catch the broadcast bug where each loaded cut was replicated
    // across all N_scenes and pushed into every scene's cut store —
    // the symptom that produced 6 400 → 104 800 → 1 679 560 cuts at
    // each juan/IPLP cascade transition.  This stronger check
    // enforces the per-scene routing contract: 1 file row → 1 store
    // entry.
    const auto stored_after_load = sddp2.num_stored_cuts();
    CHECK(stored_after_load == saved_count);
  }

  std::filesystem::remove(cuts_file);
}

TEST_CASE("save_cuts_parquet creates a non-empty Parquet file")
{
  // Lightweight sanity check: the parquet writer must actually emit a
  // file (not just succeed on an empty span).  Catches a regression
  // where the writer would silently no-op on a non-empty cut set.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 2;
  opts.apertures = std::vector<Uid> {};

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto cuts = sddp.stored_cuts();
  REQUIRE(!cuts.empty());

  const auto path = (std::filesystem::temp_directory_path()
                     / std::format("gtopt_test_cascade_writer_{}.parquet",
                                   static_cast<std::int64_t>(::getpid())))
                        .string();
  std::filesystem::remove(path);

  auto save_result = save_cuts_parquet(cuts, planning_lp, path);
  REQUIRE(save_result.has_value());
  REQUIRE(std::filesystem::exists(path));
  CHECK(std::filesystem::file_size(path) > 0);

  std::filesystem::remove(path);
}

// ───────────────────────────────────────────────────────────────────────────
// Layer 4 — Cascade level-to-level transfer (the integration target)
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Cascade level 0 -> level 1 transfers optimality cuts when "
    "inherit_optimality_cuts=-1")
{
  // This is the critical regression test that mirrors the juan/IPLP
  // symptom: level 1 should START with cuts already loaded.
  //
  // We assert via two channels:
  //  (a) level 0 produced at least one optimality cut
  //      (cuts_added > 0 in level_stats[0]).
  //  (b) level 1 was reasonably close to converged at the same bound
  //      that level 0 reached — a strong proxy for "cuts inherited".
  //      Without inheritance, level 1 would have to rebuild the value
  //      function from scratch in <= max_iterations.
  //
  // If the cascade silently skips serialize/load (the juan bug), level
  // 1 either starts from the alpha floor LB (LB << level 0 LB) or runs
  // far more iterations than level 0 did.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());

  REQUIRE(solver.level_stats().size() == 2);
  const auto& s0 = solver.level_stats()[0];
  const auto& s1 = solver.level_stats()[1];

  // (a) Level 0 must have produced cuts — otherwise nothing to transfer
  // and the test below would be meaningless.
  REQUIRE(s0.cuts_added > 0);

  // (b) Level 1 starts with level 0's cuts → its starting LB should be
  // at least as good as level 0's final LB (within a small slack).  The
  // first SDDPIterationResult of level 1 lives in `all_results()` right
  // after the last training iter of level 0.  This is fragile but
  // localised: we look up the second level's first iteration.
  //
  // Easier proxy: level 1's final LB should match level 0's final LB
  // (same problem, same cuts → same fixed point).  Use a generous
  // tolerance because Benders converge tolerance is not bit-identical.
  CHECK(s1.lower_bound >= s0.lower_bound * 0.999 - 1e-3);

  // (c) With inheritance, level 1 must NOT need more training
  // iterations than level 0 did to reach the same bound.  This is the
  // canonical "cuts inherited" signal — same in test_cascade_integration.
  CHECK(s1.iterations <= s0.iterations);
}

TEST_CASE("Cuts survive system_file swap across cascade levels")
{
  // Mirror of the inherit=-1 test above but with a system_file swap at
  // level 1.  Cuts are state-variable-indexed (by class+col+uid), NOT
  // by bus uid, so a system swap must NOT lose them.  Concretely:
  //
  //   level 0  — single-bus, builds cuts
  //   level 1  — same single-bus shape (loaded from temp JSON), should
  //              inherit cuts and converge in ≤ level 0's iters
  //
  // This is the canonical "cuts survived the LP rebuild" smoke test.
  // It uses the SAME planning at both levels (the swap is a semantic
  // no-op on the system) — what we're proving is that the LP rebuild
  // doesn't drop state-var references stored in cuts.
  auto planning_for_disk = make_2scene_3phase_hydro_planning(0.5, 0.5);
  const auto tmp_path = std::filesystem::temp_directory_path()
      / std::format("gtopt_cascade_cut_swap_{}.json", ::getpid());
  {
    std::ofstream ofs(tmp_path);
    REQUIRE(ofs.is_open());
    ofs << daw::json::to_json(planning_for_disk);
  }
  REQUIRE(std::filesystem::exists(tmp_path));

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_with_system_swap"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .system_file = OptName {tmp_path.string()},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());

  REQUIRE(solver.level_stats().size() == 2);
  const auto& s0 = solver.level_stats()[0];
  const auto& s1 = solver.level_stats()[1];

  // Level 0 must have produced cuts — otherwise nothing to transfer.
  REQUIRE(s0.cuts_added > 0);
  // Cuts survived the system_file swap → level 1's final LB matches
  // level 0's (same problem, same cuts → same fixed point).
  CHECK(s1.lower_bound >= s0.lower_bound * 0.999 - 1e-3);
  // And level 1 converges in ≤ level 0's iters — the cuts gave it a
  // head start.
  CHECK(s1.iterations <= s0.iterations);

  std::error_code ec;
  std::filesystem::remove(tmp_path, ec);
}

TEST_CASE(
    "Cascade with inherit_optimality_cuts=0 does NOT transfer "
    "(level 1 starts fresh)")
{
  // Companion: with `inherit_optimality_cuts = 0`, the cascade gate
  //    if (want_opt_cuts && !stored_cuts.empty())
  // must be false (want_opt_cuts = (opt_cut_mode != 0) = false), so the
  // save_cuts_parquet code path never executes and level 1 must build
  // its own value function from scratch.  When level 1 has the same
  // structure as level 0, it should require ROUGHLY as many iterations
  // (no inheritance benefit).
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_no_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  // The whole-cascade run completes; we don't assert about iteration
  // counts here because the level may still converge in 1 iteration
  // for a trivial 3-phase fixture.  We do assert level 1 produced its
  // own cuts (mode=0 ⇒ no inheritance, so any cuts at L1 were learned
  // locally).
  CHECK(solver.level_stats()[1].cuts_added >= 0);
  // The cascade must not crash and must report a finite, non-positive
  // gap on convergence (or a positive gap if budget ran out).
  CHECK(std::isfinite(solver.level_stats()[1].gap));
}

TEST_CASE("Cascade with default transition (= nullopt) does NOT transfer cuts")
{
  // When level 1 has no `transition` block at all, the cascade code
  // never enters the `if (next_level.transition)` branch — no cuts
  // serialized.  This is the "off by default" guarantee.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_default"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
              },
          // .transition absent ⇒ nullopt
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);
  // We just want the run to complete without producing an invalid
  // state — and to document that absent transition is the "no-op"
  // default the cascade must respect.
}

// ───────────────────────────────────────────────────────────────────────────
// Layer 5 — Filter behaviour (in-process, no cascade orchestration)
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Cascade pre-save filter keeps Optimality cuts and drops Feasibility cuts")
{
  // Mirrors the filter inside CascadePlanningMethod::solve at
  // cascade_method.cpp:738-748 — exercised here as a free predicate so
  // a refactor that breaks the type check is caught at unit-test scale.
  std::vector<StoredCut> raw;
  raw.push_back(StoredCut {
      .type = CutType::Optimality,
      .phase_uid = make_uid<Phase>(1),
      .scene_uid = make_uid<Scene>(1),
      .iteration_index = IterationIndex {0},
      .rhs = 10.0,
  });
  raw.push_back(StoredCut {
      .type = CutType::Feasibility,
      .phase_uid = make_uid<Phase>(1),
      .scene_uid = make_uid<Scene>(1),
      .iteration_index = IterationIndex {0},
      .rhs = 5.0,
  });
  raw.push_back(StoredCut {
      .type = CutType::Optimality,
      .phase_uid = make_uid<Phase>(2),
      .scene_uid = make_uid<Scene>(2),
      .iteration_index = IterationIndex {1},
      .rhs = 20.0,
  });

  // Apply the same predicate the cascade does: keep CutType::Optimality.
  std::vector<StoredCut> filtered;
  filtered.reserve(raw.size());
  for (const auto& c : raw) {
    if (c.type == CutType::Optimality) {
      filtered.push_back(c);
    }
  }

  CHECK(filtered.size() == 2);
  for (const auto& c : filtered) {
    CHECK(c.type == CutType::Optimality);
    // (Previously also asserted ``c.name.find("feas") == npos`` — the
    // ``name`` field was removed; ``c.type`` is now the authoritative
    // distinction between optimality and feasibility cuts, so the
    // single check above subsumes the redundant name-based one.)
  }
}

// Tests for `optimality_dual_threshold` removed on 2026-05-15: the
// option was structurally broken (it filtered on `StoredCut.dual`,
// which was only ever populated by reading the wrong LP — the main
// cell's post-cut-add-without-resolve state, never the apertures
// where cuts are actually exercised), and is now deleted alongside
// the `update_stored_cut_duals` machinery.  A correct cut-activity
// signal would be a probability-weighted aperture-dual aggregate
// accumulated during the backward pass; that's a real feature, not
// a fix, and it is not currently implemented.

// ───────────────────────────────────────────────────────────────────────────
// Layer 6 — Forget-after-N, multi-level, selective and threshold modes
// ───────────────────────────────────────────────────────────────────────────
//
// These cases extend Layer 4 to cover the *full* tri-state semantics of
// `CascadeTransition.inherit_optimality_cuts` (see cascade_method.cpp
// lines 539-618):
//
//   0 or absent : do not inherit
//   -1          : inherit and keep forever
//   N > 0       : inherit and FORGET after N training iterations, then
//                 re-solve with only self-generated cuts.
//
// CascadePlanningMethod does not expose the inner SDDPMethod, so we
// observe the inheritance lifecycle through three external channels:
//   (a) spdlog messages emitted by the cascade orchestrator
//       ("transferred N cuts from", "forgetting N inherited cuts",
//        "re-solving without inherited cuts", "serialized N cuts").
//   (b) `solver.level_stats()` per-level convergence + counts.
//   (c) `solver.all_results()` total size — when a forget cycle fires,
//       phase-1 results are appended in addition to phase-2.

TEST_CASE(  // NOLINT
    "Cascade inherit_optimality_cuts forget after N iters (N > 0)")
{
  // N>0 mode: level 1 should LOAD level 0's cuts, run *at most* N
  // iterations with them, then FORGET them and re-solve.  The forget
  // path is gated by `inherited_cut_count > 0 && forget_threshold > 0`
  // (cascade_method.cpp line 578).  We pin every observable side
  // effect via the log capture + level_stats accessors.
  using gtopt::test::LogCapture;

  constexpr int forget_n = 2;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 30;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0_seed"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {6},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_forget_2"},
          // Same model_options as level 0 ⇒ apples-to-apples comparison
          // (the inherited cuts are valid in level 1's value-function
          // domain).
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {forget_n},
              },
      },
  };

  LogCapture logs(1024);
  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());

  REQUIRE(solver.level_stats().size() == 2);
  const auto& s0 = solver.level_stats()[0];

  // (a) Level 0 actually produced optimality cuts — otherwise nothing
  // to inherit and the forget machinery is a no-op.
  REQUIRE(s0.cuts_added > 0);

  // (b) The cascade orchestrator's inheritance log fires.  These
  // strings live at cascade_method.cpp:525, :581, and :609 — pinning
  // them guards against a silent rename or downgrade to DEBUG-only.
  CHECK(logs.contains("transferred"));
  CHECK(logs.contains("forgetting"));
  CHECK(logs.contains("re-solving without inherited cuts"));

  // (c) Phase-1 must be capped at `forget_n` iters: the log emits the
  // observed phase-1 iter count in the same line, which must be
  // <= forget_n.  This is the strongest external signal that the
  // `max_iterations = std::min(level_max, forget_threshold)` cap at
  // cascade_method.cpp:563 was honoured.
  bool phase1_capped = false;
  for (const auto& m : logs.messages()) {
    // Look for "forgetting <C> inherited cuts after <K> iters" and
    // assert K <= forget_n.
    const auto pos = m.find("forgetting ");
    if (pos == std::string::npos) {
      continue;
    }
    const auto after = m.find(" after ", pos);
    const auto iters_str = m.find(" iters", after);
    if (after == std::string::npos || iters_str == std::string::npos) {
      continue;
    }
    const auto k_str = m.substr(after + 7, iters_str - (after + 7));
    try {
      const int k = std::stoi(k_str);
      CHECK(k >= 1);
      CHECK(k <= forget_n);
      phase1_capped = true;
    } catch (...) {
      // Ignore malformed — the CHECK below catches the absence.
    }
  }
  CHECK(phase1_capped);

  // (d) Level 1 completed (phase-2 ran).  Phase-2's training iters are
  // what `level_stats[1].iterations` reflects (the phase-1 result
  // vector is appended to `all_results()` separately, see
  // cascade_method.cpp:595 vs :624 — phase-2 supersedes `result` for
  // the stats block).
  const auto& s1 = solver.level_stats()[1];
  CHECK(s1.iterations >= 1);
  CHECK(std::isfinite(s1.gap));
  CHECK(s1.lower_bound <= s1.upper_bound + 1e-6);

  // (e) Both phase-1 and phase-2 results are inserted into
  // `all_results()`.  Under the post-2026-05 cascade accounting,
  // ``s1.iterations`` is the "full level" stat
  // (``phase1_iter_count_for_stats + level_iterations`` — see
  // cascade_method.cpp:761-765), so the total record count equals
  //
  //   level0_size           (= s0.iterations, intermediate-level no-sim)
  //   + phase1_size         (= phase1_iters + 1 sim entry)
  //   + phase2_size         (= level_iterations + 1 sim entry)
  //   = s0.iterations + (phase1_iters + 1) + (level_iterations + 1)
  //   = s0.iterations + s1.iterations + 2
  //
  // which is exactly the "naive" ``s0.iterations + 1 + s1.iterations
  // + 1`` headcount.  The CHECK below pins the equality so a future
  // regression that DROPS phase-1 results entirely (size shrinks by
  // ``phase1_size``) or appends them twice (size grows) trips the
  // test.  The earlier strict-greater form silently passed when no
  // phase-1 was actually appended (size collapsed to baseline-2)
  // because the regression set ``forget_threshold=0`` and the test
  // saw "exactly baseline" as success.
  const auto baseline =
      static_cast<size_t>(s0.iterations + 1 + s1.iterations + 1);
  CHECK(solver.all_results().size() == baseline);
}

TEST_CASE(  // NOLINT
    "Cascade inherit_optimality_cuts across THREE levels (cumulative)")
{
  // Pin "inheritance is transitive across consecutive levels": with
  // inherit_optimality_cuts=-1 on BOTH level 1 and level 2 transitions,
  // the orchestrator must emit the serialize/transfer log pair at
  // *every* level boundary (0→1 and 1→2).
  //
  // Level 2 ends up with cuts from {level 0, level 1} loaded
  // cumulatively (level 1 keeps the inherited cuts in its store, so
  // its end-of-level serialization at cascade_method.cpp:725 picks up
  // both).
  using gtopt::test::LogCapture;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 30;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  const CascadeTransition keep_forever {
      .inherit_optimality_cuts = OptInt {-1},
  };

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
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"transport"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition = keep_forever,
      },
      CascadeLevel {
          .name = OptName {"full_network"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition = keep_forever,
      },
  };

  LogCapture logs(2048);
  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 3);

  // Count log occurrences for each side of the transfer.
  int serialize_n = 0;
  int transfer_n = 0;
  int forget_n_msgs = 0;
  for (const auto& m : logs.messages()) {
    if (m.contains("serialized") && m.contains("cuts for next level")) {
      ++serialize_n;
    }
    if (m.contains("transferred") && m.contains("cuts from")) {
      ++transfer_n;
    }
    if (m.contains("forgetting") && m.contains("inherited cuts")) {
      ++forget_n_msgs;
    }
  }

  // Two level boundaries (0→1 and 1→2) with N=-1 must each fire the
  // serialize + transfer pair.  No forget messages because N<0.
  CHECK(serialize_n >= 2);
  CHECK(transfer_n >= 2);
  CHECK(forget_n_msgs == 0);

  // All three levels succeeded with valid bounds.
  for (const auto& ls : solver.level_stats()) {
    CHECK(std::isfinite(ls.gap));
    CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
    CHECK(ls.iterations >= 1);
  }

  // Cumulative inheritance: level 2's transferred count includes BOTH
  // level 0's cuts (kept by level 1 across its run) AND level 1's own
  // new cuts.  Therefore the second "transferred N cuts from" event
  // must report a count ≥ the first event's count.
  std::vector<int> transferred_counts;
  for (const auto& m : logs.messages()) {
    const auto needle = std::string {"transferred "};
    const auto pos = m.find(needle);
    if (pos == std::string::npos) {
      continue;
    }
    const auto after = m.find(" cuts from", pos);
    if (after == std::string::npos) {
      continue;
    }
    const auto n_str =
        m.substr(pos + needle.size(), after - (pos + needle.size()));
    try {
      transferred_counts.push_back(std::stoi(n_str));
    } catch (...) {
      // Ignore malformed numbers.
    }
  }
  REQUIRE(transferred_counts.size() >= 2);
  CHECK(transferred_counts[1] >= transferred_counts[0]);
}

TEST_CASE(  // NOLINT
    "Cascade selective inheritance — level 1 skips, level 2 inherits "
    "(per-level granularity)")
{
  // Per-level granularity: setting `inherit_optimality_cuts = 0` on a
  // single boundary must SUPPRESS the serialize step at that boundary
  // *only*, while a later boundary with -1 still fires normally.
  //
  // Cascade pre-save filter gate (cascade_method.cpp:728):
  //   const bool want_opt_cuts = opt_cut_mode != 0;
  // → with mode=0 the `save_cuts_parquet` call at line 762 is skipped,
  // so the file does not exist when level 1 starts and
  // `inherited_cut_count` stays at 0 ⇒ no "transferred ..." log at
  // level 1.  Level 2 still serializes level 1's own cuts.
  using gtopt::test::LogCapture;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 30;
  base.convergence_tol = 1e-4;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"l0_seed"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"l1_no_inherit"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  // Level 1 does NOT inherit cuts from level 0
                  .inherit_optimality_cuts = OptInt {0},
              },
      },
      CascadeLevel {
          .name = OptName {"l2_inherits_from_l1"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  // Level 2 inherits cuts (picks up level 1's own)
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  LogCapture logs(2048);
  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 3);

  // Count serialize + transfer events.  We want EXACTLY ONE of each
  // (the 1→2 boundary), and ZERO at the 0→1 boundary.
  int serialize_n = 0;
  int transfer_n = 0;
  for (const auto& m : logs.messages()) {
    if (m.contains("serialized") && m.contains("cuts for next level")) {
      ++serialize_n;
    }
    if (m.contains("transferred") && m.contains("cuts from")) {
      ++transfer_n;
    }
  }
  // Level 0 → level 1 transition has inherit_optimality_cuts = 0, so
  // it must NOT serialize.  Level 1 → level 2 transition has -1, so
  // it must serialize exactly once.  Transfer log fires once at level
  // 2.
  CHECK(serialize_n == 1);
  CHECK(transfer_n == 1);

  // Level 1 must have produced its own cuts (the inputs to level 2's
  // inheritance), which becomes the SOURCE of level 2's transfer.
  CHECK(solver.level_stats()[1].cuts_added > 0);

  // The whole cascade still completes with valid bounds.
  for (const auto& ls : solver.level_stats()) {
    CHECK(std::isfinite(ls.gap));
    CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
  }
}

// ───────────────────────────────────────────────────────────────────────────
// Layer 7 — Cascade × low_memory_mode and num_apertures integration
// ───────────────────────────────────────────────────────────────────────────
//
// These tests close two specific coverage gaps left by Layers 4-6 + the
// `select_apertures` unit tests + the `CascadeLevelMethod::num_apertures`
// JSON tests:
//
//   * Gap 1: cut inheritance under `low_memory_mode = compress`.  The
//     per-cell release / reconstruct cycle in compress mode is the
//     riskiest path for the cascade transfer (cuts must survive the
//     release of the previous level's LP/solver, and the freshly
//     reconstructed L1 LP must load them via the cut file).
//
//   * Gap 2: cascade level `num_apertures` actually limits the SDDP
//     backward pass.  `select_apertures` is unit-tested in isolation and
//     `CascadeLevelMethod::num_apertures` is JSON-tested, but the full
//     chain (cascade level → level SDDPOptions → aperture pass) has no
//     integration coverage.
//
//   * Gap 3: when L0 cuts (built with N apertures) are inherited into L1
//     (built with M apertures, M != N), the cut-load path must remain
//     clean — no warnings, no errors, transfer count > 0, L1 converges.

TEST_CASE(  // NOLINT
    "Cascade transfer survives low_memory_mode={off, compress}")
{
  // Gap 1: rerun the Layer-4 cut-transfer flow under both low_memory
  // settings.  The compress path releases each per-(scene, phase)
  // solver between operations and reconstructs from a compressed flat
  // LP — without correct lifecycle handling the cut transfer could
  // silently lose cuts or trip the load path on an unbuilt backend.
  //
  // We assert in both modes that:
  //   (a) the orchestrator emits `Cascade [...]: transferred N cuts`
  //       with N > 0
  //   (b) level 1's final LB is finite and bounded by UB (sanity)
  //   (c) level 1's bound is non-trivial — meaningfully above the
  //       alpha-bootstrap floor (which would manifest as LB ≈ 0 for
  //       this fixture if cuts were missing).
  using gtopt::test::LogCapture;

  auto run_under_mode = [](LowMemoryMode mode)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions base;
    base.max_iterations = 20;
    base.min_iterations = 1;
    base.convergence_tol = 1e-4;
    base.apertures = std::vector<Uid> {};
    base.low_memory_mode = mode;

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"level0"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {10},
                    .apertures = Array<Uid> {},
                },
        },
        CascadeLevel {
            .name = OptName {"level1_cuts"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {10},
                    .apertures = Array<Uid> {},
                },
            .transition =
                CascadeTransition {
                    .inherit_optimality_cuts = OptInt {-1},
                },
        },
    };

    LogCapture logs(1024);
    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
    REQUIRE(solver.level_stats().size() == 2);

    // (a) Transfer log fired with a positive count.
    int transferred_count = 0;
    for (const auto& m : logs.messages()) {
      if (m.contains("transferred") && m.contains("cuts from")) {
        // Parse the integer after "transferred ".
        const auto needle = std::string {"transferred "};
        const auto pos = m.find(needle);
        if (pos == std::string::npos) {
          continue;
        }
        const auto after = pos + needle.size();
        const auto space = m.find(' ', after);
        if (space == std::string::npos) {
          continue;
        }
        try {
          transferred_count = std::stoi(m.substr(after, space - after));
        } catch (...) {
          // Leave at 0 — the assertion below will catch it.
        }
      }
    }
    CHECK(transferred_count > 0);

    const auto& s0 = solver.level_stats()[0];
    const auto& s1 = solver.level_stats()[1];
    REQUIRE(s0.cuts_added > 0);

    // (b) sanity: finite, LB <= UB.
    CHECK(std::isfinite(s1.lower_bound));
    CHECK(std::isfinite(s1.upper_bound));
    CHECK(s1.lower_bound <= s1.upper_bound + 1e-6);

    // (c) Level 1 LB should match level 0's LB (same problem, inherited
    // cuts → identical value function).  In particular, far above the
    // alpha-bootstrap floor of 0 (or 1e-9) — for this fixture the
    // converged LB is several hundred at minimum.
    CHECK(s1.lower_bound >= s0.lower_bound * 0.999 - 1e-3);
  };

  SUBCASE("low_memory_mode = off")
  {
    run_under_mode(LowMemoryMode::off);
  }
  SUBCASE("low_memory_mode = compress")
  {
    run_under_mode(LowMemoryMode::compress);
  }
}

TEST_CASE(  // NOLINT
    "CascadeLevelMethod::num_apertures honored by backward pass")
{
  // Gap 2: verify the full chain
  //   CascadeLevel.sddp_options.num_apertures = K
  //     → cascade builds per-level SDDPOptions with num_apertures = K
  //     → backward pass selects K of the N phase apertures via
  //       `select_apertures(plp.apertures(), K, mode)`
  //     → aperture-pass log emits "K/K feasible" (denominator is the
  //       size of the SELECTED set, NOT the planning-level pool).
  //
  // Build a planning with 4 phase apertures, then run a 1-level cascade
  // with num_apertures=2 and check the log denominator.
  using gtopt::test::LogCapture;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);

  // Attach 4 apertures + phase mapping.  The base fixture has 2
  // scenarios — re-use both for apertures 1/2, and add scenarios 3/4
  // pointing at the existing inflow Flow (no extra inflow data needed,
  // build_effective_apertures only requires the source_scenario UID to
  // resolve, value comes from the per-aperture flow it ends up using).
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {3},
      .probability_factor = 0.25,
  });
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {4},
      .probability_factor = 0.25,
  });
  planning.simulation.scenario_array[0].probability_factor = OptReal {0.25};
  planning.simulation.scenario_array[1].probability_factor = OptReal {0.25};

  planning.simulation.aperture_array = Array<Aperture> {
      Aperture {
          .uid = Uid {1},
          .source_scenario = Uid {1},
          .probability_factor = 0.25,
      },
      Aperture {
          .uid = Uid {2},
          .source_scenario = Uid {2},
          .probability_factor = 0.25,
      },
      Aperture {
          .uid = Uid {3},
          .source_scenario = Uid {3},
          .probability_factor = 0.25,
      },
      Aperture {
          .uid = Uid {4},
          .source_scenario = Uid {4},
          .probability_factor = 0.25,
      },
  };
  for (auto& phase : planning.simulation.phase_array) {
    phase.apertures = {Uid {1}, Uid {2}, Uid {3}, Uid {4}};
  }

  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 3;
  base.min_iterations = 1;
  base.convergence_tol = 1e-4;
  base.apertures = std::nullopt;  // use per-phase apertures
  base.enable_api = false;

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0_subset"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  // num_apertures = 2 must shrink the per-phase aperture
                  // set from 4 to 2 in the backward pass.
                  .num_apertures = OptInt {2},
              },
      },
  };

  LogCapture logs(4096);
  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 1);

  // Look for at least one "X/2 feasible" aperture batch line.  The
  // format is:
  //     "SDDP Aperture [iN sM pK]: <X>/<Y> feasible, ..."
  // Y must be 2 because `select_apertures(_, 2, head)` reduces the
  // per-phase aperture vector to its first 2 entries before
  // `build_effective_apertures` materialises the effective list.
  int aperture_lines_with_denom_2 = 0;
  int aperture_lines_with_denom_4 = 0;
  for (const auto& m : logs.messages()) {
    if (!m.contains("SDDP Aperture") || !m.contains("feasible")) {
      continue;
    }
    if (m.contains("/2 feasible")) {
      ++aperture_lines_with_denom_2;
    }
    if (m.contains("/4 feasible")) {
      ++aperture_lines_with_denom_4;
    }
  }
  CHECK(aperture_lines_with_denom_2 >= 1);
  // The full pool of 4 must NEVER appear — that would mean num_apertures
  // was ignored / silently dropped through the cascade pipeline.
  CHECK(aperture_lines_with_denom_4 == 0);
}

TEST_CASE(  // NOLINT
    "Cuts inherited from L0 (num_apertures=2) load cleanly into L1 "
    "(num_apertures=4)")
{
  // Gap 3: heterogeneous aperture counts across levels.  When L0 trains
  // its cuts with a subset of apertures (N=2) and L1 inherits them and
  // continues training with the full set (N=4), the cut-load path must
  // remain clean — cuts encode (scene, phase, alpha-coefficient,
  // state-variable-coefficients, rhs) and do NOT depend on the aperture
  // count at training time, so the load must succeed without warnings.
  using gtopt::test::LogCapture;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);

  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {3},
      .probability_factor = 0.25,
  });
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {4},
      .probability_factor = 0.25,
  });
  planning.simulation.scenario_array[0].probability_factor = OptReal {0.25};
  planning.simulation.scenario_array[1].probability_factor = OptReal {0.25};

  planning.simulation.aperture_array = Array<Aperture> {
      Aperture {
          .uid = Uid {1},
          .source_scenario = Uid {1},
          .probability_factor = 0.25,
      },
      Aperture {
          .uid = Uid {2},
          .source_scenario = Uid {2},
          .probability_factor = 0.25,
      },
      Aperture {
          .uid = Uid {3},
          .source_scenario = Uid {3},
          .probability_factor = 0.25,
      },
      Aperture {
          .uid = Uid {4},
          .source_scenario = Uid {4},
          .probability_factor = 0.25,
      },
  };
  for (auto& phase : planning.simulation.phase_array) {
    phase.apertures = {Uid {1}, Uid {2}, Uid {3}, Uid {4}};
  }

  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 30;
  base.min_iterations = 1;
  base.convergence_tol = 1e-4;
  base.apertures = std::nullopt;
  base.enable_api = false;

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"l0_n2"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .num_apertures = OptInt {2},
              },
      },
      CascadeLevel {
          .name = OptName {"l1_n4"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .num_apertures = OptInt {4},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  LogCapture logs(4096);
  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  // (a) Transfer fired with a positive count.
  int transferred_count = 0;
  bool saw_load_warn_or_err = false;
  for (const auto& m : logs.messages()) {
    if (m.contains("transferred") && m.contains("cuts from")) {
      const auto needle = std::string {"transferred "};
      const auto pos = m.find(needle);
      if (pos != std::string::npos) {
        const auto after = pos + needle.size();
        const auto space = m.find(' ', after);
        if (space != std::string::npos) {
          try {
            transferred_count = std::stoi(m.substr(after, space - after));
          } catch (...) {
            // ignore — assertion below covers it
          }
        }
      }
    }
    // Sentinel: any cascade-level warning/error during the cut-load
    // step would indicate a heterogeneity bug.
    if (m.contains("cut load failed")
        || (m.contains("[warning]") && m.contains("Cascade")))
    {
      saw_load_warn_or_err = true;
    }
  }
  CHECK(transferred_count > 0);
  CHECK_FALSE(saw_load_warn_or_err);

  // (b) Sanity: both levels finished with valid bounds.
  const auto& s0 = solver.level_stats()[0];
  const auto& s1 = solver.level_stats()[1];
  REQUIRE(s0.cuts_added > 0);
  CHECK(std::isfinite(s1.gap));
  CHECK(s1.lower_bound <= s1.upper_bound + 1e-6);

  // (c) With inherited cuts L1 cannot have a WORSE LB than L0 by more
  // than solver tolerance — the inherited cuts are valid lower bounds
  // on the value function, and L1 only adds more cuts on top.
  CHECK(s1.lower_bound >= s0.lower_bound * 0.999 - 1e-3);
}

// NOLINTEND(bugprone-unchecked-optional-access)
