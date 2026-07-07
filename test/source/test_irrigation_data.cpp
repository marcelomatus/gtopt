// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_irrigation_data.hpp
 * @brief     Unit tests for FlowRight and VolumeRight data
 * @date      2026-04-01
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/volume_right.hpp>

using namespace gtopt;

TEST_CASE("Irrigation data: FlowRight construction and default values")
{
  using namespace gtopt;

  const FlowRight fr;

  CHECK(fr.uid == Uid {unknown_uid});
  CHECK(fr.name == Name {});
  CHECK_FALSE(fr.active.has_value());
  CHECK_FALSE(fr.purpose.has_value());
  CHECK_FALSE(fr.junction_a.has_value());
  CHECK_FALSE(fr.direction.has_value());
  CHECK_FALSE(fr.fcost.has_value());
  CHECK_FALSE(fr.priority.has_value());
}

TEST_CASE("Irrigation data: FlowRight attribute assignment")
{
  using namespace gtopt;

  FlowRight fr;

  fr.uid = 100;
  fr.name = "laja_nuevo_riego";
  fr.active = true;
  fr.purpose = "irrigation";
  fr.junction_a = Name {"laja_downstream"};
  fr.target = 65.0;
  fr.fcost = 5000.0;
  fr.priority = 1.0;

  CHECK(fr.uid == 100);
  CHECK(fr.name == "laja_nuevo_riego");
  CHECK(std::get<IntBool>(fr.active.value()) == 1);
  REQUIRE(fr.purpose.has_value());
  CHECK(*fr.purpose == "irrigation");
  REQUIRE(fr.junction_a.has_value());
  CHECK(std::get<Name>(*fr.junction_a) == "laja_downstream");
  REQUIRE(fr.fcost.has_value());
  CHECK(std::get<Real>(*fr.fcost) == doctest::Approx(5000.0));
  CHECK_FALSE(fr.uvalue.has_value());
  CHECK(fr.priority.value_or(0.0) == doctest::Approx(1.0));
}

TEST_CASE("FlowRight with seasonal target schedule")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 101;
  fr.name = "seasonal_right";

  // Target schedule — the field is `OptTBRealFieldSched` so the C++
  // literal uses a 2-D shape (one inner element per block of each
  // stage).  Scalar / 1-D JSON shapes still parse through the variant
  // fallback at JSON-decode time and broadcast across blocks.
  const std::vector<std::vector<Real>> seasonal = {
      {0.0},
      {0.0},
      {0.0},
      {19.5},
      {42.25},
      {55.25},
      {65.0},
      {65.0},
      {52.0},
      {32.5},
      {13.0},
      {0.0},
  };
  fr.target = seasonal;

  REQUIRE(fr.target.has_value());
  auto* vec_ptr = std::get_if<std::vector<std::vector<Real>>>(&*fr.target);
  REQUIRE(vec_ptr != nullptr);
  CHECK(vec_ptr->size() == 12);
  REQUIRE((*vec_ptr)[7].size() == 1);
  CHECK((*vec_ptr)[7][0] == doctest::Approx(65.0));
}

TEST_CASE("VolumeRight construction and default values")
{
  using namespace gtopt;

  const VolumeRight vr;

  CHECK(vr.uid == Uid {unknown_uid});
  CHECK(vr.name == Name {});
  CHECK_FALSE(vr.active.has_value());
  CHECK_FALSE(vr.purpose.has_value());
  CHECK_FALSE(vr.reservoir.has_value());
  CHECK_FALSE(vr.emin.has_value());
  CHECK_FALSE(vr.emax.has_value());
  CHECK_FALSE(vr.ecost.has_value());
  CHECK_FALSE(vr.eini.has_value());
  CHECK_FALSE(vr.efin.has_value());
  CHECK_FALSE(vr.demand.has_value());
  CHECK_FALSE(vr.fmax.has_value());
  CHECK_FALSE(vr.fail_cost.has_value());
  CHECK_FALSE(vr.priority.has_value());
  CHECK_FALSE(vr.use_state_variable.has_value());

  // Default flow_conversion_rate
  REQUIRE(vr.flow_conversion_rate.has_value());
  CHECK(vr.flow_conversion_rate.value_or(0.0)
        == doctest::Approx(VolumeRight::default_flow_conversion_rate));
}

