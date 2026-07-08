// SPDX-License-Identifier: BSD-3-Clause

/**
 * @file      test_sddp_aperture_functions.hpp
 * @brief     Tests for build_effective_apertures and build_synthetic_apertures
 * @date      2026-03-22
 */

#include <limits>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/aperture.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

using namespace gtopt;

// ─── build_effective_apertures ──────────────────────────────────────────────

TEST_CASE(
    "build_effective_apertures — empty phase_apertures uses all active")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
      {
          .uid = Uid {3},
          .source_scenario = Uid {30},
      },
  };

  const auto result = build_effective_apertures(defs, {});
  REQUIRE(result.size() == 3);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 1);
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[1].count == 1);
  CHECK(result[2].aperture.get().uid == Uid {3});
  CHECK(result[2].count == 1);
}

TEST_CASE(
    "build_effective_apertures — inactive apertures are excluded")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .active = false,
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
  };

  const auto result = build_effective_apertures(defs, {});
  REQUIRE(result.size() == 1);
  CHECK(result[0].aperture.get().uid == Uid {2});
}

TEST_CASE(
    "build_effective_apertures — deduplicates phase_apertures with counts")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
      {
          .uid = Uid {3},
          .source_scenario = Uid {30},
      },
  };

  // Phase apertures: [1, 2, 2, 3, 3, 3]
  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {2},
      Uid {2},
      Uid {3},
      Uid {3},
      Uid {3},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 3);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 1);
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[1].count == 2);
  CHECK(result[2].aperture.get().uid == Uid {3});
  CHECK(result[2].count == 3);
}

TEST_CASE(
    "build_effective_apertures — preserves first-appearance order")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
      {
          .uid = Uid {3},
          .source_scenario = Uid {30},
      },
  };

  // Phase apertures in reverse order
  const std::vector<Uid> phase_aps {
      Uid {3},
      Uid {2},
      Uid {1},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 3);
  CHECK(result[0].aperture.get().uid == Uid {3});
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[2].aperture.get().uid == Uid {1});
}

TEST_CASE(
    "build_effective_apertures — unknown UIDs in phase_apertures are skipped")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
  };

  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {99},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 1);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 1);
}

TEST_CASE(
    "build_effective_apertures — empty defs yields empty result")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {};
  const auto result = build_effective_apertures(defs, {});
  CHECK(result.empty());
}

// Gap C4: pin the count-accumulation contract that backs the per-chunk
// UID memo in solve_apertures_for_phase.  Non-contiguous duplicates
// (e.g. [1, 2, 1]) must be collapsed by UID into a single ApertureEntry
// per UID with count == total occurrences, regardless of layout — the
// memo branch hashes on UID, not list position.
TEST_CASE(
    "build_effective_apertures — non-contiguous duplicates "
    "accumulate counts")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
  };

  // UID 1 appears twice, separated by UID 2.  First-appearance order
  // is [1, 2], so the deduplicated result must be [{uid=1, count=2},
  // {uid=2, count=1}] — the second uid1 must NOT create a new entry.
  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {2},
      Uid {1},
  };
  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 2);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 2);
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[1].count == 1);
}

TEST_CASE(
    "build_effective_apertures — inactive apertures in phase list are skipped")  // NOLINT
{
  using namespace gtopt;

  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .active = false,
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
  };

  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {2},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 1);
  CHECK(result[0].aperture.get().uid == Uid {2});
}

// ─── build_synthetic_apertures ──────────────────────────────────────────────

TEST_CASE(
    "build_synthetic_apertures — creates N apertures with equal probability")  // NOLINT
{
  using namespace gtopt;

  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {
          Scenario {
              .uid = Uid {10},
          },
          first_scenario_index(),
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {20},
          },
          ScenarioIndex {1},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {30},
          },
          ScenarioIndex {2},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {40},
          },
          ScenarioIndex {3},
      },
  };

  const auto result = build_synthetic_apertures(scenarios, 4);
  REQUIRE(result.size() == 4);

  for (const auto& ap : result) {
    CHECK(ap.probability_factor.value_or(0.0) == doctest::Approx(0.25));
    CHECK(ap.uid == ap.source_scenario);
  }

  CHECK(result[0].uid == Uid {10});
  CHECK(result[1].uid == Uid {20});
  CHECK(result[2].uid == Uid {30});
  CHECK(result[3].uid == Uid {40});
}

TEST_CASE("build_synthetic_apertures — caps at scenario count")  // NOLINT
{
  using namespace gtopt;

  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {
          Scenario {
              .uid = Uid {1},
          },
          first_scenario_index(),
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {2},
          },
          ScenarioIndex {1},
      },
  };

  const auto result = build_synthetic_apertures(scenarios, 10);
  REQUIRE(result.size() == 2);
  CHECK(result[0].probability_factor.value_or(0.0) == doctest::Approx(0.5));
  CHECK(result[1].probability_factor.value_or(0.0) == doctest::Approx(0.5));
}

TEST_CASE(
    "build_synthetic_apertures — single aperture gets probability 1.0")  // NOLINT
{
  using namespace gtopt;

  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {
          Scenario {
              .uid = Uid {5},
          },
          first_scenario_index(),
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {6},
          },
          ScenarioIndex {1},
      },
  };

  const auto result = build_synthetic_apertures(scenarios, 1);
  REQUIRE(result.size() == 1);
  CHECK(result[0].uid == Uid {5});
  CHECK(result[0].source_scenario == Uid {5});
  CHECK(result[0].probability_factor.value_or(0.0) == doctest::Approx(1.0));
}

