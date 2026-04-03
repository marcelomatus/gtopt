// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_strong_uid_index.hpp
 * @brief     Tests that Uid and Index strong types are never confused
 *
 * Validates the type-safety invariants that prevent mixing Uid (user-assigned
 * identifier) with Index (array position).  These two concepts are distinct:
 * a scene with uid=10 might be stored at index=0.  Confusing them leads to
 * out-of-bounds access or silent data corruption.
 *
 * The SDDP solver had latent bugs where StoredCut.scene (a SceneUid) was
 * used directly as a SceneIndex for array access.  Strong types exposed the
 * error at compile time once the fields were changed from plain int.
 */

#include <type_traits>

#include <doctest/doctest.h>
#include <gtopt/fmap.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/strong_index_vector.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── SceneUid vs SceneIndex ─────────────────────────────────────────────────

TEST_CASE("SceneUid and SceneIndex are distinct strong types")  // NOLINT
{
  SUBCASE("they are different types")
  {
    static_assert(!std::is_same_v<SceneUid, SceneIndex>);
    CHECK(true);
  }

  SUBCASE("same underlying value does not make them interchangeable")
  {
    // SceneUid{5} and SceneIndex{5} represent different concepts:
    // uid=5 is a user-assigned identifier, index=5 is an array position.
    const SceneUid uid {5};
    const SceneIndex idx {5};

    // Both hold the value 5 in their underlying representation…
    CHECK(static_cast<gtopt::uid_t>(uid) == 5);
    CHECK(static_cast<Index>(idx) == 5);

    // …but they are distinct strong types — not convertible to each other.
    static_assert(!std::is_convertible_v<SceneUid, SceneIndex>);
    static_assert(!std::is_convertible_v<SceneIndex, SceneUid>);
    CHECK(true);
  }

  SUBCASE("uid-to-index lookup resolves non-trivial mapping")
  {
    // Simulate 3 scenes whose UIDs do not match their array positions:
    //   index 0 → uid 10
    //   index 1 → uid 20
    //   index 2 → uid 30
    flat_map<SceneUid, SceneIndex> scene_uid_map;
    scene_uid_map[SceneUid {10}] = SceneIndex {0};
    scene_uid_map[SceneUid {20}] = SceneIndex {1};
    scene_uid_map[SceneUid {30}] = SceneIndex {2};

    // Looking up by UID yields the correct index
    CHECK(scene_uid_map.at(SceneUid {10}) == SceneIndex {0});
    CHECK(scene_uid_map.at(SceneUid {20}) == SceneIndex {1});
    CHECK(scene_uid_map.at(SceneUid {30}) == SceneIndex {2});

    // Index values used as UIDs are correctly absent — they are different
    // concepts even though they share the same underlying integer type.
    CHECK_FALSE(scene_uid_map.contains(SceneUid {0}));
    CHECK_FALSE(scene_uid_map.contains(SceneUid {1}));
    CHECK_FALSE(scene_uid_map.contains(SceneUid {2}));
  }

  SUBCASE("StoredCut.scene is a SceneUid, not an index")
  {
    static_assert(std::is_same_v<decltype(StoredCut::scene), SceneUid>);
    CHECK(true);
  }
}

// ─── PhaseUid vs PhaseIndex ─────────────────────────────────────────────────

TEST_CASE("PhaseUid and PhaseIndex are distinct strong types")  // NOLINT
{
  SUBCASE("they are different types")
  {
    static_assert(!std::is_same_v<PhaseUid, PhaseIndex>);
    CHECK(true);
  }

  SUBCASE("same underlying value does not make them interchangeable")
  {
    const PhaseUid uid {3};
    const PhaseIndex idx {3};

    CHECK(static_cast<gtopt::uid_t>(uid) == 3);
    CHECK(static_cast<Index>(idx) == 3);

    static_assert(!std::is_convertible_v<PhaseUid, PhaseIndex>);
    static_assert(!std::is_convertible_v<PhaseIndex, PhaseUid>);
    CHECK(true);
  }

  SUBCASE("uid-to-index lookup resolves non-trivial mapping")
  {
    // Phases with non-sequential UIDs:
    //   index 0 → uid 100
    //   index 1 → uid 200
    flat_map<PhaseUid, PhaseIndex> phase_uid_map;
    phase_uid_map[PhaseUid {100}] = PhaseIndex {0};
    phase_uid_map[PhaseUid {200}] = PhaseIndex {1};

    CHECK(phase_uid_map.at(PhaseUid {100}) == PhaseIndex {0});
    CHECK(phase_uid_map.at(PhaseUid {200}) == PhaseIndex {1});

    // Using the index value as a UID key must not find anything
    CHECK_FALSE(phase_uid_map.contains(PhaseUid {0}));
    CHECK_FALSE(phase_uid_map.contains(PhaseUid {1}));
  }

  SUBCASE("StoredCut.phase is a PhaseUid, not an index")
  {
    static_assert(std::is_same_v<decltype(StoredCut::phase), PhaseUid>);
    CHECK(true);
  }
}

