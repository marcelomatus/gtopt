#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <gtopt/uididx_traits.hpp>

namespace gtopt {

TEST_CASE("UidMapTraits basic functionality") {
  using TestTraits = UidMapTraits<int, std::string, int>;
  
  SUBCASE("Type aliases") {
    CHECK(std::is_same_v<TestTraits::value_type, int>);
    CHECK(std::is_same_v<TestTraits::key_type, std::tuple<std::string, int>>);
    CHECK(std::is_same_v<TestTraits::uid_map_t, 
          gtopt::flat_map<std::tuple<std::string, int>, int>>);
  }
  
  SUBCASE("Static members") {
    CHECK(TestTraits::uid_count == 2);
    CHECK(std::is_same_v<TestTraits::uid_type<0>, std::string>);
    CHECK(std::is_same_v<TestTraits::uid_type<1>, int>);
  }
  
  SUBCASE("make_key") {
    auto key = TestTraits::make_key("test", 42);
    CHECK(std::get<0>(key) == "test");
    CHECK(std::get<1>(key) == 42);
  }
}

TEST_CASE("ArrowUidTraits functionality") {
  using TestTraits = ArrowUidTraits<std::string, int>;
  
  SUBCASE("Inheritance") {
    CHECK(std::is_base_of_v<ArrowTraits<Uid>, TestTraits>);
    CHECK(std::is_base_of_v<UidMapTraits<ArrowIndex, std::string, int>, TestTraits>);
  }
  
  // Note: Actual Arrow table tests would require real Arrow data
  // These are compile-time checks only
  SUBCASE("make_uid_column return type") {
    CHECK(std::is_same_v<
      decltype(TestTraits::make_uid_column(nullptr, "")::value_type,
      std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType>>);
  }
}

TEST_CASE("UidToVectorIdx functionality") {
  using TestTraits = UidToVectorIdx<ScenarioUid, StageUid>;
  
  SUBCASE("Type aliases") {
    CHECK(std::is_same_v<TestTraits::IndexKey, std::tuple<Index, Index>>);
    CHECK(std::is_same_v<TestTraits::UidKey, std::tuple<ScenarioUid, StageUid>>);
    CHECK(std::is_same_v<TestTraits::uid_vector_idx_map_t, 
          gtopt::flat_map<std::tuple<ScenarioUid, StageUid>, std::tuple<Index, Index>>>);
  }
  
  // Note: Actual SimulationLP tests would require a real SimulationLP object
}

} // namespace gtopt
