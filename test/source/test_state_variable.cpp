#include <doctest/doctest.h>
#include <gtopt/object_utils.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

// Define a mock element for testing
struct MockElement : public ObjectUtils
{
  static constexpr std::string_view class_name() { return "MockElement"; }
  static Uid uid() { return 123; }
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

  SUBCASE("Key comparison")
  {
    auto key1 = StateVariable::key(element, "col1");
    auto key2 = StateVariable::key(element, "col1");
    auto key3 = StateVariable::key(element, "col2");

    CHECK(key1 == key2);
    CHECK(key1 != key3);
  }
}
