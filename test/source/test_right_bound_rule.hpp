// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/right_bound_rule.hpp>

TEST_CASE("RightBoundSegment construction and defaults")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundSegment seg;

  CHECK(seg.volume == doctest::Approx(0.0));
  CHECK(seg.slope == doctest::Approx(0.0));
  CHECK(seg.constant == doctest::Approx(0.0));
}

TEST_CASE("RightBoundSegment designated initializer")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundSegment seg {
      .volume = 1200.0,
      .slope = 0.4,
      .constant = 90.0,
  };

  CHECK(seg.volume == doctest::Approx(1200.0));
  CHECK(seg.slope == doctest::Approx(0.4));
  CHECK(seg.constant == doctest::Approx(90.0));
}

TEST_CASE("RightBoundRule construction and defaults")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule;

  CHECK(std::get<Uid>(rule.reservoir) == Uid {unknown_uid});
  CHECK(rule.segments.empty());
  CHECK_FALSE(rule.cap.has_value());
  CHECK_FALSE(rule.floor.has_value());
}

TEST_CASE("RightBoundRule with Laja cushion zone segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Laja 4-zone example from the documentation:
  // Rights_irr = 570 + 0.00*min(V,1200) + 0.40*min(max(V-1200,0),700)
  //            + 0.25*max(V-1900,0)
  const RightBoundRule rule {
      .reservoir = SingleId {Uid {9001}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 570.0,
              },
              {
                  .volume = 1200.0,
                  .slope = 0.4,
                  .constant = 90.0,
              },
              {
                  .volume = 1900.0,
                  .slope = 0.25,
                  .constant = 375.0,
              },
          },
      .cap = 5000.0,
  };

  CHECK(std::get<Uid>(rule.reservoir) == Uid {9001});
  CHECK(rule.segments.size() == 3);
  REQUIRE(rule.cap.has_value());
  CHECK(rule.cap.value_or(0.0) == doctest::Approx(5000.0));
}

TEST_CASE("find_active_bound_segment")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<RightBoundSegment> segments {
      {
          .volume = 0.0,
          .slope = 0.0,
          .constant = 570.0,
      },
      {
          .volume = 1200.0,
          .slope = 0.4,
          .constant = 90.0,
      },
      {
          .volume = 1900.0,
          .slope = 0.25,
          .constant = 375.0,
      },
  };

  SUBCASE("volume below first breakpoint uses first segment")
  {
    const auto* seg = find_active_bound_segment(segments, -10.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(570.0));
    CHECK(seg->slope == doctest::Approx(0.0));
  }

  SUBCASE("volume at first breakpoint uses first segment")
  {
    const auto* seg = find_active_bound_segment(segments, 0.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(570.0));
  }

  SUBCASE("volume between first and second breakpoints uses first segment")
  {
    const auto* seg = find_active_bound_segment(segments, 600.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(570.0));
    CHECK(seg->slope == doctest::Approx(0.0));
  }

  SUBCASE("volume at second breakpoint uses second segment")
  {
    const auto* seg = find_active_bound_segment(segments, 1200.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(90.0));
    CHECK(seg->slope == doctest::Approx(0.4));
  }

  SUBCASE("volume between second and third breakpoints uses second segment")
  {
    const auto* seg = find_active_bound_segment(segments, 1500.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(90.0));
    CHECK(seg->slope == doctest::Approx(0.4));
  }

  SUBCASE("volume at third breakpoint uses third segment")
  {
    const auto* seg = find_active_bound_segment(segments, 1900.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(375.0));
    CHECK(seg->slope == doctest::Approx(0.25));
  }

  SUBCASE("volume above all breakpoints uses last segment")
  {
    const auto* seg = find_active_bound_segment(segments, 5000.0);
    REQUIRE(seg != nullptr);
    CHECK(seg->constant == doctest::Approx(375.0));
    CHECK(seg->slope == doctest::Approx(0.25));
  }
}

TEST_CASE("evaluate_bound_rule with empty segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .cap = 100.0,
  };

  CHECK(evaluate_bound_rule(rule, 500.0) == doctest::Approx(100.0));
}

TEST_CASE("evaluate_bound_rule with empty segments and no cap")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
  };

  CHECK(evaluate_bound_rule(rule, 500.0) == doctest::Approx(0.0));
}

TEST_CASE("evaluate_bound_rule Laja cushion zone")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {9001}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 570.0,
              },
              {
                  .volume = 1200.0,
                  .slope = 0.4,
                  .constant = 90.0,
              },
              {
                  .volume = 1900.0,
                  .slope = 0.25,
                  .constant = 375.0,
              },
          },
      .cap = 5000.0,
  };

  SUBCASE("at volume 0 -> constant 570")
  {
    CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(570.0));
  }

  SUBCASE("at volume 600 -> constant 570 (flat segment)")
  {
    CHECK(evaluate_bound_rule(rule, 600.0) == doctest::Approx(570.0));
  }

  SUBCASE("at volume 1200 -> 90 + 0.4 * 1200 = 570")
  {
    CHECK(evaluate_bound_rule(rule, 1200.0) == doctest::Approx(570.0));
  }

  SUBCASE("at volume 1500 -> 90 + 0.4 * 1500 = 690")
  {
    CHECK(evaluate_bound_rule(rule, 1500.0) == doctest::Approx(690.0));
  }

  SUBCASE("at volume 1900 -> 375 + 0.25 * 1900 = 850")
  {
    CHECK(evaluate_bound_rule(rule, 1900.0) == doctest::Approx(850.0));
  }

  SUBCASE("at volume 3000 -> 375 + 0.25 * 3000 = 1125")
  {
    CHECK(evaluate_bound_rule(rule, 3000.0) == doctest::Approx(1125.0));
  }
}

TEST_CASE("evaluate_bound_rule with cap")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 1.0,
                  .constant = 0.0,
              },
          },
      .cap = 100.0,
  };

  // Without cap: 0 + 1.0 * 200 = 200
  // With cap of 100: min(200, 100) = 100
  CHECK(evaluate_bound_rule(rule, 200.0) == doctest::Approx(100.0));
  CHECK(evaluate_bound_rule(rule, 50.0) == doctest::Approx(50.0));
}

TEST_CASE("evaluate_bound_rule with floor")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.1,
                  .constant = -50.0,
              },
          },
      .floor = 10.0,
  };

  // At volume 0: -50 + 0.1*0 = -50, clamped to floor 10
  CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(10.0));
  // At volume 1000: -50 + 0.1*1000 = 50, above floor
  CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(50.0));
}

TEST_CASE("evaluate_bound_rule with both cap and floor")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 1.0,
                  .constant = 0.0,
              },
          },
      .cap = 500.0,
      .floor = 50.0,
  };

  CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(50.0));
  CHECK(evaluate_bound_rule(rule, 200.0) == doctest::Approx(200.0));
  CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(500.0));
}
