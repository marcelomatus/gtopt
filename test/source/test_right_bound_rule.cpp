// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/json/json_right_bound_rule.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/stage_enums.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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

// -- BoundRuleAxis (Tier 0+ A1 multi-axis) ------------------------------

TEST_CASE("RightBoundRule default axis is reservoir_volume")
{
  const RightBoundRule rule;
  CHECK(rule.axis == BoundRuleAxis::reservoir_volume);
  CHECK(axis_uses_reservoir(rule.axis));
}

TEST_CASE("axis_uses_reservoir predicate")
{
  CHECK(axis_uses_reservoir(BoundRuleAxis::reservoir_volume));
  CHECK_FALSE(axis_uses_reservoir(BoundRuleAxis::stage_month));
}

TEST_CASE("resolve_bound_rule_axis_value - reservoir_volume axis")
{
  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .axis = BoundRuleAxis::reservoir_volume,
  };

  // Volume getter is invoked exactly once for reservoir-volume axis.
  int call_count = 0;
  const auto value =
      resolve_bound_rule_axis_value(rule,
                                    std::optional<MonthType> {MonthType::june},
                                    [&]() -> Real
                                    {
                                      ++call_count;
                                      return 1234.5;
                                    });

  CHECK(value == doctest::Approx(1234.5));
  CHECK(call_count == 1);
}

TEST_CASE("resolve_bound_rule_axis_value - stage_month axis")
{
  const RightBoundRule rule {
      .axis = BoundRuleAxis::stage_month,
  };

  SUBCASE("with month set returns numeric month value")
  {
    // Volume getter is NOT invoked when axis is stage_month.
    int call_count = 0;
    const auto value = resolve_bound_rule_axis_value(
        rule,
        std::optional<MonthType> {MonthType::june},
        [&]() -> Real
        {
          ++call_count;
          return 0.0;
        });

    CHECK(value == doctest::Approx(6.0));
    CHECK(call_count == 0);
  }

  SUBCASE("with no month throws std::runtime_error (fail-fast)")
  {
    // Previously returned 0.0 silently; that degraded into subtly
    // wrong LPs.  The contract now requires callers to supply a
    // calendar month when the rule's axis is stage_month.
    const auto call = [&]()
    {
      const auto v = resolve_bound_rule_axis_value(
          rule, std::optional<MonthType> {}, []() -> Real { return 0.0; });
      (void)v;
    };
    CHECK_THROWS_AS(call(), std::runtime_error);
  }
}

TEST_CASE("evaluate_bound_rule - 12-segment monthly modulation")
{
  // A stage-month-driven rule encodes a 12-segment monthly modulation.
  // Each segment becomes active when stage_month >= breakpoint, so the
  // segment with breakpoint == month is the one selected.
  RightBoundRule rule {
      .axis = BoundRuleAxis::stage_month,
  };
  for (int m = 1; m <= 12; ++m) {
    rule.segments.push_back(RightBoundSegment {
        .volume = static_cast<Real>(m),
        .slope = 0.0,
        .constant = static_cast<Real>(100 * m),
    });
  }

  // June -> 600, December -> 1200, January -> 100.
  CHECK(evaluate_bound_rule(rule, 1.0) == doctest::Approx(100.0));
  CHECK(evaluate_bound_rule(rule, 6.0) == doctest::Approx(600.0));
  CHECK(evaluate_bound_rule(rule, 12.0) == doctest::Approx(1200.0));
}

// -- JSON wire format ---------------------------------------------------

TEST_CASE("RightBoundRule JSON parse - default axis when omitted")
{
  constexpr std::string_view json = R"({
    "reservoir": 9001,
    "segments": [
      {"volume": 0, "slope": 0, "constant": 570}
    ],
    "cap": 5000
  })";

  const auto rule = daw::json::from_json<RightBoundRule>(json);

  CHECK(rule.axis == BoundRuleAxis::reservoir_volume);
  CHECK(std::get<Uid>(rule.reservoir) == Uid {9001});
  CHECK(rule.segments.size() == 1);
}