// ─── Cross-type safety: UID ≠ Index for array access ────────────────────────

TEST_CASE("UID must not be used as array index in StrongIndexVector")  // NOLINT
{
  SUBCASE("scene: UID-based lookup finds correct data")
  {
    // 3 scenes with non-trivial uid→index mapping
    constexpr int num_scenes = 3;
    StrongIndexVector<SceneIndex, double> scene_values;
    scene_values.resize(num_scenes);
    scene_values[SceneIndex {0}] = 100.0;
    scene_values[SceneIndex {1}] = 200.0;
    scene_values[SceneIndex {2}] = 300.0;

    // Scene UIDs: 10, 20, 30 — not equal to indices 0, 1, 2
    flat_map<SceneUid, SceneIndex> uid_map;
    uid_map[SceneUid {10}] = SceneIndex {0};
    uid_map[SceneUid {20}] = SceneIndex {1};
    uid_map[SceneUid {30}] = SceneIndex {2};

    // Correct pattern: look up UID → get index → access vector
    const auto it = uid_map.find(SceneUid {20});
    REQUIRE(it != uid_map.end());
    CHECK(scene_values[it->second] == doctest::Approx(200.0));

    // Wrong pattern (the old bug): using UID value directly as index
    // SceneUid{20} has underlying value 20, which is out of bounds for
    // a 3-element vector.  The strong type system prevents this at
    // compile time: SceneUid cannot be used where SceneIndex is expected.
    static_assert(!std::is_convertible_v<SceneUid, SceneIndex>);
    CHECK(true);
  }

  SUBCASE("phase: UID-based lookup finds correct data")
  {
    constexpr int num_phases = 2;
    StrongIndexVector<PhaseIndex, double> phase_values;
    phase_values.resize(num_phases);
    phase_values[PhaseIndex {0}] = 1.5;
    phase_values[PhaseIndex {1}] = 2.5;

    flat_map<PhaseUid, PhaseIndex> uid_map;
    uid_map[PhaseUid {100}] = PhaseIndex {0};
    uid_map[PhaseUid {200}] = PhaseIndex {1};

    const auto it = uid_map.find(PhaseUid {200});
    REQUIRE(it != uid_map.end());
    CHECK(phase_values[it->second] == doctest::Approx(2.5));

    static_assert(!std::is_convertible_v<PhaseUid, PhaseIndex>);
    CHECK(true);
  }
}

// ─── StoredCut round-trip with strong types ─────────────────────────────────

TEST_CASE("StoredCut preserves strong-typed scene and phase UIDs")  // NOLINT
{
  const StoredCut cut {
      .type = CutType::Optimality,
      .phase = PhaseUid {42},
      .scene = SceneUid {7},
      .name = "test_cut",
      .rhs = -1.5,
      .coefficients =
          {
              {ColIndex {0}, 1.0},
              {ColIndex {3}, -2.0},
          },
  };

  CHECK(cut.phase == PhaseUid {42});
  CHECK(cut.scene == SceneUid {7});
  CHECK(cut.type == CutType::Optimality);
  CHECK(cut.rhs == doctest::Approx(-1.5));
  CHECK(cut.coefficients.size() == 2);

  // Verify the stored UID values are not accidentally indices
  CHECK(static_cast<gtopt::uid_t>(cut.phase) == 42);
  CHECK(static_cast<gtopt::uid_t>(cut.scene) == 7);
}