TEST_CASE("VolumeRight attribute assignment")
{
  using namespace gtopt;

  VolumeRight vr;

  vr.uid = 200;
  vr.name = "laja_irrigation_vol";
  vr.active = true;
  vr.purpose = "irrigation";
  vr.reservoir = Name {"laguna_laja"};
  vr.emax = 500.0;
  vr.eini = 0.0;
  vr.demand = 300.0;
  vr.fail_cost = 5000.0;
  vr.priority = 2.0;
  vr.use_state_variable = true;

  CHECK(vr.uid == 200);
  CHECK(vr.name == "laja_irrigation_vol");
  REQUIRE(vr.purpose.has_value());
  CHECK(*vr.purpose == "irrigation");
  REQUIRE(vr.reservoir.has_value());
  CHECK(std::get<Name>(*vr.reservoir) == "laguna_laja");
  CHECK(*std::get_if<Real>(&vr.emax.value()) == doctest::Approx(500.0));
  CHECK(vr.eini.value_or(-1.0) == doctest::Approx(0.0));
  CHECK(*std::get_if<Real>(&vr.demand.value()) == doctest::Approx(300.0));
  CHECK(vr.fail_cost.value_or(0.0) == doctest::Approx(5000.0));
  CHECK(vr.use_state_variable.value_or(false) == true);
}

TEST_CASE("VolumeRight default constants")
{
  using namespace gtopt;

  CHECK(VolumeRight::default_flow_conversion_rate == doctest::Approx(0.0036));
}

TEST_CASE("VolumeRight use_state_variable defaults and explicit set")
{
  using namespace gtopt;

  SUBCASE("default is nullopt (coupled by convention)")
  {
    const VolumeRight vr;
    CHECK_FALSE(vr.use_state_variable.has_value());
    CHECK(vr.use_state_variable.value_or(true) == true);
  }

  SUBCASE("can be set to false (decoupled)")
  {
    VolumeRight vr;
    vr.use_state_variable = false;
    REQUIRE(vr.use_state_variable.has_value());
    CHECK(vr.use_state_variable.value_or(true) == false);
  }

  SUBCASE("can be set to true (explicitly coupled)")
  {
    VolumeRight vr;
    vr.use_state_variable = true;
    REQUIRE(vr.use_state_variable.has_value());
    CHECK(vr.use_state_variable.value_or(false) == true);
  }
}

TEST_CASE("FlowRight with direction")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 102;
  fr.name = "supply_flow";
  fr.direction = 1;
  fr.target = 50.0;

  REQUIRE(fr.direction.has_value());
  CHECK(fr.direction.value_or(0) == 1);
}

TEST_CASE("FlowRight with fmax for variable mode")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 103;
  fr.name = "variable_right";
  fr.fmax = 500.0;

  REQUIRE(fr.fmax.has_value());
  auto* val = std::get_if<Real>(&*fr.fmax);
  REQUIRE(val != nullptr);
  CHECK(*val == doctest::Approx(500.0));

  // target is unset → variable mode [0, fmax] (mode "Variable allocation"
  // in the unified-mode table — would need a uvalue to actually push the
  // column up).
  CHECK_FALSE(fr.target.has_value());
}

TEST_CASE("FlowRight uvalue field")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 110;
  fr.name = "benefit_right";
  fr.uvalue = -500.0;  // negative = penalty for over-delivery

  REQUIRE(fr.uvalue.has_value());
  CHECK(std::get<Real>(*fr.uvalue) == doctest::Approx(-500.0));

  SUBCASE("default is nullopt")
  {
    const FlowRight fr2;
    CHECK_FALSE(fr2.uvalue.has_value());
  }

  SUBCASE("schedule uvalue")
  {
    FlowRight fr3;
    fr3.uid = 111;
    fr3.name = "scheduled_cost";
    // 2-D schedule: per-(stage, block) since PR-C.  Two stages × one
    // block each is the legacy per-stage equivalent.
    const std::vector<std::vector<Real>> sched = {{100.0}, {200.0}};
    fr3.uvalue = sched;

    REQUIRE(fr3.uvalue.has_value());
    auto* vec_ptr = std::get_if<std::vector<std::vector<Real>>>(&*fr3.uvalue);
    REQUIRE(vec_ptr != nullptr);
    CHECK(vec_ptr->size() == 2);
    CHECK((*vec_ptr)[0][0] == doctest::Approx(100.0));
    CHECK((*vec_ptr)[1][0] == doctest::Approx(200.0));
  }
}

