#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable key method")
{
  SUBCASE("Basic key formation")
  {
    auto key = StateVariable::key(
        "TestClass", 123, "col_name", PhaseIndex {1}, StageUid {2});
    CHECK(key.scenario_uid == ScenarioUid {unknown_uid});
    CHECK(key.stage_uid == StageUid {2});
    CHECK(key.class_name == "TestClass");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "col_name");
  }

  SUBCASE("Key formation with all parameters")
  {
    auto key = StateVariable::key("TestClass",
                                  123,
                                  "col_name",
                                  PhaseIndex {1},
                                  StageUid {2},
                                  SceneIndex {3},
                                  ScenarioUid {4});
    CHECK(key.scenario_uid == ScenarioUid {4});
    CHECK(key.stage_uid == StageUid {2});
    CHECK(key.class_name == "TestClass");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "col_name");
    CHECK(key.lp_key.scene_index == SceneIndex {3});
    CHECK(key.lp_key.phase_index == PhaseIndex {1});
  }

  SUBCASE("Key comparison")
  {
    auto key1 =
        StateVariable::key("Class1", 1, "col1", PhaseIndex {1}, StageUid {1});
    auto key2 =
        StateVariable::key("Class1", 1, "col1", PhaseIndex {1}, StageUid {1});
    auto key3 =
        StateVariable::key("Class2", 1, "col1", PhaseIndex {1}, StageUid {1});
    auto key4 =
        StateVariable::key("Class1", 2, "col1", PhaseIndex {1}, StageUid {1});

    CHECK(key1 == key2);
    CHECK(key1 != key3);
    CHECK(key1 != key4);
  }
}

TEST_CASE("StateVariable construction and basic properties")
{
  SUBCASE("Basic construction")
  {
    StateVariable var(
        {.scene_index = SceneIndex {1}, .phase_index = PhaseIndex {2}}, 3);

    CHECK(var.col() == 3);
    CHECK(var.scene_index() == SceneIndex {1});
    CHECK(var.phase_index() == PhaseIndex {2});
    CHECK(var.dependent_variables().empty());
  }

  SUBCASE("With dependent variables")
  {
    StateVariable var(
        {.scene_index = SceneIndex {1}, .phase_index = PhaseIndex {2}}, 3);

    var.add_dependent_variable(SceneIndex {4}, PhaseIndex {5}, 10);
    var.add_dependent_variable(SceneIndex {6}, PhaseIndex {7}, 20);

    const auto deps = var.dependent_variables();
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].lp_key.scene_index == SceneIndex {4});
    CHECK(deps[0].lp_key.phase_index == PhaseIndex {5});
    CHECK(deps[0].col == 10);
    CHECK(deps[1].lp_key.scene_index == SceneIndex {6});
    CHECK(deps[1].lp_key.phase_index == PhaseIndex {7});
    CHECK(deps[1].col == 20);
  }
}

TEST_CASE("StateVariable dependent variables")
{
  StateVariable var(
      {.scene_index = SceneIndex {1}, .phase_index = PhaseIndex {2}}, 3);

  SUBCASE("Adding single dependent variable")
  {
    auto& dep = var.add_dependent_variable(SceneIndex {4}, PhaseIndex {5}, 6);
    CHECK(dep.lp_key.scene_index == SceneIndex {4});
    CHECK(dep.lp_key.phase_index == PhaseIndex {5});
    CHECK(dep.col == 6);
  }

  SUBCASE("Adding multiple dependent variables")
  {
    var.add_dependent_variable(SceneIndex {4}, PhaseIndex {5}, 6);
    var.add_dependent_variable(SceneIndex {7}, PhaseIndex {8}, 9);

    const auto deps = var.dependent_variables();
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].col == 6);
    CHECK(deps[1].col == 9);
  }
}
