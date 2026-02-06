#include <doctest/doctest.h>
#include <gtopt/element_index.hpp>  // For ElementIndex
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>

using namespace gtopt;

TEST_SUITE("SceneLP")
{
  TEST_CASE("Default construction")
  {
    const SceneLP scene;
    CHECK(scene.index() == ElementIndex<SceneLP> {unknown_index});
    CHECK(scene.count_scenario() == -1);
    CHECK(scene.first_scenario() == ScenarioIndex {0});
    CHECK(scene.is_active());
  }

  TEST_CASE("Construction with scene and scenarios")
  {
    const Scene scene {
        .uid = 1,
        .name = "test_scene",
        .active = true,
        .first_scenario = 0,
        .count_scenario = 2,
    };

    std::vector<Scenario> scenarios {
        Scenario {.uid = 1, .active = true},
        Scenario {.uid = 2, .active = true},
        Scenario {.uid = 3, .active = false},
    };

    const SceneLP scene_lp(scene, scenarios, SceneIndex {1});

    CHECK(scene_lp.index() == SceneIndex {1});
    CHECK(scene_lp.is_active());
    CHECK(scene_lp.first_scenario() == ScenarioIndex {0});
    CHECK(scene_lp.count_scenario() == 2);
  }

  TEST_CASE("Construction with simulation")
  {
    const Scene scene {
        .uid = 2,
        .name = "simulation_scene",
        .active = true,
        .first_scenario = 1,
        .count_scenario = 1,
    };

    const std::vector<Scenario> scenarios {
        Scenario {.uid = 1, .active = false},
        Scenario {.uid = 2, .active = true},
        Scenario {.uid = 3, .active = true},
    };

    SceneIndex index {2};
    const SceneLP scene_lp(scene, scenarios, index);

    CHECK(scene_lp.index() == index);
    CHECK(scene_lp.is_active());
    CHECK(scene_lp.first_scenario() == ScenarioIndex {1});
    CHECK(scene_lp.count_scenario() == 1);
  }

  TEST_CASE("Inactive scene")
  {
    const Scene scene {
        .uid = 3,
        .name = "inactive_scene",
        .active = false,
        .first_scenario = 0,
        .count_scenario = 2,
    };

    std::vector<Scenario> scenarios {
        Scenario {.uid = 1, .active = true},
        Scenario {.uid = 2, .active = true},
    };

    SceneIndex index {3};
    const SceneLP scene_lp(scene, scenarios, index);

    CHECK(scene_lp.index() == index);
    CHECK_FALSE(scene_lp.is_active());
  }

  TEST_CASE("Edge cases")
  {
    SUBCASE("Empty scenario list")
    {
      const Scene scene {
          .uid = 4,
          .name = "empty_scene",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 0,
      };

      const SceneLP scene_lp(scene, std::vector<Scenario> {});

      CHECK(scene_lp.count_scenario() == 0);
      CHECK(scene_lp.first_scenario() == ScenarioIndex {0});
    }
  }
}
