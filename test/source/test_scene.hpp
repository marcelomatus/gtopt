// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/scene.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Scene construction and default values")
{
  const Scene scene;

  CHECK(scene.uid == Uid {unknown_uid});
  CHECK_FALSE(scene.name.has_value());
  CHECK_FALSE(scene.active.has_value());
  CHECK(scene.first_scenario == 0);
  CHECK(scene.count_scenario == std::dynamic_extent);
  CHECK(scene.class_name == "scene");
}

TEST_CASE("Scene is_active default behaviour")  // NOLINT
{
  SUBCASE("default is active when unset")
  {
    const Scene scene;
    CHECK(scene.is_active() == true);
  }

  SUBCASE("explicitly active")
  {
    Scene scene;
    scene.active = true;
    CHECK(scene.is_active() == true);
  }

  SUBCASE("explicitly inactive")
  {
    Scene scene;
    scene.active = false;
    CHECK(scene.is_active() == false);
  }
}

TEST_CASE("Scene attribute assignment")
{
  Scene scene;

  scene.uid = 1;
  scene.name = "wet_scenarios";
  scene.first_scenario = 3;
  scene.count_scenario = 5;

  CHECK(scene.uid == 1);
  REQUIRE(scene.name.has_value());
  CHECK(scene.name.value() == "wet_scenarios");
  CHECK(scene.first_scenario == 3);
  CHECK(scene.count_scenario == 5);
}

TEST_CASE("Scene designated initializer construction")
{
  const Scene scene {
      .uid = Uid {2},
      .name = "all_scenarios",
      .first_scenario = 0,
      .count_scenario = 10,
  };

  CHECK(scene.uid == Uid {2});
  REQUIRE(scene.name.has_value());
  CHECK(scene.name.value() == "all_scenarios");
  CHECK(scene.first_scenario == 0);
  CHECK(scene.count_scenario == 10);
}

TEST_CASE("Scene with dynamic_extent covers all scenarios")
{
  const Scene scene {
      .uid = Uid {1},
      .first_scenario = 2,
      .count_scenario = std::dynamic_extent,
  };

  CHECK(scene.first_scenario == 2);
  CHECK(scene.count_scenario == std::dynamic_extent);
}

TEST_CASE("SceneUid, SceneIndex and OptSceneIndex strong types")
{
  const SceneUid suid {3};
  const SceneIndex sidx {1};
  const OptSceneIndex opt_sidx {SceneIndex {5}};
  const OptSceneIndex empty_sidx {};

  CHECK(suid == SceneUid {3});
  CHECK(sidx == SceneIndex {1});
  REQUIRE(opt_sidx.has_value());
  CHECK(opt_sidx.value() == SceneIndex {5});
  CHECK_FALSE(empty_sidx.has_value());
}

TEST_CASE("Scene array construction")  // NOLINT
{
  const Array<Scene> scenes {
      {
          .uid = Uid {1},
          .first_scenario = 0,
          .count_scenario = 3,
      },
      {
          .uid = Uid {2},
          .first_scenario = 3,
          .count_scenario = 4,
      },
  };

  CHECK(scenes.size() == 2);
  CHECK(scenes[0].first_scenario == 0);
  CHECK(scenes[0].count_scenario == 3);
  CHECK(scenes[1].first_scenario == 3);
  CHECK(scenes[1].count_scenario == 4);
}