// ─── resolve_effective_apertures ────────────────────────────────────────────
//
// The four-way decision tree in `resolve_effective_apertures` is shared
// by both `backward_pass_with_apertures` and its single-phase variant.
// The semantics that must be preserved:
//
//   case A: requested_uids = nullopt, aperture_defs empty   → nullopt
//   case B: requested_uids = nullopt, aperture_defs present → pass-through
//   case C: requested_uids empty                            → nullopt
//   case D: requested_uids present, aperture_defs present   → filter-by-UID
//   case E: requested_uids present, aperture_defs empty     → synthetic
//   case F: filter / synthesis produces an empty list       → nullopt
//
// Each subcase locks in one branch.  Keeping these in unit tests means
// silently breaking the filter/fallback logic surfaces here instead of
// in a four-hour SDDP regression run.

TEST_CASE(
    "resolve_effective_apertures — case A: nullopt + empty defs")  // NOLINT
{
  // No requested UIDs and no aperture definitions → nothing to do,
  // caller falls back to the non-aperture backward pass.
  Array<Aperture> owned;
  const auto result = resolve_effective_apertures(
      /*aperture_defs=*/ {},
      /*all_scenarios=*/ {},
      /*requested_uids=*/std::nullopt,
      owned,
      "test");
  CHECK_FALSE(result.has_value());
  CHECK(owned.empty());
}

TEST_CASE(
    "resolve_effective_apertures — case B: nullopt + defs pass through")  // NOLINT
{
  // No requested UIDs but defs are present → return the defs span
  // unchanged.  ``owned`` stays untouched so we can assert that the
  // returned span aliases the input, not the scratch buffer.
  const std::vector<Aperture> defs {
      {.uid = Uid {1}, .source_scenario = Uid {10}},
      {.uid = Uid {2}, .source_scenario = Uid {20}},
  };

  Array<Aperture> owned;
  const auto result = resolve_effective_apertures(
      /*aperture_defs=*/defs,
      /*all_scenarios=*/ {},
      /*requested_uids=*/std::nullopt,
      owned,
      "test");

  REQUIRE(result.has_value());
  CHECK(result->size() == defs.size());
  CHECK(result->data() == defs.data());  // aliases input, not `owned`
  CHECK(owned.empty());
}

TEST_CASE(
    "resolve_effective_apertures — case C: empty requested list")  // NOLINT
{
  // requested_uids present but empty: explicit "no apertures
  // requested" — caller must fall back.  Distinct from case A in
  // that aperture_defs MAY exist but is irrelevant.
  const std::vector<Aperture> defs {
      {.uid = Uid {1}, .source_scenario = Uid {10}},
  };

  Array<Aperture> owned;
  const auto result = resolve_effective_apertures(
      /*aperture_defs=*/defs,
      /*all_scenarios=*/ {},
      /*requested_uids=*/std::optional<Array<Uid>> {Array<Uid> {}},
      owned,
      "test");

  CHECK_FALSE(result.has_value());
  CHECK(owned.empty());
}

TEST_CASE("resolve_effective_apertures — case D: filter defs by UID")  // NOLINT
{
  // Requested UIDs present + defs present → keep only the defs
  // whose uid appears in the requested set, preserving definition
  // order.  Unmatched requested UIDs emit a SPDLOG_WARN but do NOT
  // cause failure.
  const std::vector<Aperture> defs {
      {.uid = Uid {1}, .source_scenario = Uid {10}},
      {.uid = Uid {2}, .source_scenario = Uid {20}},
      {.uid = Uid {3}, .source_scenario = Uid {30}},
      {.uid = Uid {4}, .source_scenario = Uid {40}},
  };

  Array<Aperture> owned;
  const Array<Uid> requested {Uid {2}, Uid {4}, Uid {99}};  // 99 = not found

  const auto result = resolve_effective_apertures(
      /*aperture_defs=*/defs,
      /*all_scenarios=*/ {},
      /*requested_uids=*/std::optional<Array<Uid>> {requested},
      owned,
      "test");

  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);
  CHECK((*result)[0].uid == Uid {2});
  CHECK((*result)[1].uid == Uid {4});
  // Returned span aliases `owned`, not the input defs.
  CHECK(result->data() == owned.data());
}

TEST_CASE(
    "resolve_effective_apertures — case E: synthetic from scenarios")  // NOLINT
{
  // requested_uids present + aperture_defs empty → synthesise
  // apertures from the first |min(requested, scenarios)| scenarios,
  // each with equal probability.  See `build_synthetic_apertures`.
  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {Scenario {.uid = Uid {10}}, first_scenario_index()},
      ScenarioLP {Scenario {.uid = Uid {20}}, ScenarioIndex {1}},
      ScenarioLP {Scenario {.uid = Uid {30}}, ScenarioIndex {2}},
  };
  const Array<Uid> requested {Uid {10}, Uid {20}};  // ask for 2

  Array<Aperture> owned;
  const auto result = resolve_effective_apertures(
      /*aperture_defs=*/ {},
      /*all_scenarios=*/scenarios,
      /*requested_uids=*/std::optional<Array<Uid>> {requested},
      owned,
      "test");

  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);
  CHECK((*result)[0].uid == Uid {10});
  CHECK((*result)[1].uid == Uid {20});
  CHECK((*result)[0].probability_factor.value_or(0.0) == doctest::Approx(0.5));
  CHECK((*result)[1].probability_factor.value_or(0.0) == doctest::Approx(0.5));
  CHECK(result->data() == owned.data());
}

