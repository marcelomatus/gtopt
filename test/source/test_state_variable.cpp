#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

// Define a mock element for testing
struct MockElement : public ObjectUtils {
  static constexpr std::string_view class_name = "MockElement";
  [[nodiscard]] static Uid uid() { return 123; }
  
  auto sv_key(std::string_view col_name, 
              StageUid stage_uid = StageUid{unknown_uid},
              ScenarioUid scenario_uid = ScenarioUid{unknown_uid}) const {
    return StateVariable::key(*this, col_name, stage_uid, scenario_uid);
  }
};

static_assert(requires {
  { MockElement::class_name } -> std::same_as<const std::string_view&>;
  { MockElement::uid() } -> std::same_as<Uid>;
}, "MockElement must satisfy element interface requirements");

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
}