TEST_CASE("RightBoundRule JSON parse - explicit reservoir_volume axis")
{
  constexpr std::string_view json = R"({
    "axis": "reservoir_volume",
    "reservoir": 42,
    "segments": [
      {"volume": 0, "slope": 1.0, "constant": 0}
    ]
  })";

  const auto rule = daw::json::from_json<RightBoundRule>(json);
  CHECK(rule.axis == BoundRuleAxis::reservoir_volume);
}

TEST_CASE("RightBoundRule JSON parse - stage_month axis without reservoir")
{
  // stage_month rules are allowed to omit the reservoir reference.
  constexpr std::string_view json = R"({
    "axis": "stage_month",
    "segments": [
      {"volume": 1, "slope": 0, "constant": 100},
      {"volume": 6, "slope": 0, "constant": 600},
      {"volume": 12, "slope": 0, "constant": 1200}
    ],
    "cap": 2000
  })";

  const auto rule = daw::json::from_json<RightBoundRule>(json);

  CHECK(rule.axis == BoundRuleAxis::stage_month);
  CHECK(std::get<Uid>(rule.reservoir) == Uid {unknown_uid});
  CHECK(rule.segments.size() == 3);
  CHECK(rule.cap.value_or(0.0) == doctest::Approx(2000.0));
}

TEST_CASE("RightBoundRule JSON parse - invalid axis throws")
{
  constexpr std::string_view json = R"({
    "axis": "not_a_real_axis",
    "segments": []
  })";

  CHECK_THROWS([&] { (void)daw::json::from_json<RightBoundRule>(json); }());
}

// -- Tier 3: edge cases (irrigation test ladder) ------------------------

TEST_CASE("evaluate_bound_rule single-segment rule")  // NOLINT
{
  // A rule with exactly one segment behaves as a single linear function
  // for all volumes >= the segment's breakpoint.  Volumes below the
  // breakpoint still resolve to the first (only) segment because
  // find_active_bound_segment guarantees a non-null result for non-empty
  // segments lists.
  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .segments =
          {
              {
                  .volume = 100.0,
                  .slope = 0.5,
                  .constant = 25.0,
              },
          },
  };

  // Below breakpoint (still uses the only segment): 25 + 0.5 * 50 = 50
  CHECK(evaluate_bound_rule(rule, 50.0) == doctest::Approx(50.0));
  // At breakpoint:                                  25 + 0.5 * 100 = 75
  CHECK(evaluate_bound_rule(rule, 100.0) == doctest::Approx(75.0));
  // Above breakpoint:                               25 + 0.5 * 500 = 275
  CHECK(evaluate_bound_rule(rule, 500.0) == doctest::Approx(275.0));
}

TEST_CASE("evaluate_bound_rule negative volume picks first segment")  // NOLINT
{
  // Even when the input value is negative (e.g. an axis value below the
  // first breakpoint), the rule resolves via the first segment's slope
  // and constant.
  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 100.0,
              },
              {
                  .volume = 500.0,
                  .slope = 1.0,
                  .constant = -400.0,
              },
          },
  };

  // V = -50: first segment (flat 100), regardless of negative input.
  CHECK(evaluate_bound_rule(rule, -50.0) == doctest::Approx(100.0));
  // V = -1e6: still first segment.
  CHECK(evaluate_bound_rule(rule, -1.0e6) == doctest::Approx(100.0));
}