TEST_CASE(
    "resolve_effective_apertures — case F: filter result is empty")  // NOLINT
{
  // requested_uids present + aperture_defs present, but the filter
  // produces nothing (no UID overlap) → return nullopt so the caller
  // falls back to the non-aperture backward path.  All requested UIDs
  // log a "not found" warning but the function must still return
  // nullopt rather than an empty span.
  const std::vector<Aperture> defs {
      {.uid = Uid {1}, .source_scenario = Uid {10}},
      {.uid = Uid {2}, .source_scenario = Uid {20}},
  };
  const Array<Uid> requested {Uid {99}, Uid {100}};

  Array<Aperture> owned;
  const auto result = resolve_effective_apertures(
      /*aperture_defs=*/defs,
      /*all_scenarios=*/ {},
      /*requested_uids=*/std::optional<Array<Uid>> {requested},
      owned,
      "test");

  CHECK_FALSE(result.has_value());
  CHECK(owned.empty());
}

// ─── ApertureValueFn concept tests ──────────────────────────────────────────

TEST_CASE("ApertureValueFn — lambda returning value")  // NOLINT
{
  using namespace gtopt;

  const ApertureValueFn fn = [](StageUid /*st*/,
                                BlockUid /*bl*/) -> std::optional<double>
  { return 42.0; };

  const auto val = fn(make_uid<Stage>(0), make_uid<Block>(0));
  REQUIRE(val.has_value());
  CHECK(val.value_or(0.0) == doctest::Approx(42.0));
}

TEST_CASE("ApertureValueFn — lambda returning nullopt")  // NOLINT
{
  using namespace gtopt;

  const ApertureValueFn fn = [](StageUid /*st*/,
                                BlockUid /*bl*/) -> std::optional<double>
  { return std::nullopt; };

  const auto val = fn(make_uid<Stage>(0), make_uid<Block>(0));
  CHECK_FALSE(val.has_value());
}

TEST_CASE("ApertureValueFn — cache-backed lambda")  // NOLINT
{
  using namespace gtopt;

  // Simulate a cache lookup pattern
  std::map<std::pair<int, int>, double> cache {
      {
          {0, 0},
          100.0,
      },
      {
          {0, 1},
          200.0,
      },
      {
          {1, 0},
          300.0,
      },
  };

  const ApertureValueFn fn = [&cache](StageUid st,
                                      BlockUid bl) -> std::optional<double>
  {
    auto key = std::pair {Index {st}, Index {bl}};
    auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
    return std::nullopt;
  };

  CHECK(fn(make_uid<Stage>(0), make_uid<Block>(0)).value_or(0.0)
        == doctest::Approx(100.0));
  CHECK(fn(make_uid<Stage>(0), make_uid<Block>(1)).value_or(0.0)
        == doctest::Approx(200.0));
  CHECK(fn(make_uid<Stage>(1), make_uid<Block>(0)).value_or(0.0)
        == doctest::Approx(300.0));
  CHECK_FALSE(fn(make_uid<Stage>(1), make_uid<Block>(1)).has_value());
}

// ─── ApertureCutResult ──────────────────────────────────────────────────────

TEST_CASE("ApertureCutResult default construction")  // NOLINT
{
  using namespace gtopt;

  const ApertureCutResult result;
  CHECK(result.weight == doctest::Approx(0.0));
  CHECK_FALSE(result.feasible);
  CHECK(result.status == 0);
  CHECK_FALSE(result.cut.has_value());
}

}  // namespace

// ─── SDDPMethod aperture integration tests (through solve()) ────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod aperture with empty aperture_array falls back to Benders")
{
  using namespace gtopt;

  auto planning = make_2phase_2scenario_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.apertures = std::vector<Uid> {};  // empty = no apertures
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  int total_cuts = 0;
  for (const auto& r : *results) {
    total_cuts += r.cuts_added;
  }
  CHECK(total_cuts > 0);  // Benders cuts were generated
}

TEST_CASE(  // NOLINT
    "SDDPMethod aperture with synthetic apertures converges")
{
  using namespace gtopt;

  auto planning = make_2phase_2scenario_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.apertures = std::nullopt;  // use per-phase / synthetic
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  int total_cuts = 0;
  for (const auto& r : *results) {
    total_cuts += r.cuts_added;
  }
  CHECK(total_cuts > 0);
}

TEST_CASE(  // NOLINT
    "SDDPMethod aperture timeout triggers fallback")
{
  using namespace gtopt;

  auto planning = make_2phase_2scenario_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.apertures = std::nullopt;
  sddp_opts.aperture_timeout = 0.001;  // 1ms — likely triggers timeout
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  // Solver should still produce results (fallback to Benders on timeout)
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
}

