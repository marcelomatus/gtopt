// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_cut_store.cpp
 * @brief     Direct unit tests for the SDDPCutManager public API
 * @date      2026-04-10
 *
 * Targets the SDDPCutManager methods that were previously reachable only
 * through a full SDDP solve, exercising the pieces that don't require a
 * PlanningLP fixture:
 *   - store_cut() writes to per-scene storage
 *   - clear() wipes per-scene containers
 *   - resize_scenes() + scene_cuts() size
 *   - num_stored_cuts() counts across all scenes
 *   - StoredCut fields populated from SparseRow
 *   - scene_cuts_before snapshot is mutable
 *
 * Post-Phase-1b: `m_stored_cuts_` + global mutex are gone; per-scene
 * vectors in `m_scene_cuts_` are the single source of truth.  A
 * combined view is available via `build_combined_cuts()` (tested via
 * integration paths, not here — that function needs a PlanningLP).
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build a SparseRow that looks like a real Benders cut:
/// two coefficients, a lower bound (rhs), and a class/constraint name.
[[nodiscard]] auto make_test_cut(double rhs, double c1 = 1.5, double c2 = -2.5)
    -> SparseRow
{
  SparseRow row {};
  row.lowb = rhs;
  row.uppb = SparseRow::DblMax;
  row.scale = 1.0;
  row.class_name = "Sddp";
  row.constraint_name = "cut";
  row.variable_uid = Uid {42};
  row[ColIndex {0}] = c1;
  row[ColIndex {1}] = c2;
  return row;
}

}  // namespace

TEST_CASE("SDDPCutManager - default state is empty")  // NOLINT
{
  const SDDPCutManager store;
  CHECK(store.scene_cuts().empty());
  CHECK(store.num_stored_cuts() == 0);
}

TEST_CASE("SDDPCutManager - resize_scenes sizes per-scene container")  // NOLINT
{
  SDDPCutManager store;
  store.resize_scenes(3);
  REQUIRE(store.scene_cuts().size() == 3);
  for (const auto& sc : store.scene_cuts()) {
    CHECK(sc.empty());
  }
}

TEST_CASE("SDDPCutManager - store_cut writes to per-scene vector")  // NOLINT
{
  SDDPCutManager store;
  store.resize_scenes(2);

  const auto cut = make_test_cut(/*rhs=*/10.5);
  store.store_cut(first_scene_index(),
                  PhaseIndex {1},
                  cut,
                  CutType::Optimality,
                  RowIndex {7},
                  make_uid<Scene>(100),
                  make_uid<Phase>(200));

  REQUIRE(store.scene_cuts()[first_scene_index()].size() == 1);
  CHECK(store.scene_cuts()[SceneIndex {1}].empty());
  CHECK(store.num_stored_cuts() == 1);

  const auto& stored = store.scene_cuts()[first_scene_index()].front();
  CHECK(stored.type == CutType::Optimality);
  CHECK(stored.phase_uid == make_uid<Phase>(200));
  CHECK(stored.scene_uid == make_uid<Scene>(100));
  CHECK(stored.rhs == doctest::Approx(10.5));
  CHECK(stored.scale == doctest::Approx(1.0));
  CHECK(stored.row == RowIndex {7});
  REQUIRE(stored.coefficients.size() == 2);
  // Coefficients preserved from the SparseRow's flat_map (sorted by ColIndex).
  CHECK(stored.coefficients[0].first == ColIndex {0});
  CHECK(stored.coefficients[0].second == doctest::Approx(1.5));
  CHECK(stored.coefficients[1].first == ColIndex {1});
  CHECK(stored.coefficients[1].second == doctest::Approx(-2.5));
}

