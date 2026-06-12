// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_stage.cpp
 * @brief     Unit tests for the Stage struct and StageIndex helpers
 * @date      2026-04-23
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <span>

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Stage default construction")  // NOLINT
{
  const Stage s {};
  CHECK(s.uid == unknown_uid);
  CHECK_FALSE(s.name.has_value());
  CHECK_FALSE(s.active.has_value());
  CHECK(s.first_block == 0);
  CHECK(s.count_block == std::dynamic_extent);
  CHECK(s.discount_factor.value_or(1.0) == doctest::Approx(1.0));
  CHECK_FALSE(s.month.has_value());
  CHECK_FALSE(s.chronological.has_value());
  CHECK(s.class_name == "stage");
}

TEST_CASE("Stage is_active defaults to true")  // NOLINT
{
  const Stage s {};
  CHECK(s.is_active());
}

TEST_CASE("Stage is_active respects explicit active=false")  // NOLINT
{
  const Stage s {.uid = Uid {1}, .active = false};
  CHECK_FALSE(s.is_active());
}

TEST_CASE("Stage designated initializer construction")  // NOLINT
{
  const Stage s {
      .uid = Uid {3},
      .name = "year_2030",
      .active = true,
      .first_block = 24,
      .count_block = 8760,
      .discount_factor = 0.9,
  };
  CHECK(s.uid == 3);
  REQUIRE(s.name.has_value());
  CHECK(s.name.value() == "year_2030");
  CHECK(s.is_active());
  CHECK(s.first_block == 24);
  CHECK(s.count_block == 8760);
  CHECK(s.discount_factor.value_or(1.0) == doctest::Approx(0.9));
}

TEST_CASE("Stage month field")  // NOLINT
{
  const Stage s {
      .uid = Uid {1},
      .month = MonthType::january,
  };
  REQUIRE(s.month.has_value());
  CHECK(s.month.value() == MonthType::january);
}

// ─── StageIndex helpers ────────────────────────────────────────────────────

TEST_CASE("first_stage_index returns index 0")  // NOLINT
{
  const StageIndex fi = first_stage_index();
  CHECK(static_cast<std::size_t>(fi) == 0);
}

TEST_CASE("next and previous stage index")  // NOLINT
{
  const StageIndex idx0 = first_stage_index();
  const StageIndex idx1 = next(idx0);
  CHECK(static_cast<std::size_t>(idx1) == 1);
  CHECK(static_cast<std::size_t>(previous(idx1)) == 0);
}

TEST_CASE("StageUid is a UidOf<Stage>")  // NOLINT
{
  static_assert(!std::is_same_v<StageUid, BlockUid>);
  const StageUid su = make_uid<Stage>(5);
  CHECK(su.value() == 5);
}
