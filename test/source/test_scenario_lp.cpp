/**
 * @file test_scenario_lp.cpp
 * @brief Tests for Scenario and ScenarioLP classes
 */

#include <doctest/doctest.h>
#include <gtopt/scenario.hpp>
#include <gtopt/scenario_lp.hpp>

TEST_SUITE("Scenario")
{
  TEST_CASE("is_active")
  {
    SUBCASE("default constructed is active")
    {
      const gtopt::Scenario scenario;
      CHECK(scenario.is_active());
    }

    SUBCASE("explicitly active")
    {
      const gtopt::Scenario scenario {.active = true};
      CHECK(scenario.is_active());
    }

    SUBCASE("explicitly inactive")
    {
      const gtopt::Scenario scenario {.active = false};
      CHECK_FALSE(scenario.is_active());
    }

    SUBCASE("with other fields set")
    {
      const gtopt::Scenario scenario {.uid = gtopt::Uid {123},
                                      .name = "Test Scenario",
                                      .active = false,
                                      .probability_factor = 0.5};
      CHECK_FALSE(scenario.is_active());
    }
  }
}

TEST_SUITE("ScenarioLP")
{
  TEST_CASE("is_active")
  {
    SUBCASE("default constructed is active")
    {
      const gtopt::ScenarioLP scenario_lp;
      CHECK(scenario_lp.is_active());
    }

    SUBCASE("active scenario")
    {
      gtopt::Scenario scenario {.active = true};
      const gtopt::ScenarioLP scenario_lp(std::move(scenario));
      CHECK(scenario_lp.is_active());
    }

    SUBCASE("inactive scenario")
    {
      gtopt::Scenario scenario {.active = false};
      const gtopt::ScenarioLP scenario_lp(std::move(scenario));
      CHECK_FALSE(scenario_lp.is_active());
    }

    SUBCASE("with index and scene_index")
    {
      gtopt::Scenario scenario {.active = false};
      const gtopt::ScenarioLP scenario_lp(
          std::move(scenario), gtopt::ScenarioIndex {1}, gtopt::SceneIndex {2});
      CHECK_FALSE(scenario_lp.is_active());
    }
  }

  TEST_CASE("other methods")
  {
    gtopt::Scenario scenario {.uid = gtopt::Uid {123},
                              .name = "Test Scenario",
                              .active = true,
                              .probability_factor = 0.75};
    const gtopt::ScenarioLP scenario_lp(
        std::move(scenario), gtopt::ScenarioIndex {1}, gtopt::SceneIndex {2});

    SUBCASE("uid")
    {
      CHECK(scenario_lp.uid() == gtopt::ScenarioUid {123});
    }

    SUBCASE("probability_factor")
    {
      CHECK(scenario_lp.probability_factor() == 0.75);
    }

    SUBCASE("index")
    {
      CHECK(scenario_lp.index() == gtopt::ScenarioIndex {1});
    }

    SUBCASE("scene_index")
    {
      CHECK(scenario_lp.scene_index() == gtopt::SceneIndex {2});
    }

    SUBCASE("is_first")
    {
      CHECK_FALSE(scenario_lp.is_first());
      const gtopt::ScenarioLP first_scenario(
          gtopt::Scenario {}, gtopt::ScenarioIndex {0}, gtopt::SceneIndex {0});
      CHECK(first_scenario.is_first());
    }
  }
}