TEST_CASE(
    "SDDPCutManager - feasibility cut stored in the requesting scene")  // NOLINT
{
  SDDPCutManager store;
  store.resize_scenes(2);

  store.store_cut(SceneIndex {1},
                  first_phase_index(),
                  make_test_cut(/*rhs=*/3.0),
                  CutType::Feasibility,
                  RowIndex {12},
                  make_uid<Scene>(7),
                  make_uid<Phase>(11));

  CHECK(store.scene_cuts()[first_scene_index()].empty());
  REQUIRE(store.scene_cuts()[SceneIndex {1}].size() == 1);
  CHECK(store.num_stored_cuts() == 1);

  const auto& stored = store.scene_cuts()[SceneIndex {1}].front();
  CHECK(stored.type == CutType::Feasibility);
  CHECK(stored.phase_uid == make_uid<Phase>(11));
  CHECK(stored.scene_uid == make_uid<Scene>(7));
  CHECK(stored.row == RowIndex {12});
}

TEST_CASE("SDDPCutManager - multiple cuts across scenes accumulate")  // NOLINT
{
  SDDPCutManager store;
  store.resize_scenes(3);

  store.store_cut(first_scene_index(),
                  PhaseIndex {1},
                  make_test_cut(1.0),
                  CutType::Optimality,
                  RowIndex {10},
                  make_uid<Scene>(1),
                  make_uid<Phase>(1));
  store.store_cut(first_scene_index(),
                  PhaseIndex {1},
                  make_test_cut(2.0),
                  CutType::Optimality,
                  RowIndex {11},
                  make_uid<Scene>(1),
                  make_uid<Phase>(1));
  store.store_cut(SceneIndex {2},
                  PhaseIndex {1},
                  make_test_cut(3.0),
                  CutType::Feasibility,
                  RowIndex {20},
                  make_uid<Scene>(3),
                  make_uid<Phase>(1));

  CHECK(store.scene_cuts()[first_scene_index()].size() == 2);
  CHECK(store.scene_cuts()[SceneIndex {1}].empty());
  CHECK(store.scene_cuts()[SceneIndex {2}].size() == 1);
  CHECK(store.num_stored_cuts() == 3);

  // Scene-0 cuts preserve insertion order.
  CHECK(store.scene_cuts()[first_scene_index()][0].rhs == doctest::Approx(1.0));
  CHECK(store.scene_cuts()[first_scene_index()][1].rhs == doctest::Approx(2.0));
  // The scene-2 fcut is distinct.
  CHECK(store.scene_cuts()[SceneIndex {2}][0].rhs == doctest::Approx(3.0));
  CHECK(store.scene_cuts()[SceneIndex {2}][0].type == CutType::Feasibility);
}

TEST_CASE("SDDPCutManager - clear() wipes per-scene containers")  // NOLINT
{
  SDDPCutManager store;
  store.resize_scenes(2);

  store.store_cut(first_scene_index(),
                  first_phase_index(),
                  make_test_cut(1.0),
                  CutType::Optimality,
                  RowIndex {0},
                  make_uid<Scene>(1),
                  make_uid<Phase>(1));
  store.store_cut(SceneIndex {1},
                  first_phase_index(),
                  make_test_cut(2.0),
                  CutType::Optimality,
                  RowIndex {1},
                  make_uid<Scene>(2),
                  make_uid<Phase>(1));

  REQUIRE(store.num_stored_cuts() == 2);

  store.clear();

  REQUIRE(store.scene_cuts().size() == 2);
  CHECK(store.scene_cuts()[first_scene_index()].empty());
  CHECK(store.scene_cuts()[SceneIndex {1}].empty());
  CHECK(store.num_stored_cuts() == 0);
}

