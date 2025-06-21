#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable key method")
{
  SUBCASE("Basic key formation")
  {
    auto key = StateVariable::key("col_name", 123, "class_name");
    CHECK(key.scenario_uid == ScenarioUid{unknown_uid});
    CHECK(key.stage_uid == StageUid{unknown_uid});
    CHECK(key.class_name == "class_name");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "col_name");
  }

  SUBCASE("Key with stage and scenario")
  {
    StageUid stage_uid{42};
    ScenarioUid scenario_uid{100};
    auto key = StateVariable::key("another_col", 456, "Generator", stage_uid, scenario_uid);
    
    CHECK(key.scenario_uid == scenario_uid);
    CHECK(key.stage_uid == stage_uid);
    CHECK(key.class_name == "Generator");
    CHECK(key.uid == 456);
    CHECK(key.col_name == "another_col");
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