TEST_CASE("evaluate_bound_rule Laja 4-zone breakpoint continuity")  // NOLINT
{
  // Realistic Laja 4-zone irrigation rule with widths derived from
  // _zones_to_bound_rule_segments (gtopt_expand.laja_agreement).
  // Each segment is constructed so the piecewise function is continuous
  // at every breakpoint - that property is what the LP solver needs to
  // produce smooth zone-boundary behaviour.
  //
  // Zones: [0,400), [400,700), [700,1300), [1300,2100)
  // Constants chosen so rights(V) is continuous at each break:
  //   zone 0: rights(V) = 100              (V=400   -> 100)
  //   zone 1: rights(V) = 0.4*V - 60       (V=400   -> 100; V=700  -> 220)
  //   zone 2: rights(V) = 0.25*V + 45      (V=700   -> 220; V=1300 -> 370)
  //   zone 3: rights(V) = 0.10*V + 240     (V=1300  -> 370; V=2100 -> 450)
  const RightBoundRule rule {
      .reservoir = SingleId {Uid {9001}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 100.0,
              },
              {
                  .volume = 400.0,
                  .slope = 0.4,
                  .constant = -60.0,
              },
              {
                  .volume = 700.0,
                  .slope = 0.25,
                  .constant = 45.0,
              },
              {
                  .volume = 1300.0,
                  .slope = 0.10,
                  .constant = 240.0,
              },
          },
  };

  // Continuity check at each interior breakpoint: evaluate just-below and
  // just-above the breakpoint and confirm the gap is below an LP-relevant
  // tolerance (the actual mathematical gap is exactly zero for a
  // continuous piecewise function).
  constexpr Real eps = 1.0e-6;
  for (const Real bp : {400.0, 700.0, 1300.0}) {
    const Real below = evaluate_bound_rule(rule, bp - eps);
    const Real above = evaluate_bound_rule(rule, bp + eps);
    CHECK(below == doctest::Approx(above).epsilon(1.0e-6));
  }

  // Spot-check absolute values at each breakpoint.
  CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(100.0));
  CHECK(evaluate_bound_rule(rule, 400.0) == doctest::Approx(100.0));
  CHECK(evaluate_bound_rule(rule, 700.0) == doctest::Approx(220.0));
  CHECK(evaluate_bound_rule(rule, 1300.0) == doctest::Approx(370.0));
  CHECK(evaluate_bound_rule(rule, 2100.0) == doctest::Approx(450.0));
}

TEST_CASE("evaluate_bound_rule volume exactly at a breakpoint")  // NOLINT
{
  // When the input volume equals a breakpoint exactly,
  // find_active_bound_segment selects the segment whose volume == V (the
  // segment that *starts* at V), not the previous one.  The continuity test
  // above already verifies the numeric continuity; this test is the symbolic
  // equivalent — it locks the tie-breaking convention.
  const RightBoundRule rule {
      .reservoir = SingleId {Uid {1}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 10.0,
              },
              {
                  .volume = 1000.0,
                  .slope = 0.5,
                  .constant = -490.0,
              },
              {
                  .volume = 2000.0,
                  .slope = 0.25,
                  .constant = 10.0,
              },
          },
  };

  // At V=1000 — second segment starts here.
  // rights = -490 + 0.5 * 1000 = 10  (matches first segment's constant)
  CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(10.0));
  // At V=2000 — third segment starts here.
  // rights = 10 + 0.25 * 2000 = 510
  // (continuous with second: -490 + 0.5*2000 = 510)
  CHECK(evaluate_bound_rule(rule, 2000.0) == doctest::Approx(510.0));
}

TEST_CASE("RightBoundRule JSON round-trip preserves axis")
{
  const RightBoundRule original {
      .segments =
          {
              {.volume = 1.0, .slope = 0.0, .constant = 100.0},
              {.volume = 7.0, .slope = 0.0, .constant = 700.0},
          },
      .cap = 1500.0,
      .axis = BoundRuleAxis::stage_month,
  };

  const auto json_str = daw::json::to_json(original);
  const auto restored = daw::json::from_json<RightBoundRule>(json_str);

  CHECK(restored.axis == BoundRuleAxis::stage_month);
  CHECK(restored.segments.size() == 2);
  CHECK(restored.cap.value_or(0.0) == doctest::Approx(1500.0));
}