TEST_CASE("FlowRight fcost as schedule")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 112;
  fr.name = "scheduled_fail";

  SUBCASE("scalar fcost (backward compatible)")
  {
    fr.fcost = 5000.0;
    REQUIRE(fr.fcost.has_value());
    CHECK(std::get<Real>(*fr.fcost) == doctest::Approx(5000.0));
  }

  SUBCASE("per-stage fcost schedule")
  {
    // OptTBRealFieldSched since PR-C: per-(stage, block).  Three stages
    // × one block each is the legacy per-stage equivalent.
    std::vector<std::vector<Real>> sched = {{1000.0}, {2000.0}, {3000.0}};
    fr.fcost = sched;
    REQUIRE(fr.fcost.has_value());
    auto* vec_ptr = std::get_if<std::vector<std::vector<Real>>>(&*fr.fcost);
    REQUIRE(vec_ptr != nullptr);
    CHECK(vec_ptr->size() == 3);
    CHECK((*vec_ptr)[1][0] == doctest::Approx(2000.0));
  }
}

TEST_CASE("FlowRight use_average flag")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 105;
  fr.name = "avg_right";
  fr.use_average = true;

  CHECK(fr.use_average.value_or(false) == true);

  SUBCASE("default is false")
  {
    const FlowRight fr2;
    CHECK(fr2.use_average.value_or(false) == false);
  }
}

TEST_CASE("VolumeRight with right_reservoir and direction")
{
  using namespace gtopt;

  VolumeRight vr;
  vr.uid = 201;
  vr.name = "child_vol";
  vr.right_reservoir = Name {"parent_vol"};
  vr.direction = -1;

  REQUIRE(vr.right_reservoir.has_value());
  CHECK(std::get<Name>(*vr.right_reservoir) == "parent_vol");
  REQUIRE(vr.direction.has_value());
  CHECK(vr.direction.value_or(0) == -1);
}

// -- RightBoundRule tests --

TEST_CASE("RightBoundSegment default construction")
{
  using namespace gtopt;

  const RightBoundSegment seg;
  CHECK(seg.volume == doctest::Approx(0.0));
  CHECK(seg.slope == doctest::Approx(0.0));
  CHECK(seg.constant == doctest::Approx(0.0));
}

TEST_CASE("evaluate_bound_rule - Laja 4-zone cushion model")
{
  using namespace gtopt;

  // Laja irrigation formula:
  //   Rights = 570 + 0.00*min(V,1200) + 0.40*min(max(V-1200,0),700)
  //          + 0.25*max(V-1900,0)
  // Piecewise-linear segments:
  const RightBoundRule rule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 570.0,
              },
              {
                  .volume = 1200.0,
                  .slope = 0.40,
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

  SUBCASE("below first breakpoint: constant 570")
  {
    CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(570.0));
    CHECK(evaluate_bound_rule(rule, 500.0) == doctest::Approx(570.0));
    CHECK(evaluate_bound_rule(rule, 1200.0) == doctest::Approx(570.0));
  }

  SUBCASE("zone 2: 90 + 0.40 * V")
  {
    // V=1200: 90 + 0.40*1200 = 90 + 480 = 570 (continuous)
    CHECK(evaluate_bound_rule(rule, 1201.0)
          == doctest::Approx(90.0 + 0.40 * 1201.0));
    CHECK(evaluate_bound_rule(rule, 1500.0)
          == doctest::Approx(90.0 + 0.40 * 1500.0));
    CHECK(evaluate_bound_rule(rule, 1900.0)
          == doctest::Approx(90.0 + 0.40 * 1900.0));
  }

  SUBCASE("zone 3: 375 + 0.25 * V")
  {
    // V=1900: 375 + 0.25*1900 = 375 + 475 = 850 (continuous with zone 2)
    CHECK(evaluate_bound_rule(rule, 1901.0)
          == doctest::Approx(375.0 + 0.25 * 1901.0));
    CHECK(evaluate_bound_rule(rule, 3000.0)
          == doctest::Approx(375.0 + 0.25 * 3000.0));
    CHECK(evaluate_bound_rule(rule, 5000.0)
          == doctest::Approx(375.0 + 0.25 * 5000.0));
  }

  SUBCASE("cap applied")
  {
    // At very high volume, cap should kick in
    // V=20000: 375 + 0.25*20000 = 5375, capped to 5000
    CHECK(evaluate_bound_rule(rule, 20000.0) == doctest::Approx(5000.0));
  }
}