TEST_CASE("SceneCutStore - cuts_before snapshot is per-scene")  // NOLINT
{
  // Migrated from the legacy "scene_cuts_before snapshot is mutable"
  // test that exercised the parallel `m_scene_cuts_before_` vector.
  // After plan step 4 the snapshot lives on each `SceneCutStore`'s
  // `cuts_before()` member; the parallel global vector is gone.
  SDDPCutManager store;
  store.resize_scenes(3);

  // Defaults to zero on each scene.
  for (Index si = 0; si < 3; ++si) {
    CHECK(store.at(SceneIndex {si}).cuts_before() == 0);
  }

  store.store_cut(first_scene_index(),
                  first_phase_index(),
                  make_test_cut(5.0),
                  CutType::Optimality,
                  RowIndex {0},
                  make_uid<Scene>(1),
                  make_uid<Phase>(1));

  // Record a snapshot of how many cuts existed before an iteration on
  // scene 0; other scenes remain at zero.
  store.at(first_scene_index())
      .set_cuts_before(store.at(first_scene_index()).size());
  CHECK(store.at(first_scene_index()).cuts_before() == 1);
  CHECK(store.at(SceneIndex {1}).cuts_before() == 0);
  CHECK(store.at(SceneIndex {2}).cuts_before() == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Direct SceneCutStore tests — pin the behaviour of methods migrated
// from `SDDPCutManager` in plan steps 2a-2c so subsequent migrations
// (steps 2d, 2e, 6) cannot regress them silently.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SceneCutStore - default state is empty")  // NOLINT
{
  const SceneCutStore sc;
  CHECK(sc.empty());
  CHECK(sc.size() == 0);
  CHECK(sc.cuts().empty());
  CHECK(sc.cuts_before() == 0);
}

TEST_CASE(
    "SceneCutStore - store() populates per-scene vector directly")  // NOLINT
{
  // Plan step 2b migrated `SDDPCutManager::store_cut(...)` to
  // `SceneCutStore::store(...)` (without the leading `SceneIndex`
  // parameter — ownership is implicit from the SceneCutStore
  // instance).  This test exercises the migrated method directly.
  SceneCutStore sc;

  const auto cut = make_test_cut(/*rhs=*/12.5, /*c1=*/2.0, /*c2=*/-3.0);
  sc.store(cut,
           CutType::Optimality,
           RowIndex {7},
           make_uid<Scene>(100),
           make_uid<Phase>(200));

  REQUIRE(sc.size() == 1);
  REQUIRE_FALSE(sc.empty());

  const auto& stored = sc.front();
  CHECK(stored.type == CutType::Optimality);
  CHECK(stored.phase_uid == make_uid<Phase>(200));
  CHECK(stored.scene_uid == make_uid<Scene>(100));
  CHECK(stored.rhs == doctest::Approx(12.5));
  CHECK(stored.scale == doctest::Approx(1.0));
  CHECK(stored.row == RowIndex {7});
  REQUIRE(stored.coefficients.size() == 2);
  CHECK(stored.coefficients[0].first == ColIndex {0});
  CHECK(stored.coefficients[0].second == doctest::Approx(2.0));
  CHECK(stored.coefficients[1].first == ColIndex {1});
  CHECK(stored.coefficients[1].second == doctest::Approx(-3.0));
}

TEST_CASE("SceneCutStore - vector-like forwarders match std::vector")  // NOLINT
{
  // The wrapper exposes `size`, `empty`, `front/back`, `operator[]`,
  // range iteration, `push_back`, `clear`, `resize`, `erase`,
  // `emplace_back` — all forwarded to `m_cuts_`.  This test pins
  // each one so the layout swap done in plan step 1 stays
  // semantically transparent for legacy call sites.
  SceneCutStore sc;
  CHECK(sc.empty());

  // push_back (lvalue + rvalue overloads).
  StoredCut a;
  a.rhs = 1.0;
  sc.push_back(a);  // copy
  StoredCut b;
  b.rhs = 2.0;
  sc.push_back(std::move(b));  // move
  CHECK(sc.size() == 2);
  CHECK(sc.front().rhs == doctest::Approx(1.0));
  CHECK(sc.back().rhs == doctest::Approx(2.0));

  // operator[] read.
  CHECK(sc[0].rhs == doctest::Approx(1.0));
  CHECK(sc[1].rhs == doctest::Approx(2.0));

  // Range-for sums.
  double sum = 0.0;
  for (const auto& cut : sc) {
    sum += cut.rhs;
  }
  CHECK(sum == doctest::Approx(3.0));

  // emplace_back.
  auto& c = sc.emplace_back();
  c.rhs = 3.0;
  CHECK(sc.size() == 3);
  CHECK(sc.back().rhs == doctest::Approx(3.0));

  // erase: drop entry [1] (rhs=2) — survivors are rhs=1 then rhs=3.
  sc.erase(sc.begin() + 1);
  REQUIRE(sc.size() == 2);
  CHECK(sc[0].rhs == doctest::Approx(1.0));
  CHECK(sc[1].rhs == doctest::Approx(3.0));

  // clear (vector-clearing forwarder, distinct from clear_with_lp).
  sc.clear();
  CHECK(sc.empty());
  CHECK(sc.size() == 0);
}

TEST_CASE("SceneCutStore - cuts_before() persists across mutations")  // NOLINT
{
  // The per-scene `cuts_before` snapshot replaces the legacy parallel
  // `m_scene_cuts_before_` vector.  Plan step 4 migrated callers to
  // `at(s).set_cuts_before(n)` / `at(s).cuts_before()`; this test
  // pins the snapshot lifetime — set once, survives subsequent
  // `store()` / `clear()` operations until explicitly reset.
  SceneCutStore sc;

  // Setup: 2 cuts, snapshot at size=2.
  sc.store(make_test_cut(1.0),
           CutType::Optimality,
           RowIndex {0},
           make_uid<Scene>(1),
           make_uid<Phase>(1));
  sc.store(make_test_cut(2.0),
           CutType::Optimality,
           RowIndex {1},
           make_uid<Scene>(1),
           make_uid<Phase>(2));
  sc.set_cuts_before(sc.size());
  REQUIRE(sc.cuts_before() == 2);

  // Add one more cut; snapshot still reflects pre-mutation state.
  sc.store(make_test_cut(3.0),
           CutType::Optimality,
           RowIndex {2},
           make_uid<Scene>(1),
           make_uid<Phase>(3));
  CHECK(sc.size() == 3);
  CHECK(sc.cuts_before() == 2);  // unchanged

  // Clear in-memory vector; snapshot still preserved (clear_with_lp
  // semantics are tested separately).
  sc.clear();
  CHECK(sc.empty());
  CHECK(sc.cuts_before() == 2);

  // Explicit reset.
  sc.set_cuts_before(0);
  CHECK(sc.cuts_before() == 0);
}

TEST_CASE("SceneCutStore - cuts() returns underlying vector")  // NOLINT
{
  // The `cuts()` accessor is the escape hatch for call sites that
  // need a `std::vector<StoredCut>&` — used by `std::erase_if`
  // (plan step 1) and by `save_scene_cuts_csv` (which takes a
  // `std::span<const StoredCut>` requiring contiguous storage).
  SceneCutStore sc;
  sc.store(make_test_cut(1.0),
           CutType::Optimality,
           RowIndex {0},
           make_uid<Scene>(1),
           make_uid<Phase>(1));
  sc.store(make_test_cut(2.0),
           CutType::Feasibility,
           RowIndex {1},
           make_uid<Scene>(1),
           make_uid<Phase>(2));
  REQUIRE(sc.size() == 2);

  // Mutable access through cuts().
  auto& vec = sc.cuts();
  static_assert(std::is_same_v<decltype(vec), std::vector<StoredCut>&>);
  CHECK(vec.size() == 2);

  // std::erase_if works on the underlying vector.
  const auto removed = std::erase_if(
      sc.cuts(),
      [](const StoredCut& c) { return c.type == CutType::Feasibility; });
  CHECK(removed == 1);
  CHECK(sc.size() == 1);
  CHECK(sc.front().type == CutType::Optimality);

  // Const access mirrors mutable.
  const auto& csc = sc;
  static_assert(
      std::is_same_v<decltype(csc.cuts()), const std::vector<StoredCut>&>);
}
