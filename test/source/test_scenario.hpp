// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/scenario.hpp>

TEST_CASE("Scenario construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Scenario scenario;

  CHECK(scenario.uid == Uid {unknown_uid});
  CHECK_FALSE(scenario.name.has_value());
  CHECK_FALSE(scenario.active.has_value());
  REQUIRE(scenario.probability_factor.has_value());
  CHECK(scenario.probability_factor.value_or(0.0) == doctest::Approx(1.0));
  CHECK(scenario.class_name == "scenario");
}

TEST_CASE("Scenario is_active default behaviour")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default is active when unset")
  {
    const Scenario scenario;
    CHECK(scenario.is_active() == true);
  }

  SUBCASE("explicitly active")
  {
    Scenario scenario;
    scenario.active = true;
    CHECK(scenario.is_active() == true);
  }

  SUBCASE("explicitly inactive")
  {
    Scenario scenario;
    scenario.active = false;
    CHECK(scenario.is_active() == false);
  }
}

TEST_CASE("Scenario attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Scenario scenario;

  scenario.uid = 1;
  scenario.name = "dry_year";
  scenario.probability_factor = 0.3;

  CHECK(scenario.uid == 1);
  REQUIRE(scenario.name.has_value());
  CHECK(scenario.name.value() == "dry_year");
  CHECK(scenario.probability_factor.value_or(0.0) == doctest::Approx(0.3));
}

TEST_CASE("Scenario designated initializer construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Scenario scenario {
      .uid = Uid {2},
      .name = "wet_year",
      .active = {},
      .probability_factor = 0.7,
  };

  CHECK(scenario.uid == Uid {2});
  REQUIRE(scenario.name.has_value());
  CHECK(scenario.name.value() == "wet_year");
  CHECK(scenario.probability_factor.value_or(0.0) == doctest::Approx(0.7));
}

TEST_CASE("Scenario with zero probability")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Scenario scenario {
      .uid = Uid {3},
      .name = "extreme",
      .active = {},
      .probability_factor = 0.0,
  };

  CHECK(scenario.probability_factor.value_or(1.0) == doctest::Approx(0.0));
}

TEST_CASE("ScenarioUid and ScenarioIndex strong types")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ScenarioUid suid {7};
  const ScenarioIndex sidx {4};

  CHECK(suid == ScenarioUid {7});
  CHECK(sidx == ScenarioIndex {4});
}

TEST_CASE("Scenario array with probability weighting")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Scenario> scenarios {
      {
          .uid = Uid {1},
          .name = "dry",
          .active = {},
          .probability_factor = 0.3,
      },
      {
          .uid = Uid {2},
          .name = "normal",
          .active = {},
          .probability_factor = 0.5,
      },
      {
          .uid = Uid {3},
          .name = "wet",
          .active = {},
          .probability_factor = 0.2,
      },
  };

  CHECK(scenarios.size() == 3);

  // Check probability sums to 1.0
  Real total = 0.0;
  for (const auto& s : scenarios) {
    total += s.probability_factor.value_or(0.0);
  }
  CHECK(total == doctest::Approx(1.0));
}