TEST_CASE(  // NOLINT
    "SDDPMethod aperture on single-scenario problem uses Benders")
{
  using namespace gtopt;

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-3;
  // Request apertures on a 1-scenario problem — should gracefully
  // fall back to standard Benders since there are no alternative
  // scenarios to build apertures from.
  sddp_opts.apertures = std::nullopt;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

// ─── compute_auto_aperture_chunk_size ──────────────────────────────────────

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — single-scene under-saturated → 1")
{
  using namespace gtopt;
  // 14 apertures × 1 scene = 14 work units; pool target = 2 × 16 = 32.
  // Under-saturated → chunk_size 1 (no warm-start chunking needed).
  CHECK(compute_auto_aperture_chunk_size(
            /*n_aps=*/14, /*n_scenes=*/1, /*n_cores=*/16, /*pf=*/2.0)
        == 1);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — multi-scene saturated, raw value "
    "rounded DOWN to strictly-previous power of 2")
{
  using namespace gtopt;
  // 14 apertures × 16 scenes = 224 work units; target = 32 →
  // K_raw = ⌈224/32⌉ = 7 → bit_floor(7-1) = bit_floor(6) = 4.
  CHECK(compute_auto_aperture_chunk_size(14, 16, 16, 2.0) == 4);
  // Exactly-at-pow-2: 8×16/32 = 4 → strictly-previous-pow2 →
  // bit_floor(4-1) = bit_floor(3) = 2 (one step BELOW the input).
  CHECK(compute_auto_aperture_chunk_size(8, 16, 16, 2.0) == 2);
  // One unit over a clean divisor → ceil bumps K_raw by 1: 9×16/32 =
  // 4.5 → 5 → bit_floor(5-1) = bit_floor(4) = 4.
  CHECK(compute_auto_aperture_chunk_size(9, 16, 16, 2.0) == 4);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — heavy oversubscription caps at A "
    "AFTER strictly-previous pow-2 rounding")
{
  using namespace gtopt;
  // 80 apertures × 32 scenes = 2560 work units; target = 32 →
  // K_raw = 80 → bit_floor(80-1) = bit_floor(79) = 64, min(64, 80) = 64.
  CHECK(compute_auto_aperture_chunk_size(80, 32, 16, 2.0) == 64);
  // Push past A: 80 apertures × 64 scenes = 5120; K_raw = 160 →
  // bit_floor(160-1) = bit_floor(159) = 128, min(128, 80) = 80 (cap wins).
  CHECK(compute_auto_aperture_chunk_size(80, 64, 16, 2.0) == 80);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — strictly-previous-pow-2 ladder")
{
  using namespace gtopt;
  // Sweep every K_raw ∈ [1, 17] under a single-scene fixture chosen so
  // K_raw = max_apertures_per_phase exactly (pf=1, cores=1 collapses
  // the target to 1, so K_raw = A · S = A · 1 = A).  Each cell pins
  // the strictly-previous-pow-2 mapping
  // {1→1, 2→1, 3→2, 4→2, 5→4, 6→4, 7→4, 8→4,
  //  9-15→8, 16→8, 17→16 (capped at A=17)}.
  CHECK(compute_auto_aperture_chunk_size(1, 1, 1, 1.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(2, 1, 1, 1.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(3, 1, 1, 1.0) == 2);
  CHECK(compute_auto_aperture_chunk_size(4, 1, 1, 1.0) == 2);
  CHECK(compute_auto_aperture_chunk_size(5, 1, 1, 1.0) == 4);
  CHECK(compute_auto_aperture_chunk_size(6, 1, 1, 1.0) == 4);
  CHECK(compute_auto_aperture_chunk_size(7, 1, 1, 1.0) == 4);
  CHECK(compute_auto_aperture_chunk_size(8, 1, 1, 1.0) == 4);
  CHECK(compute_auto_aperture_chunk_size(9, 1, 1, 1.0) == 8);
  CHECK(compute_auto_aperture_chunk_size(15, 1, 1, 1.0) == 8);
  CHECK(compute_auto_aperture_chunk_size(16, 1, 1, 1.0) == 8);
  // K_raw = 17 → bit_floor(17-1) = bit_floor(16) = 16, capped at A=17.
  CHECK(compute_auto_aperture_chunk_size(17, 1, 1, 1.0) == 16);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — production juan/IPLP shape "
    "16 × 16 × 20-core → K=4")
{
  using namespace gtopt;
  // The juan/IPLP production case: 16 apertures per phase × 16 scenes,
  // pf=2 on a 20-physical-core box.  K_raw = ⌈16·16 / (2·20)⌉ =
  // ⌈6.4⌉ = 7 → bit_floor(7) = 4.  Empirically the fastest auto K
  // on this workload (matches K=8 wall but with 2× fewer chunks).
  CHECK(compute_auto_aperture_chunk_size(16, 16, 20, 2.0) == 4);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — degenerate inputs collapse to 1")
{
  using namespace gtopt;
  // Any zero/negative dimension → safe no-chunking default.
  CHECK(compute_auto_aperture_chunk_size(0, 16, 16, 2.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(14, 0, 16, 2.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(14, 16, 0, 2.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(14, 16, 16, 0.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(14, 16, 16, -1.0) == 1);
  CHECK(compute_auto_aperture_chunk_size(-1, 16, 16, 2.0) == 1);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — single core stretches K to A "
    "AFTER pow-2 rounding")
{
  using namespace gtopt;
  // 1 core, target = 2.  14×1 / 2 = 7 → bit_floor(7) = 4.
  CHECK(compute_auto_aperture_chunk_size(14, 1, 1, 2.0) == 4);
  // 14×16 / 2 = 112 → bit_floor(112) = 64 → capped at A=14.
  CHECK(compute_auto_aperture_chunk_size(14, 16, 1, 2.0) == 14);
}

TEST_CASE(  // NOLINT
    "compute_auto_aperture_chunk_size — parallel_factor scales target "
    "inversely (then strictly-previous pow-2 round)")
{
  using namespace gtopt;
  // pf=1 halves the target → doubles K_raw vs pf=2 (modulo ceil).
  // 14×16 / (1·16) = 14 → bit_floor(14-1) = bit_floor(13) = 8.
  CHECK(compute_auto_aperture_chunk_size(14, 16, 16, 1.0) == 8);
  // 14×16 / (4·16) = 3.5 → ceil 4 → bit_floor(4-1) = bit_floor(3) = 2.
  CHECK(compute_auto_aperture_chunk_size(14, 16, 16, 4.0) == 2);
}

// `constexpr`-evaluation regression: the helper is `constexpr`, so
// the assertions above are also evaluated at compile time.  These
// static_asserts lock the post-pow-2 contract — a future edit that
// makes the body non-constexpr would fail to build, and any
// regression of the bit_floor rounding would fail at compile time.
static_assert(compute_auto_aperture_chunk_size(14, 16, 16, 2.0) == 4);
static_assert(compute_auto_aperture_chunk_size(0, 16, 16, 2.0) == 1);
static_assert(compute_auto_aperture_chunk_size(80, 64, 16, 2.0) == 80);
static_assert(compute_auto_aperture_chunk_size(16, 16, 20, 2.0) == 4);

// ─── select_apertures ──────────────────────────────────────────────────────

TEST_CASE(
    "select_apertures — nullopt num_apertures returns full copy")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}, Uid {3}};
  const auto out = select_apertures(phase_aps, std::nullopt);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == Uid {1});
  CHECK(out[1] == Uid {2});
  CHECK(out[2] == Uid {3});
}

TEST_CASE("select_apertures — head mode picks first N entries")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {
      Uid {10},
      Uid {20},
      Uid {30},
      Uid {40},
      Uid {50},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {2}, ApertureSelectionMode::head);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == Uid {10});
  CHECK(out[1] == Uid {20});
}

TEST_CASE("select_apertures — head is the default mode")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}, Uid {3}, Uid {4}};
  const auto out = select_apertures(phase_aps, std::optional<int> {2});
  REQUIRE(out.size() == 2);
  CHECK(out[0] == Uid {1});
  CHECK(out[1] == Uid {2});
}

