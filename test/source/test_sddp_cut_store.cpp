// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_cut_store.cpp
 * @brief     Direct unit tests for the SDDPCutStore public API
 * @date      2026-04-10
 *
 * Targets the SDDPCutStore methods that were previously reachable only
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

TEST_CASE("SDDPCutStore - default state is empty")  // NOLINT
{
  const SDDPCutStore store;
  CHECK(store.scene_cuts().empty());
  CHECK(store.num_stored_cuts() == 0);
}

TEST_CASE("SDDPCutStore - resize_scenes sizes per-scene container")  // NOLINT
{
  SDDPCutStore store;
  store.resize_scenes(3);
  REQUIRE(store.scene_cuts().size() == 3);
  for (const auto& sc : store.scene_cuts()) {
    CHECK(sc.empty());
  }
}

TEST_CASE("SDDPCutStore - store_cut writes to per-scene vector")  // NOLINT
{
  SDDPCutStore store;
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
    "SDDPCutStore - feasibility cut stored in the requesting scene")  // NOLINT
{
  SDDPCutStore store;
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

TEST_CASE("SDDPCutStore - multiple cuts across scenes accumulate")  // NOLINT
{
  SDDPCutStore store;
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

TEST_CASE("SDDPCutStore - clear() wipes per-scene containers")  // NOLINT
{
  SDDPCutStore store;
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

TEST_CASE("SDDPCutStore - scene_cuts_before snapshot is mutable")  // NOLINT
{
  SDDPCutStore store;
  store.resize_scenes(3);
  auto& before = store.scene_cuts_before();
  before.assign(3, std::size_t {0});
  CHECK(store.scene_cuts_before().size() == 3);

  store.store_cut(first_scene_index(),
                  first_phase_index(),
                  make_test_cut(5.0),
                  CutType::Optimality,
                  RowIndex {0},
                  make_uid<Scene>(1),
                  make_uid<Phase>(1));

  // Record a snapshot of how many cuts existed before an iteration.
  before[0] = store.scene_cuts()[first_scene_index()].size();
  CHECK(store.scene_cuts_before()[0] == 1);
}
