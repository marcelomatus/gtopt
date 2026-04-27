// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_stage_month_guard.cpp
 * @brief     Unit tests for require_stage_month guard
 * @date      2026-04-23
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <stdexcept>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/stage.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/stage_month_guard.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT
{
// Build a StageLP with an optional calendar month.
StageLP make_stage_lp(std::optional<MonthType> month = std::nullopt)
{
  Stage s {
      .uid = Uid {1},
      .month = month,
  };
  return StageLP {std::move(s)};
}
}  // namespace

TEST_CASE("require_stage_month returns month when set")  // NOLINT
{
  const auto slp = make_stage_lp(MonthType::january);
  const auto m = require_stage_month(slp, "VolumeRight", "vr1", "reset_month");
  CHECK(m == MonthType::january);
}

TEST_CASE(
    "require_stage_month returns correct month for each MonthType")  // NOLINT
{
  for (const auto month : {MonthType::january,
                           MonthType::february,
                           MonthType::march,
                           MonthType::april,
                           MonthType::may,
                           MonthType::june,
                           MonthType::july,
                           MonthType::august,
                           MonthType::september,
                           MonthType::october,
                           MonthType::november,
                           MonthType::december})
  {
    const auto slp = make_stage_lp(month);
    const auto m =
        require_stage_month(slp, "UserConstraint", "uc1", "monthly_param");
    CHECK(m == month);
  }
}

TEST_CASE("require_stage_month throws when month is absent")  // NOLINT
{
  const auto slp = make_stage_lp(std::nullopt);
  CHECK_THROWS_AS([[maybe_unused]] const auto r = require_stage_month(
                      slp, "VolumeRight", "vr1", "reset_month"),
                  std::runtime_error);
}

TEST_CASE("require_stage_month error message contains consumer_kind")  // NOLINT
{
  const auto slp = make_stage_lp(std::nullopt);
  bool caught = false;
  try {
    [[maybe_unused]] const auto r =
        require_stage_month(slp, "MyElement", "elem42", "some_feature");
  } catch (const std::runtime_error& ex) {
    caught = true;
    const std::string msg = ex.what();
    CHECK(msg.find("MyElement") != std::string::npos);
    CHECK(msg.find("elem42") != std::string::npos);
    CHECK(msg.find("some_feature") != std::string::npos);
  }
  CHECK(caught);
}

TEST_CASE("require_stage_month error message contains stage uid")  // NOLINT
{
  Stage s {.uid = Uid {99}, .month = std::nullopt};
  const StageLP slp {std::move(s)};
  bool caught = false;
  try {
    [[maybe_unused]] const auto r =
        require_stage_month(slp, "Kind", "id", "feature");
  } catch (const std::runtime_error& ex) {
    caught = true;
    const std::string msg = ex.what();
    CHECK(msg.find("99") != std::string::npos);
  }
  CHECK(caught);
}