TEST_CASE("select_apertures — N = 0 returns empty (every mode)")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}, Uid {3}};
  CHECK(select_apertures(
            phase_aps, std::optional<int> {0}, ApertureSelectionMode::head)
            .empty());
  CHECK(select_apertures(
            phase_aps, std::optional<int> {0}, ApertureSelectionMode::stride)
            .empty());
  CHECK(select_apertures(
            phase_aps, std::optional<int> {0}, ApertureSelectionMode::tail)
            .empty());
}

TEST_CASE(
    "select_apertures — N >= size returns full copy (every mode)")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}};
  for (auto mode : {ApertureSelectionMode::head,
                    ApertureSelectionMode::stride,
                    ApertureSelectionMode::tail})
  {
    const auto out = select_apertures(phase_aps, std::optional<int> {99}, mode);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == Uid {1});
    CHECK(out[1] == Uid {2});
  }
}

TEST_CASE("select_apertures — negative N is treated as 0")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}};
  CHECK(select_apertures(
            phase_aps, std::optional<int> {-5}, ApertureSelectionMode::head)
            .empty());
}

TEST_CASE("select_apertures — empty input passes through")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {};
  CHECK(select_apertures(phase_aps, std::nullopt).empty());
  CHECK(select_apertures(phase_aps, std::optional<int> {4}).empty());
}

TEST_CASE(
    "select_apertures — stride mode picks evenly spaced entries")  // NOLINT
{
  using namespace gtopt;
  // 8 entries with N=4 under endpoint-inclusive formula
  // i*(total-1)/(n-1) = i*7/3:  i=0→0, i=1→2, i=2→4, i=3→7.
  // Picks wettest (Uid{1}) and driest (Uid{8}) plus two interior.
  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {2},
      Uid {3},
      Uid {4},
      Uid {5},
      Uid {6},
      Uid {7},
      Uid {8},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {4}, ApertureSelectionMode::stride);
  REQUIRE(out.size() == 4);
  CHECK(out[0] == Uid {1});  // index 0 (wettest)
  CHECK(out[1] == Uid {3});  // index 7/3 = 2
  CHECK(out[2] == Uid {5});  // index 14/3 = 4
  CHECK(out[3] == Uid {8});  // index 21/3 = 7 (driest)
}

TEST_CASE(
    "select_apertures — stride mode N=1 picks the MEDIAN entry")  // NOLINT
{
  // When n == 1, stride returns phase_apertures[total/2] (the median),
  // NOT the wettest.  This is the canonical single-representative sample
  // for stride semantics; use head/tail for the wettest/driest extremes.
  // 4 entries: indices 0,1,2,3 → median = phase_apertures[4/2=2] = Uid{30}.
  using namespace gtopt;
  const std::vector<Uid> phase_aps {
      Uid {10},
      Uid {20},
      Uid {30},
      Uid {40},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {1}, ApertureSelectionMode::stride);
  REQUIRE(out.size() == 1);
  CHECK(out[0] == Uid {30});  // phase_aps[4/2] = phase_aps[2]
}

