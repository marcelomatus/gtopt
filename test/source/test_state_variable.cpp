#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("StateVariable::Key functionality")
{
  using namespace gtopt;
  SUBCASE("Basic key creation")
  {
    // Post-2026-04-23: `StateVariable::key()` requires concrete
    // scene/scenario/stage uids — the unknown-uid defaults were
    // removed because `unknown_uid = -1` silently embeds the
    // rejected `-` char into downstream LP labels (CoinLpIO /
    // CBC failure mode).
    const auto key = StateVariable::key("TestClass",
                                        123,
                                        "test_col",
                                        PhaseIndex {1},
                                        make_uid<Stage>(2),
                                        SceneIndex {0},
                                        make_uid<Scenario>(0));

    CHECK(key.class_name == "TestClass");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "test_col");
    CHECK(key.lp_key.phase_index == PhaseIndex {1});
    CHECK(key.stage_uid == make_uid<Stage>(2));
    CHECK(key.scenario_uid == make_uid<Scenario>(0));
    CHECK(key.lp_key.scene_index == SceneIndex {0});
  }

  SUBCASE("Key comparison")
  {
    const auto key1 = StateVariable::key("ClassA",
                                         1,
                                         "col1",
                                         PhaseIndex {1},
                                         make_uid<Stage>(1),
                                         SceneIndex {0},
                                         make_uid<Scenario>(0));
    const auto key2 = StateVariable::key("ClassA",
                                         1,
                                         "col1",
                                         PhaseIndex {1},
                                         make_uid<Stage>(1),
                                         SceneIndex {0},
                                         make_uid<Scenario>(0));
    const auto key3 = StateVariable::key("ClassB",
                                         1,
                                         "col1",
                                         PhaseIndex {1},
                                         make_uid<Stage>(1),
                                         SceneIndex {0},
                                         make_uid<Scenario>(0));

    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }

  SUBCASE("Factory throws on unknown uids")
  {
    // uid must be concrete
    CHECK_THROWS_AS(
        [[maybe_unused]] auto k1 = StateVariable::key("C",
                                                      unknown_uid,
                                                      "col",
                                                      PhaseIndex {0},
                                                      make_uid<Stage>(0),
                                                      SceneIndex {0},
                                                      make_uid<Scenario>(0)),
        std::invalid_argument);

    // scenario_uid must be concrete
    CHECK_THROWS_AS([[maybe_unused]] auto k2 =
                        StateVariable::key("C",
                                           1,
                                           "col",
                                           PhaseIndex {0},
                                           make_uid<Stage>(0),
                                           SceneIndex {0},
                                           make_uid<Scenario>(unknown_uid)),
                    std::invalid_argument);

    // stage_uid must be concrete
    CHECK_THROWS_AS([[maybe_unused]] auto k3 =
                        StateVariable::key("C",
                                           1,
                                           "col",
                                           PhaseIndex {0},
                                           make_uid<Stage>(unknown_uid),
                                           SceneIndex {0},
                                           make_uid<Scenario>(0)),
                    std::invalid_argument);
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

TEST_CASE("StateVariable physical accessors")
{
  // `col_sol_physical()` returns `LP × var_scale`; `reduced_cost_physical`
  // returns `LP × scale_obj / var_scale`, mirroring the LinearInterface
  // `get_col_cost()` physical convention.  These are the accessors the
  // phase-B physical Benders cut migration will use to replace the
  // today's raw LP idioms at the cut-builder call sites.
  const StateVariable::LPKey lp_key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };
  StateVariable var {lp_key,
                     ColIndex {0},
                     /*scost=*/0.0,
                     /*var_scale=*/8.0,
                     LpContext {}};

  SUBCASE("col_sol_physical scales by var_scale")
  {
    var.set_col_sol(5.0);
    CHECK(var.col_sol() == doctest::Approx(5.0));  // raw LP unchanged
    CHECK(var.col_sol_physical() == doctest::Approx(40.0));  // 5 × 8
  }

  SUBCASE("reduced_cost_physical = LP × scale_obj / var_scale")
  {
    var.set_reduced_cost(2.0);
    CHECK(var.reduced_cost() == doctest::Approx(2.0));  // raw LP unchanged
    // physical = 2 × 1000 / 8 = 250
    CHECK(var.reduced_cost_physical(1000.0) == doctest::Approx(250.0));
  }

  SUBCASE("var_scale = 1 means physical equals LP")
  {
    StateVariable unit_var {lp_key, ColIndex {1}, 0.0, 1.0, LpContext {}};
    unit_var.set_col_sol(3.14);
    unit_var.set_reduced_cost(-1.5);
    CHECK(unit_var.col_sol_physical() == doctest::Approx(3.14));
    CHECK(unit_var.reduced_cost_physical(1.0) == doctest::Approx(-1.5));
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

  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(3));
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
  CHECK(std::get<1>(stg) == make_uid<Stage>(3));
}

TEST_CASE("StateVariable with BlockContext")
{
  using namespace gtopt;

  const auto ctx = make_block_context(
      make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(4));
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
  CHECK(std::get<1>(blk) == make_uid<Stage>(2));
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
