#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable::Key functionality")
{
  SUBCASE("Basic key creation")
  {
    const auto key = StateVariable::key(
        "TestClass", 123, "test_col", PhaseIndex {1}, StageUid {2});

    CHECK(key.class_name == "TestClass");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "test_col");
    CHECK(key.lp_key.phase_index == PhaseIndex {1});
    CHECK(key.stage_uid == StageUid {2});
    CHECK(key.scenario_uid == ScenarioUid {unknown_uid});
    CHECK(key.lp_key.scene_index == SceneIndex {unknown_index});
  }

  SUBCASE("Key comparison")
  {
    const auto key1 =
        StateVariable::key("ClassA", 1, "col1", PhaseIndex {1}, StageUid {1});
    const auto key2 =
        StateVariable::key("ClassA", 1, "col1", PhaseIndex {1}, StageUid {1});
    const auto key3 =
        StateVariable::key("ClassB", 1, "col1", PhaseIndex {1}, StageUid {1});

    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }
}

TEST_CASE("StateVariable core functionality")
{
  const StateVariable::LPKey lp_key {.scene_index = SceneIndex {1},
                                     .phase_index = PhaseIndex {2}};
  StateVariable var {lp_key, 3};

  SUBCASE("Basic properties")
  {
    CHECK(var.col() == 3);
    CHECK(var.scene_index() == SceneIndex {1});
    CHECK(var.phase_index() == PhaseIndex {2});
    CHECK(var.dependent_variables().empty());
  }

  SUBCASE("Dependent variables")
  {
    var.add_dependent_variable(
        {.scene_index = SceneIndex {4}, .phase_index = PhaseIndex {5}}, 10);
    var.add_dependent_variable(
        {.scene_index = SceneIndex {6}, .phase_index = PhaseIndex {7}}, 20);

    const auto deps = var.dependent_variables();
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].scene_index() == SceneIndex {4});
    CHECK(deps[0].phase_index() == PhaseIndex {5});
    CHECK(deps[0].col() == 10);
    CHECK(deps[1].scene_index() == SceneIndex {6});
    CHECK(deps[1].phase_index() == PhaseIndex {7});
    CHECK(deps[1].col() == 20);
  }
}

TEST_CASE("StateVariable dependent variable templates")
{
  StateVariable var {
      {.scene_index = SceneIndex {1}, .phase_index = PhaseIndex {2}}, 3};

  SUBCASE("Template add_dependent_variable")
  {
    struct TestStageLP
    {
      static PhaseIndex phase_index() { return PhaseIndex {5}; }
    };
    struct TestScenarioLP
    {
      static SceneIndex scene_index() { return SceneIndex {4}; }
    };

    const TestScenarioLP scenario;
    const TestStageLP stage;

    const auto& dep = var.add_dependent_variable(scenario, stage, 6);
    CHECK(dep.scene_index() == SceneIndex {4});
    CHECK(dep.phase_index() == PhaseIndex {5});
    CHECK(dep.col() == 6);
  }
}
