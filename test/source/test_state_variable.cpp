#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>
#include <gtopt/stage.hpp>  // For Stage::class_name

using namespace gtopt;

// Define a mock element for testing
struct MockElement {
    static constexpr std::string_view class_name = "MockElement";
    Uid uid() const { return 123; }
};

TEST_CASE("StateVariable key method")
{
  MockElement element;
  
  SUBCASE("Basic key formation with element")
  {
    auto key = StateVariable::key(element, "col_name");
    CHECK(key.scenario_uid == ScenarioUid{unknown_uid});
    CHECK(key.stage_uid == StageUid{unknown_uid});
    CHECK(key.class_name == "MockElement");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "col_name");
  }

  SUBCASE("Key formation with element and UIDs")
  {
    StageUid stage_uid{42};
    ScenarioUid scenario_uid{100};
    auto key = StateVariable::key(element, "another_col", stage_uid, scenario_uid);
    
    CHECK(key.scenario_uid == scenario_uid);
    CHECK(key.stage_uid == stage_uid);
    CHECK(key.class_name == "MockElement");
    CHECK(key.uid == 123);
    CHECK(key.col_name == "another_col");
  }

  SUBCASE("Key comparison")
  {
    auto key1 = StateVariable::key(element, "col1");
    auto key2 = StateVariable::key(element, "col1");
    auto key3 = StateVariable::key(element, "col2");
    
    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }

  SUBCASE("Key with real stage element")
  {
    gtopt::Stage stage;
    stage.uid = 456;
    auto key = StateVariable::key(stage, "stage_col");
    
    CHECK(key.class_name == "stage");
    CHECK(key.uid == 456);
    CHECK(key.col_name == "stage_col");
  }
}
