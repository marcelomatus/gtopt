#include <doctest/doctest.h>
#include <gtopt/stage_lp.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable key method")
{
  // Create a Stage with UID 42
  Stage stage;
  stage.uid = 42;

  // Create StageLP with the Stage object
  const StageLP stage_lp(stage, {}, 0.0, StageIndex {42}, PhaseIndex {1});

  SUBCASE("Basic key formation")
  {
    auto key = StateVariable::key(stage_lp, "var", 1, "2.0");
    CHECK(std::get<0>(key) == "var_1_2.0");
    CHECK(std::get<1>(key) == stage_lp.uid());
  }

  SUBCASE("Single component")
  {
    auto key = StateVariable::key(stage_lp, "single");
    CHECK(std::get<0>(key) == "single");
    CHECK(std::get<1>(key) == stage_lp.uid());
  }

  SUBCASE("Empty components")
  {
    auto key = StateVariable::key(stage_lp);
    CHECK(std::get<0>(key) == "");
    CHECK(std::get<1>(key) == stage_lp.uid());
  }

  SUBCASE("Key comparison")
  {
    auto key1 = StateVariable::key(stage_lp, "same", "key");
    auto key2 = StateVariable::key(stage_lp, "same", "key");
    auto key3 = StateVariable::key(stage_lp, "different", "key");

    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }
}