TEST_CASE(
    "select_apertures — stride mode N=1 odd-length picks median")  // NOLINT
{
  // 5 entries: median = phase_apertures[5/2=2] = Uid{30}.
  using namespace gtopt;
  const std::vector<Uid> phase_aps {
      Uid {10},
      Uid {20},
      Uid {30},
      Uid {40},
      Uid {50},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {1}, ApertureSelectionMode::stride);
  REQUIRE(out.size() == 1);
  CHECK(out[0] == Uid {30});  // phase_aps[5/2] = phase_aps[2]
}

TEST_CASE(
    "select_apertures — stride mode N=2 always includes first and last")  // NOLINT
{
  // stride N=2: must include wettest (index 0) AND driest (index total-1).
  // Uses the formula i*(total-1)/(n-1): i=0 → 0, i=1 → total-1.
  using namespace gtopt;
  const std::vector<Uid> phase_aps {
      Uid {10},
      Uid {20},
      Uid {30},
      Uid {40},
      Uid {50},
      Uid {60},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {2}, ApertureSelectionMode::stride);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == Uid {10});  // wettest (index 0)
  CHECK(out[1] == Uid {60});  // driest (index total-1 = 5)
}

TEST_CASE(
    "select_apertures — stride mode N=3 includes first, mid, last")  // NOLINT
{
  // 6 entries with N=3: indices i*(6-1)/(3-1) = 0, 2, 5.
  // → Uid{10}, Uid{30}, Uid{60} (first, middle-ish, last).
  using namespace gtopt;
  const std::vector<Uid> phase_aps {
      Uid {10},
      Uid {20},
      Uid {30},
      Uid {40},
      Uid {50},
      Uid {60},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {3}, ApertureSelectionMode::stride);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == Uid {10});  // index 0
  CHECK(out[1] == Uid {30});  // index 0*(6-1)/(3-1)=0, 1*(5)/(2)=2
  CHECK(out[2] == Uid {60});  // index (6-1)=5
}

TEST_CASE(
    "select_apertures — ApertureSelectionMode enum round-trip parsing")  // NOLINT
{
  // Pin the enum name/from_name contract for all three modes and their
  // canonical aliases.  This is the primary entry-point test for item 8.
  using namespace gtopt;
  CHECK(enum_name(ApertureSelectionMode::head) == "head");
  CHECK(enum_name(ApertureSelectionMode::stride) == "stride");
  CHECK(enum_name(ApertureSelectionMode::tail) == "tail");

  CHECK((enum_from_name<ApertureSelectionMode>("head")
         && *enum_from_name<ApertureSelectionMode>("head")
             == ApertureSelectionMode::head));
  CHECK((enum_from_name<ApertureSelectionMode>("stride")
         && *enum_from_name<ApertureSelectionMode>("stride")
             == ApertureSelectionMode::stride));
  CHECK((enum_from_name<ApertureSelectionMode>("tail")
         && *enum_from_name<ApertureSelectionMode>("tail")
             == ApertureSelectionMode::tail));

  // Aliases
  CHECK((enum_from_name<ApertureSelectionMode>("first")
         && *enum_from_name<ApertureSelectionMode>("first")
             == ApertureSelectionMode::head));
  CHECK((enum_from_name<ApertureSelectionMode>("interleave")
         && *enum_from_name<ApertureSelectionMode>("interleave")
             == ApertureSelectionMode::stride));
  CHECK((enum_from_name<ApertureSelectionMode>("spread")
         && *enum_from_name<ApertureSelectionMode>("spread")
             == ApertureSelectionMode::stride));
  CHECK((enum_from_name<ApertureSelectionMode>("last")
         && *enum_from_name<ApertureSelectionMode>("last")
             == ApertureSelectionMode::tail));

  CHECK_FALSE(enum_from_name<ApertureSelectionMode>("unknown").has_value());
}

TEST_CASE("select_apertures — tail mode picks last N entries")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {
      Uid {10},
      Uid {20},
      Uid {30},
      Uid {40},
      Uid {50},
  };
  const auto out = select_apertures(
      phase_aps, std::optional<int> {2}, ApertureSelectionMode::tail);
  REQUIRE(out.size() == 2);
  CHECK(out[0] == Uid {40});
  CHECK(out[1] == Uid {50});
}

TEST_CASE("select_apertures — tail mode N=1 picks the driest")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}, Uid {3}};
  const auto out = select_apertures(
      phase_aps, std::optional<int> {1}, ApertureSelectionMode::tail);
  REQUIRE(out.size() == 1);
  CHECK(out[0] == Uid {3});
}

TEST_CASE("select_apertures — head and tail are mirrors")  // NOLINT
{
  using namespace gtopt;
  const std::vector<Uid> phase_aps {Uid {1}, Uid {2}, Uid {3}, Uid {4}};
  const auto head = select_apertures(
      phase_aps, std::optional<int> {2}, ApertureSelectionMode::head);
  const auto tail = select_apertures(
      phase_aps, std::optional<int> {2}, ApertureSelectionMode::tail);
  REQUIRE(head.size() == 2);
  REQUIRE(tail.size() == 2);
  CHECK(head[0] == Uid {1});
  CHECK(head[1] == Uid {2});
  CHECK(tail[0] == Uid {3});
  CHECK(tail[1] == Uid {4});
}

// ─── partition_apertures ───────────────────────────────────────────────────

namespace
{
// Helper: build a small ApertureEntry vector backed by stable Aperture
// storage so the returned spans alias something whose lifetime exceeds
// the partition_apertures call.
struct ApertureBag
{
  std::vector<Aperture> defs;
  std::vector<ApertureEntry> entries;