TEST_CASE("evaluate_bound_rule - step function (Maule style)")
{
  using namespace gtopt;

  // Simple step function: 0 below 500, 100 above 500
  const RightBoundRule rule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 0.0,
              },
              {
                  .volume = 500.0,
                  .slope = 0.0,
                  .constant = 100.0,
              },
          },
  };

  CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(0.0));
  CHECK(evaluate_bound_rule(rule, 499.0) == doctest::Approx(0.0));
  CHECK(evaluate_bound_rule(rule, 500.0) == doctest::Approx(100.0));
  CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(100.0));
}

TEST_CASE("evaluate_bound_rule - floor clamp")
{
  using namespace gtopt;

  const RightBoundRule rule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = -0.1,
                  .constant = 100.0,
              },
          },
      .floor = 10.0,
  };

  // V=0: 100, V=500: 100-50=50, V=1000: 100-100=0 → clamped to 10
  CHECK(evaluate_bound_rule(rule, 0.0) == doctest::Approx(100.0));
  CHECK(evaluate_bound_rule(rule, 500.0) == doctest::Approx(50.0));
  CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(10.0));
  CHECK(evaluate_bound_rule(rule, 2000.0) == doctest::Approx(10.0));
}

TEST_CASE("evaluate_bound_rule - empty segments returns cap or 0")
{
  using namespace gtopt;

  SUBCASE("no cap, no segments → 0")
  {
    const RightBoundRule rule {
        .reservoir = Uid {1},
    };
    CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(0.0));
  }

  SUBCASE("with cap, no segments → cap")
  {
    const RightBoundRule rule {
        .reservoir = Uid {1},
        .cap = 42.0,
    };
    CHECK(evaluate_bound_rule(rule, 1000.0) == doctest::Approx(42.0));
  }
}

TEST_CASE("FlowRight bound_rule field")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 110;
  fr.name = "bounded_flow";
  fr.target = 50.0;

  CHECK_FALSE(fr.bound_rule.has_value());

  fr.bound_rule = RightBoundRule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 0.0,
              },
              {
                  .volume = 500.0,
                  .slope = 0.5,
                  .constant = -250.0,
              },
          },
      .cap = 200.0,
  };

  REQUIRE(fr.bound_rule.has_value());
  CHECK(fr.bound_rule->segments.size() == 2);
  CHECK(fr.bound_rule->cap.value_or(0.0) == doctest::Approx(200.0));
  CHECK_FALSE(fr.bound_rule->floor.has_value());
}

TEST_CASE("VolumeRight bound_rule field")
{
  using namespace gtopt;
  // NOLINTBEGIN(readability-math-missing-parentheses)

  VolumeRight vr;
  vr.uid = 210;
  vr.name = "bounded_vol";

  CHECK_FALSE(vr.bound_rule.has_value());

  vr.bound_rule = RightBoundRule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 570.0,
              },
              {
                  .volume = 1200.0,
                  .slope = 0.40,
                  .constant = 90.0,
              },
          },
      .cap = 5000.0,
      .floor = 0.0,
  };

  REQUIRE(vr.bound_rule.has_value());
  CHECK(vr.bound_rule->segments.size() == 2);
  CHECK(std::holds_alternative<Uid>(vr.bound_rule->reservoir));
  CHECK(std::get<Uid>(vr.bound_rule->reservoir) == 1);
}

// NOLINTEND(readability-math-missing-parentheses)