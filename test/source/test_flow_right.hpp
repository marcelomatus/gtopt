// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>

TEST_CASE("FlowRight construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const FlowRight fr;

  CHECK(fr.uid == Uid {unknown_uid});
  CHECK(fr.name == Name {});
  CHECK_FALSE(fr.active.has_value());
  CHECK_FALSE(fr.purpose.has_value());
  CHECK_FALSE(fr.junction.has_value());
  CHECK_FALSE(fr.direction.has_value());
  CHECK_FALSE(fr.fmax.has_value());
  CHECK_FALSE(fr.use_average.has_value());
  CHECK_FALSE(fr.fail_cost.has_value());
  CHECK_FALSE(fr.use_value.has_value());
  CHECK_FALSE(fr.priority.has_value());
  CHECK_FALSE(fr.bound_rule.has_value());
}

TEST_CASE("FlowRight attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  FlowRight fr;

  fr.uid = 1001;
  fr.name = "irrigation_right";
  fr.active = true;
  fr.purpose = "irrigation";
  fr.junction = Uid {7001};
  fr.direction = -1;
  fr.discharge = 50.0;
  fr.fmax = 100.0;
  fr.use_average = true;
  fr.fail_cost = 5000.0;
  fr.use_value = 10.0;
  fr.priority = 1.0;

  CHECK(fr.uid == 1001);
  CHECK(fr.name == "irrigation_right");
  CHECK(std::get<IntBool>(fr.active.value()) == 1);
  REQUIRE(fr.purpose.has_value());
  CHECK(fr.purpose.value() == "irrigation");
  CHECK(std::get<Uid>(fr.junction.value()) == Uid {7001});
  CHECK(fr.direction.value_or(0) == -1);
  CHECK(fr.use_average.value_or(false) == true);
  CHECK(fr.priority.value_or(0.0) == doctest::Approx(1.0));
}

TEST_CASE("FlowRight designated initializer construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const FlowRight fr {
      .uid = Uid {2},
      .name = "env_flow",
      .active = {},
      .purpose = "environmental",
      .junction = SingleId {Uid {10}},
      .direction = -1,
      .discharge = 25.0,
      .fmax = {},
      .use_average = {},
      .fail_cost = 10000.0,
  };

  CHECK(fr.uid == Uid {2});
  CHECK(fr.name == "env_flow");
  REQUIRE(fr.purpose.has_value());
  CHECK(fr.purpose.value() == "environmental");
  CHECK(std::get<Uid>(fr.junction.value()) == Uid {10});
  CHECK(fr.direction.value_or(0) == -1);
}

TEST_CASE("FlowRight with bound rule")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  FlowRight fr;
  fr.uid = 3;
  fr.name = "cushion_right";

  RightBoundRule rule {
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

  fr.bound_rule = rule;

  REQUIRE(fr.bound_rule.has_value());
  CHECK(std::get<Uid>(fr.bound_rule->reservoir) == Uid {9001});
  CHECK(fr.bound_rule->segments.size() == 3);
  REQUIRE(fr.bound_rule->cap.has_value());
  CHECK(fr.bound_rule->cap.value_or(0.0) == doctest::Approx(5000.0));
}

TEST_CASE("FlowRight with monthly discharge schedule")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  FlowRight fr;
  fr.uid = 4;
  fr.name = "seasonal_right";

  // Seasonal irrigation schedule: [S,T,B] = scenario x stage x block
  std::vector<std::vector<std::vector<Real>>> schedule {
      {{0.0, 0.0, 0.0, 19.5, 42.25, 55.25, 65.0, 65.0, 52.0, 32.5, 13.0, 0.0}},
  };
  fr.discharge = schedule;

  auto* vec_ptr =
      std::get_if<std::vector<std::vector<std::vector<Real>>>>(&fr.discharge);
  REQUIRE(vec_ptr != nullptr);
  CHECK(vec_ptr->size() == 1);
  CHECK((*vec_ptr)[0][0].size() == 12);
  CHECK((*vec_ptr)[0][0][3] == doctest::Approx(19.5));
  CHECK((*vec_ptr)[0][0][7] == doctest::Approx(65.0));
}

TEST_CASE("FlowRight with different purposes")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("irrigation purpose")
  {
    const FlowRight fr {
        .uid = Uid {10},
        .name = "irr",
        .active = {},
        .purpose = "irrigation",
    };
    CHECK(fr.purpose.value() == "irrigation");
  }

  SUBCASE("generation purpose")
  {
    const FlowRight fr {
        .uid = Uid {11},
        .name = "gen",
        .active = {},
        .purpose = "generation",
    };
    CHECK(fr.purpose.value() == "generation");
  }

  SUBCASE("environmental purpose")
  {
    const FlowRight fr {
        .uid = Uid {12},
        .name = "env",
        .active = {},
        .purpose = "environmental",
    };
    CHECK(fr.purpose.value() == "environmental");
  }
}