  explicit ApertureBag(int n)
  {
    defs.reserve(static_cast<std::size_t>(n));
    entries.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      defs.push_back(Aperture {
          .uid = Uid {i + 1},
          .source_scenario = Uid {(i + 1) * 10},
      });
    }
    for (auto& d : defs) {
      entries.push_back(ApertureEntry {.aperture = std::cref(d), .count = 1});
    }
  }
};
}  // namespace

TEST_CASE("partition_apertures — empty input → empty output")  // NOLINT
{
  using namespace gtopt;
  std::vector<ApertureEntry> empty;
  CHECK(partition_apertures(empty, 4).empty());
  CHECK(partition_apertures(empty, 1).empty());
  CHECK(partition_apertures(empty, 0).empty());
}

TEST_CASE("partition_apertures — K=1 yields one chunk per aperture")  // NOLINT
{
  using namespace gtopt;
  ApertureBag bag {5};
  const auto chunks = partition_apertures(bag.entries, 1);
  REQUIRE(chunks.size() == 5);
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    REQUIRE(chunks[i].size() == 1);
    CHECK(chunks[i][0].aperture.get().uid == Uid {static_cast<int>(i) + 1});
  }
}

TEST_CASE("partition_apertures — K>=N yields exactly one chunk")  // NOLINT
{
  using namespace gtopt;
  ApertureBag bag {7};
  for (int k : {7, 8, 100}) {
    const auto chunks = partition_apertures(bag.entries, k);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].size() == 7);
    CHECK(chunks[0].front().aperture.get().uid == Uid {1});
    CHECK(chunks[0].back().aperture.get().uid == Uid {7});
  }
}

TEST_CASE("partition_apertures — K=0 or K<0 collapse to K=1")  // NOLINT
{
  using namespace gtopt;
  ApertureBag bag {3};
  for (int k : {0, -1, -100}) {
    const auto chunks = partition_apertures(bag.entries, k);
    REQUIRE(chunks.size() == 3);
    for (const auto& c : chunks) {
      CHECK(c.size() == 1);
    }
  }
}

