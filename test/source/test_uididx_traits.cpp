#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/uididx_traits.hpp>

namespace gtopt
{
TEST_CASE("Basic functionality")
{
  using TestTraits = UidMapTraits<int, std::string, int>;

  // SUBCASE("Type aliases")
  {
    CHECK(std::is_same_v<TestTraits::value_type, int>);
    CHECK(std::is_same_v<TestTraits::key_type, std::tuple<std::string, int>>);
    CHECK(std::is_same_v<TestTraits::uid_map_t,
                         gtopt::flat_map<std::tuple<std::string, int>, int>>);
  }

  // SUBCASE("Map operations")
  {
    TestTraits::uid_map_t map;
    auto key = std::make_tuple("test", 42);
    map.emplace(key, 100);

    CHECK(map.size() == 1);
    CHECK(map.at(key) == 100);
  }
}

TEST_CASE("UidColumn success cases")
{
  // Would need mock Arrow table setup to test successfully
  // This is currently missing from test coverage
}

TEST_CASE("UidToArrowIdx specializations")
{
  SUBCASE("Scenario-Stage-Block mapping errors")
  {
    // Test error cases for make_arrow_uids_idx
    // Currently missing from test coverage
  }

  SUBCASE("Stage-Block mapping errors")
  {
    // Test error cases for make_arrow_uids_idx
    // Currently missing from test coverage
  }
}

TEST_CASE("Inheritance and type traits")
{
  using TestTraits = ArrowUidTraits<std::string, int>;

  SUBCASE("Inheritance")
  {
    CHECK(std::is_base_of_v<ArrowTraits<Uid>, TestTraits>);
    CHECK(std::is_base_of_v<UidMapTraits<ArrowIndex, std::string, int>,
                            TestTraits>);
  }

  SUBCASE("make_uid_column error cases")
  {
    auto result = TestTraits::make_uid_column(nullptr, "test");
    CHECK(!result.has_value());
    CHECK(result.error() == "Null table, no column for name 'test'");
  }
}

TEST_SUITE("UidToArrowIdx")
{
  TEST_CASE("Type traits")
  {
    using TestTraits = UidToArrowIdx<ScenarioUid, StageUid, BlockUid>;

    CHECK(std::is_base_of_v<ArrowUidTraits<ScenarioUid, StageUid, BlockUid>,
                            TestTraits>);
  }
}

TEST_CASE("Scenario-Stage-Block mapping")
{
  using TestTraits = UidToVectorIdx<ScenarioUid, StageUid, BlockUid>;

  SUBCASE("Type traits")
  {
    CHECK(
        std::is_same_v<TestTraits::IndexKey, std::tuple<Index, Index, Index>>);
    CHECK(std::is_same_v<TestTraits::UidKey,
                         std::tuple<ScenarioUid, StageUid, BlockUid>>);
  }

  SUBCASE("Empty simulation")
  {
    const Simulation sim;
    const OptionsLP options;
    const SimulationLP sim_lp(sim, options);

    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->empty());
  }

  SUBCASE("Multiple entries")
  {
    Simulation sim;
    sim.scenario_array.emplace_back(Scenario {.uid = ScenarioUid {1}});
    sim.scenario_array.emplace_back(Scenario {.uid = ScenarioUid {2}});

    sim.stage_array.emplace_back(
        Stage {.uid = StageUid {1}, .first_block = 0, .count_block = 1});
    sim.stage_array.emplace_back(
        Stage {.uid = StageUid {2}, .first_block = 1, .count_block = 2});

    sim.block_array.emplace_back(Block {.uid = BlockUid {1}});
    sim.block_array.emplace_back(Block {.uid = BlockUid {2}});
    sim.block_array.emplace_back(Block {.uid = BlockUid {3}});
    // Need to add blocks to test full mapping

    const OptionsLP options;
    const SimulationLP sim_lp(sim, options);

    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->size() == 6);
    CHECK(result->at({ScenarioUid {1}, StageUid {1}, BlockUid {1}})
          == std::tuple {0, 0, 0});

    auto tuid = std::tuple {ScenarioUid {1}, StageUid {2}, BlockUid {3}};
    auto tidx = std::tuple {0, 1, 1};
    CHECK(as_string(result->at(tuid)) == as_string(tidx));
    CHECK(result->at(tuid) == tidx);
  }
}

TEST_CASE("Scenario-Stage mapping")
{
  using TestTraits = UidToVectorIdx<ScenarioUid, StageUid>;

  SUBCASE("Basic mapping")
  {
    Simulation sim;
    sim.scenario_array.emplace_back(Scenario {.uid = ScenarioUid {1}});
    sim.stage_array.emplace_back(Stage {.uid = StageUid {1}});

    const OptionsLP options;
    const SimulationLP sim_lp(sim, options);

    // Remove constexpr requirement since flat_map isn't constexpr
    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->size() == 1);
    CHECK(result->at(std::make_tuple(ScenarioUid {1}, StageUid {1}))
          == std::make_tuple(0, 0));
  }
}

TEST_CASE("Stage mapping")
{
  using TestTraits = UidToVectorIdx<StageUid>;

  SUBCASE("Duplicate UID detection")
  {
    Simulation sim;
    sim.stage_array.emplace_back(Stage {.uid = StageUid {1}});
    sim.stage_array.emplace_back(Stage {.uid = StageUid {1}});  // Duplicate

    const OptionsLP options;
    const SimulationLP sim_lp(sim, options);

    // Remove constexpr requirement since flat_map isn't constexpr
    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->size() == 1);  // Only one unique UID should be stored
  }
}

TEST_CASE("Empty UID tuple")
{
  using TestTraits = UidMapTraits<int>;

  CHECK(std::is_same_v<TestTraits::key_type, std::tuple<>>);
}

TEST_CASE("Single UID type")
{
  using TestTraits = UidToVectorIdx<StageUid>;

  Simulation sim;
  sim.stage_array.emplace_back(Stage {.uid = StageUid {42}});

  const OptionsLP options;
  const SimulationLP sim_lp(sim, options);

  // Remove constexpr requirement since flat_map isn't constexpr
  auto result = TestTraits::make_vector_uids_idx(sim_lp);
  CHECK(result->at(std::make_tuple(StageUid {42})) == std::make_tuple(0));
}

}  // namespace gtopt
