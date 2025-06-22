#include <doctest/doctest.h>
#include <gtopt/object_utils.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/linear_problem.hpp>

using namespace gtopt;

// Define a mock element for testing
struct MockElement : public ObjectUtils
{
  static constexpr std::string_view class_name() { return "MockElement"; }
  [[nodiscard]] static constexpr Uid uid() noexcept { return 123; }
};

TEST_CASE("StateVariable key method")
{
  const MockElement element;

  SUBCASE("Basic key formation with element")
  {
    auto key = StateVariable::key(element, "col_name");
    CHECK(key.scenario_uid == ScenarioUid {unknown_uid});
    CHECK(key.stage_uid == StageUid {unknown_uid});
    CHECK(key.class_name == "MockElement");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "col_name");
  }

  SUBCASE("Key formation with element and UIDs")
  {
    StageUid stage_uid {42};
    ScenarioUid scenario_uid {100};
    auto key =
        StateVariable::key(element, "another_col", stage_uid, scenario_uid);

    CHECK(key.scenario_uid == scenario_uid);
    CHECK(key.stage_uid == stage_uid);
    CHECK(key.class_name == "MockElement");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "another_col");
  }

  SUBCASE("Key formation with direct parameters")
  {
    auto key = StateVariable::key("DirectClass", 456, "direct_col");
    CHECK(key.scenario_uid == ScenarioUid {unknown_uid});
    CHECK(key.stage_uid == StageUid {unknown_uid});
    CHECK(key.class_name == "DirectClass");
    CHECK(key.uid == 456);
    CHECK(key.col_name == "direct_col");
  }

  SUBCASE("Key comparison")
  {
    auto key1 = StateVariable::key(element, "col1");
    auto key2 = StateVariable::key(element, "col1");
    auto key3 = StateVariable::key(element, "col2");
    auto key4 = StateVariable::key("OtherClass", 123, "col1");

    CHECK(key1 == key2);
    CHECK(key1 != key3);
    CHECK(key1 != key4);
  }
}

TEST_CASE("StateVariable construction and basic properties")
{
  SUBCASE("Default construction")
  {
    const StateVariable var(SceneIndex{1}, PhaseIndex{2}, Index{3});
    
    CHECK(var.col() == 3);
    CHECK(var.scene_index() == SceneIndex{1});
    CHECK(var.phase_index() == PhaseIndex{2});
    CHECK(var.dependent_variables().empty());
  }

  SUBCASE("With dependent variables")
  {
    StateVariable var(SceneIndex{1}, PhaseIndex{2}, Index{3});

    var.add_dependent_variable(SceneIndex{4}, PhaseIndex{5}, Index{10});
    var.add_dependent_variable(SceneIndex{6}, PhaseIndex{7}, Index{20});

    const auto& deps = var.dependent_variables();
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].scene_index == SceneIndex{4});
    CHECK(deps[0].phase_index == PhaseIndex{5});
    CHECK(deps[0].col == 10);
    CHECK(deps[1].scene_index == SceneIndex{6});
    CHECK(deps[1].phase_index == PhaseIndex{7});
    CHECK(deps[1].col == 20);
  }
}

TEST_CASE("StateVariable dependent variables")
{
  StateVariable var(SceneIndex{1}, PhaseIndex{2}, Index{3});
  const LinearProblem lp("test");

  SUBCASE("Adding single dependent variable")
  {
    auto& dep = var.add_dependent_variable(SceneIndex{4}, PhaseIndex{5}, Index{6});
    CHECK(dep.scene_index == SceneIndex{4});
    CHECK(dep.phase_index == PhaseIndex{5});
    CHECK(dep.col == 6);
  }

  SUBCASE("Adding multiple dependent variables")
  {
    var.add_dependent_variable(SceneIndex{4}, PhaseIndex{5}, Index{6});
    var.add_dependent_variable(SceneIndex{7}, PhaseIndex{8}, Index{9});
    
    const auto& deps = var.dependent_variables();
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].col == 6);
    CHECK(deps[1].col == 9);
  }
}
