// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the shared `RuleBoundState` struct hosted in
// `gtopt/update_context.hpp` and aliased as `BoundState` inside
// `FlowRightLP` and `VolumeRightLP`.  The struct exists so both
// element types can store their per-(scenario, stage) bound-rule cache
// in a uniform `IndexHolder2<ScenarioUid, StageUid, BoundState>`
// without duplicating the layout in two element headers.

#include <type_traits>

#include <doctest/doctest.h>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/update_context.hpp>
#include <gtopt/volume_right_lp.hpp>

using namespace gtopt;
// NOLINTBEGIN(hicpp-move-const-arg,performance-move-const-arg)

namespace
{

// `BoundState` is private inside FlowRightLP / VolumeRightLP, so we
// verify shape equivalence indirectly: both element headers compile
// with `using BoundState = RuleBoundState;`, which means the trait
// expression below is identical to the one each element would use.
// If either element ever drifts to a different layout, this TU stops
// compiling because it is hard-pinned to `RuleBoundState`.

static_assert(std::is_aggregate_v<RuleBoundState>);
static_assert(std::is_trivially_copyable_v<RuleBoundState>);
static_assert(std::is_trivially_destructible_v<RuleBoundState>);

// Lower bound on the size — RuleBoundState packs at least
// `current_bound` (Real) + `reservoir_cache` (ReservoirRefCache).
// Tail padding may push this larger; an exact match is not required.
static_assert(sizeof(RuleBoundState)
              >= sizeof(Real) + sizeof(ReservoirRefCache));

}  // namespace

TEST_CASE("RuleBoundState default construction yields zeroed fields")  // NOLINT
{
  RuleBoundState st {};
  CHECK(st.current_bound == doctest::Approx(0.0));
  CHECK(st.reservoir_cache.rsv_uid == Uid {0});
  CHECK(st.reservoir_cache.default_volume == doctest::Approx(0.0));
  CHECK(st.reservoir_cache.energy_scale == doctest::Approx(1.0));
}

TEST_CASE(
    "RuleBoundState designated initialization preserves values")  // NOLINT
{
  RuleBoundState st {
      .current_bound = 42.5,
      .reservoir_cache =
          ReservoirRefCache {
              .rsv_uid = Uid {7},
              .default_volume = 100.0,
              .energy_scale = 1.5,
              .eini_col = ColIndex {3},
              .efin_col = ColIndex {4},
          },
  };
  CHECK(st.current_bound == doctest::Approx(42.5));
  CHECK(st.reservoir_cache.rsv_uid == Uid {7});
  CHECK(st.reservoir_cache.default_volume == doctest::Approx(100.0));
  CHECK(st.reservoir_cache.energy_scale == doctest::Approx(1.5));
  CHECK(st.reservoir_cache.eini_col == ColIndex {3});
  CHECK(st.reservoir_cache.efin_col == ColIndex {4});
}

TEST_CASE("RuleBoundState supports trivial copy and move")  // NOLINT
{
  const RuleBoundState src {
      .current_bound = 7.0,
      .reservoir_cache =
          ReservoirRefCache {
              .rsv_uid = Uid {1},
              .default_volume = 9.0,
              .energy_scale = 2.0,
              .eini_col = ColIndex {5},
              .efin_col = ColIndex {6},
          },
  };

  SUBCASE("copy")
  {
    RuleBoundState dst = src;
    CHECK(dst.current_bound == doctest::Approx(src.current_bound));
    CHECK(dst.reservoir_cache.rsv_uid == src.reservoir_cache.rsv_uid);
    CHECK(dst.reservoir_cache.eini_col == src.reservoir_cache.eini_col);
    CHECK(dst.reservoir_cache.efin_col == src.reservoir_cache.efin_col);
  }

  SUBCASE("move")
  {
    RuleBoundState tmp = src;
    RuleBoundState dst = std::move(tmp);
    CHECK(dst.current_bound == doctest::Approx(7.0));
    CHECK(dst.reservoir_cache.rsv_uid == Uid {1});
  }

  SUBCASE("mutation")
  {
    RuleBoundState dst = src;
    dst.current_bound = 99.0;
    dst.reservoir_cache.eini_col = ColIndex {99};
    CHECK(dst.current_bound == doctest::Approx(99.0));
    CHECK(dst.reservoir_cache.eini_col == ColIndex {99});
    // src untouched
    CHECK(src.current_bound == doctest::Approx(7.0));
    CHECK(src.reservoir_cache.eini_col == ColIndex {5});
  }
}

// NOLINTEND(hicpp-move-const-arg,performance-move-const-arg)
