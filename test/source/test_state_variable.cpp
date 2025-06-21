#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable key method")
{
  SUBCASE("Basic key formation")
  {
    auto key = StateVariable::key("col_name", 123, "class_name");
    CHECK(std::get<0>(key) == ScenarioUid{unknown_uid});
    CHECK(std::get<1>(key) == StageUid{unknown_uid});
    CHECK(std::get<2>(key) == "class_name");
    CHECK(std::get<3>(key) == 123);
    CHECK(std::get<4>(key) == "col_name");
  }

  SUBCASE("Key with stage and scenario")
  {
    StageUid stage_uid{42};
    ScenarioUid scenario_uid{100};
    auto key = StateVariable::key("another_col", 456, "Generator", stage_uid, scenario_uid);
    
    CHECK(std::get<0>(key) == scenario_uid);
    CHECK(std::get<1>(key) == stage_uid);
    CHECK(std::get<2>(key) == "Generator");
    CHECK(std::get<3>(key) == 456);
    CHECK(std::get<4>(key) == "another_col");
  }

  SUBCASE("Key comparison")
  {
    auto key1 = StateVariable::key("col1", 1, "Class1");
    auto key2 = StateVariable::key("col1", 1, "Class1");
    auto key3 = StateVariable::key("col2", 2, "Class2");
    
    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }
}
