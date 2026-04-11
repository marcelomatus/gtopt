#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("StateVariable::Key functionality")
{
  using namespace gtopt;
  SUBCASE("Basic key creation")
  {
    const auto key = StateVariable::key(
        "TestClass", 123, "test_col", PhaseIndex {1}, StageUid {2});

    CHECK(key.class_name == "TestClass");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "test_col");
    CHECK(key.lp_key.phase_index == PhaseIndex {1});
    CHECK(key.stage_uid == StageUid {2});
    CHECK(key.scenario_uid == make_uid<Scenario>(unknown_uid));
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
  const StateVariable::LPKey lp_key {
      .scene_index = SceneIndex {1},
      .phase_index = PhaseIndex {2},
  };
  StateVariable var {lp_key, ColIndex {3}, 0.0, 1.0, LpContext {}};

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
        {.scene_index = SceneIndex {4}, .phase_index = PhaseIndex {5}},
        ColIndex {10});
    var.add_dependent_variable(
        {.scene_index = SceneIndex {6}, .phase_index = PhaseIndex {7}},
        ColIndex {20});

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
      {.scene_index = SceneIndex {1}, .phase_index = PhaseIndex {2}},
      ColIndex {3},
      0.0,
      1.0,
      LpContext {}};

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

    const auto& dep = var.add_dependent_variable(scenario, stage, ColIndex {6});
    CHECK(dep.scene_index() == SceneIndex {4});
    CHECK(dep.phase_index() == PhaseIndex {5});
    CHECK(dep.col() == 6);
  }
}

TEST_CASE("StateVariable carries LpContext")
{
  using namespace gtopt;

  const auto ctx = make_stage_context(make_uid<Scenario>(0), StageUid {3});
  const StateVariable var {
      {.scene_index = first_scene_index(), .phase_index = PhaseIndex {1}},
      ColIndex {5},
      10.0,
      100.0,
      ctx,
  };

  CHECK(var.col() == 5);
  CHECK(var.scost() == doctest::Approx(10.0));
  CHECK(var.var_scale() == doctest::Approx(100.0));

  REQUIRE(std::holds_alternative<StageContext>(var.context()));
  const auto& stg = std::get<StageContext>(var.context());
  CHECK(std::get<0>(stg) == make_uid<Scenario>(0));
  CHECK(std::get<1>(stg) == StageUid {3});
}

TEST_CASE("StateVariable with BlockContext")
{
  using namespace gtopt;

  const auto ctx = make_block_context(
      make_uid<Scenario>(1), StageUid {2}, make_uid<Block>(4));
  const StateVariable var {
      {.scene_index = first_scene_index(), .phase_index = first_phase_index()},
      ColIndex {8},
      0.0,
      1.0,
      ctx,
  };

  REQUIRE(std::holds_alternative<BlockContext>(var.context()));
  const auto& blk = std::get<BlockContext>(var.context());
  CHECK(std::get<0>(blk) == make_uid<Scenario>(1));
  CHECK(std::get<1>(blk) == StageUid {2});
  CHECK(std::get<2>(blk) == make_uid<Block>(4));
}

TEST_CASE("StateVariable default context is monostate")
{
  using namespace gtopt;

  const StateVariable var {
      {.scene_index = first_scene_index(), .phase_index = first_phase_index()},
      ColIndex {0},
      0.0,
      1.0,
      LpContext {},
  };

  CHECK(std::holds_alternative<std::monostate>(var.context()));
}
