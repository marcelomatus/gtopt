#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable key method")
{
  // Use a real StageLP object instead of mock
  StageLP stage(Stage{42}, {}, 0.0, StageIndex{42}, PhaseIndex{1});

  SUBCASE("Basic key formation")
  {
    auto key = StateVariable::key(stage, "var", 1, 2.0);
    CHECK(std::get<0>(key) == "var_1_2.0");
    CHECK(std::get<1>(key) == stage.uid());
  }

  SUBCASE("Single component")
  {
    auto key = StateVariable::key(stage, "single");
    CHECK(std::get<0>(key) == "single");
  }

  SUBCASE("Empty components")
  {
    auto key = StateVariable::key(stage);
    CHECK(std::get<0>(key) == "");
  }

  SUBCASE("Key comparison")
  {
    auto key1 = StateVariable::key(stage, "same", "key");
    auto key2 = StateVariable::key(stage, "same", "key");
    auto key3 = StateVariable::key(stage, "different", "key");

    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }
}