TEST_CASE(  // NOLINT
    "partition_apertures — N=10, K=3 yields chunks of sizes [3,3,3,1]")
{
  using namespace gtopt;
  ApertureBag bag {10};
  const auto chunks = partition_apertures(bag.entries, 3);
  REQUIRE(chunks.size() == 4);
  CHECK(chunks[0].size() == 3);
  CHECK(chunks[1].size() == 3);
  CHECK(chunks[2].size() == 3);
  CHECK(chunks[3].size() == 1);
  // Order preserved end-to-end (UIDs 1..10 in their original sequence).
  std::vector<int> seen;
  for (const auto& c : chunks) {
    for (const auto& e : c) {
      seen.push_back(static_cast<int>(e.aperture.get().uid));
    }
  }
  CHECK(seen == std::vector<int> {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
}

TEST_CASE(
    "partition_apertures — N=4, K=2 yields two chunks of size 2")  // NOLINT
{
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-argument-comment,bugprone-unchecked-optional-access)
  ApertureBag bag {4};
  const auto chunks = partition_apertures(bag.entries, 2);
  REQUIRE(chunks.size() == 2);
  CHECK(chunks[0].size() == 2);
  CHECK(chunks[1].size() == 2);
  // Spans alias the input — verify by pointer equality on the first element.
  CHECK(&chunks[0].front() == &bag.entries.front());
  CHECK(&chunks[1].front() == &bag.entries[2]);
}

// NOLINTEND(bugprone-argument-comment,bugprone-unchecked-optional-access)

// ─── dual_shared_bound_correction (Lemma AP2) ───────────────────────────────

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — identical bounds give zero correction")
{
  const std::vector<double> rc {2.5, -1.0, 0.0};
  const std::vector<double> low {0.0, 1.0, -5.0};
  const std::vector<double> upp {10.0, 4.0, 5.0};

  const auto corr = dual_shared_bound_correction(rc, low, upp, low, upp);
  REQUIRE(corr.has_value());
  CHECK(*corr == doctest::Approx(0.0));
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — positive rc prices the lower bound")
{
  // d = 2.0 > 0 → λ = 2.0 on x ≥ l.  Raising l by Δl = 3.0 raises the
  // dual objective (and hence the intercept) by 2.0 × 3.0 = 6.0.
  const std::vector<double> rc {2.0};
  const std::vector<double> rep_low {1.0};
  const std::vector<double> rep_upp {9.0};
  const std::vector<double> ap_low {4.0};
  const std::vector<double> ap_upp {9.0};

  const auto corr =
      dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
  REQUIRE(corr.has_value());
  CHECK(*corr == doctest::Approx(6.0));
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — negative rc prices the upper bound")
{
  // d = −1.5 < 0 → μ = 1.5 on x ≤ u.  Lowering u by 2.0 (Δu = −2.0)
  // raises the dual objective by (−1.5) × (−2.0) = +3.0; raising u by
  // 2.0 lowers it by 3.0.
  const std::vector<double> rc {-1.5};
  const std::vector<double> rep_low {0.0};
  const std::vector<double> rep_upp {8.0};

  SUBCASE("tightened upper bound raises the intercept")
  {
    const std::vector<double> ap_low {0.0};
    const std::vector<double> ap_upp {6.0};
    const auto corr =
        dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
    REQUIRE(corr.has_value());
    CHECK(*corr == doctest::Approx(3.0));
  }

  SUBCASE("relaxed upper bound lowers the intercept")
  {
    const std::vector<double> ap_low {0.0};
    const std::vector<double> ap_upp {10.0};
    const auto corr =
        dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
    REQUIRE(corr.has_value());
    CHECK(*corr == doctest::Approx(-3.0));
  }
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — pinned column (lo == hi) moves both "
    "bounds, only the rc-signed side prices")
{
  // Flow columns are pinned lo == hi == inflow by `update_aperture`.
  // With d > 0 only the Δl term contributes; with d < 0 only Δu.  Both
  // give d × Δ (the equality-constraint sensitivity).
  const std::vector<double> rep_low {5.0, 5.0};
  const std::vector<double> rep_upp {5.0, 5.0};
  const std::vector<double> ap_low {7.0, 7.0};
  const std::vector<double> ap_upp {7.0, 7.0};
  const std::vector<double> rc {3.0, -2.0};

  const auto corr =
      dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
  REQUIRE(corr.has_value());
  // 3.0 × (7−5) + (−2.0) × (7−5) = 6 − 4 = 2.
  CHECK(*corr == doctest::Approx(2.0));
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — zero rc annihilates any delta, even "
    "sentinel/infinite ones")
{
  constexpr double huge = 1.0e30;
  const std::vector<double> rc {0.0};
  const std::vector<double> rep_low {-huge};
  const std::vector<double> rep_upp {huge};
  const std::vector<double> ap_low {0.0};
  const std::vector<double> ap_upp {5.0};

  const auto corr =
      dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
  REQUIRE(corr.has_value());
  CHECK(*corr == doctest::Approx(0.0));
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — priced sentinel delta returns nullopt")
{
  constexpr double huge = 1.0e30;

  SUBCASE("lower bound became bounded under positive rc")
  {
    const std::vector<double> rc {1.0};
    const std::vector<double> rep_low {-huge};
    const std::vector<double> rep_upp {huge};
    const std::vector<double> ap_low {0.0};
    const std::vector<double> ap_upp {huge};
    CHECK_FALSE(
        dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp)
            .has_value());
  }

  SUBCASE("upper bound became unbounded under negative rc")
  {
    const std::vector<double> rc {-1.0};
    const std::vector<double> rep_low {0.0};
    const std::vector<double> rep_upp {5.0};
    const std::vector<double> ap_low {0.0};
    const std::vector<double> ap_upp {huge};
    CHECK_FALSE(
        dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp)
            .has_value());
  }

  SUBCASE("equal sentinel bounds are skipped before any subtraction")
  {
    const std::vector<double> rc {1.0, -1.0};
    const std::vector<double> rep_low {-huge, 0.0};
    const std::vector<double> rep_upp {huge, 4.0};
    const std::vector<double> ap_low {-huge, 1.0};
    const std::vector<double> ap_upp {huge, 4.0};
    const auto corr =
        dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
    REQUIRE(corr.has_value());
    CHECK(*corr == doctest::Approx(0.0));
  }
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — non-finite inputs return nullopt")
{
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();

  SUBCASE("NaN reduced cost")
  {
    const std::vector<double> rc {nan};
    const std::vector<double> b {1.0};
    CHECK_FALSE(dual_shared_bound_correction(rc, b, b, b, b).has_value());
  }

  SUBCASE("infinite priced bound delta")
  {
    const std::vector<double> rc {2.0};
    const std::vector<double> rep_low {-inf};
    const std::vector<double> rep_upp {inf};
    const std::vector<double> ap_low {0.0};
    const std::vector<double> ap_upp {inf};
    CHECK_FALSE(
        dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp)
            .has_value());
  }
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — span size mismatch returns nullopt")
{
  const std::vector<double> rc {1.0, 2.0};
  const std::vector<double> two {0.0, 0.0};
  const std::vector<double> three {0.0, 0.0, 0.0};

  CHECK_FALSE(
      dual_shared_bound_correction(rc, three, two, two, two).has_value());
  CHECK_FALSE(
      dual_shared_bound_correction(rc, two, three, two, two).has_value());
  CHECK_FALSE(
      dual_shared_bound_correction(rc, two, two, three, two).has_value());
  CHECK_FALSE(
      dual_shared_bound_correction(rc, two, two, two, three).has_value());
}

TEST_CASE(  // NOLINT
    "dual_shared_bound_correction — mixed multi-column composition")
{
  // Columns: (d>0, Δl), (d<0, Δu), (d>0, no change), (d=0, Δ both).
  const std::vector<double> rc {2.0, -3.0, 4.0, 0.0};
  const std::vector<double> rep_low {1.0, 0.0, 2.0, 0.0};
  const std::vector<double> rep_upp {9.0, 6.0, 8.0, 1.0};
  const std::vector<double> ap_low {2.5, 0.0, 2.0, 0.5};
  const std::vector<double> ap_upp {9.0, 5.0, 8.0, 2.0};

  const auto corr =
      dual_shared_bound_correction(rc, rep_low, rep_upp, ap_low, ap_upp);
  REQUIRE(corr.has_value());
  // 2.0×1.5 + (−3.0)×(−1.0) + 0 + 0 = 3.0 + 3.0 = 6.0.
  CHECK(*corr == doctest::Approx(6.0));
}
