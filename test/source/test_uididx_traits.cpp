#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/uididx_traits.hpp>

namespace gtopt
{
TEST_SUITE("UidMapTraits")
{
  TEST_CASE("Basic functionality")
  {
    using TestTraits = UidMapTraits<int, std::string, int>;

    SUBCASE("Type aliases")
    {
      CHECK(std::is_same_v<TestTraits::value_type, int>);
      CHECK(std::is_same_v<TestTraits::key_type, std::tuple<std::string, int>>);
      CHECK(std::is_same_v<TestTraits::uid_map_t,
                           gtopt::flat_map<std::tuple<std::string, int>, int>>);
    }

    SUBCASE("Map operations")
    {
      TestTraits::uid_map_t map;
      auto key = std::make_tuple("test", 42);
      map.emplace(key, 100);

      CHECK(map.size() == 1);
      CHECK(map.at(key) == 100);
    }
  }
}

TEST_SUITE("ArrowUidTraits")
{
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
      auto result = TestTraits::make_uid_column(nullptr, "");
      CHECK(!result.has_value());
      CHECK(result.error() == "Null table provided");
    }
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

TEST_SUITE("UidToVectorIdx")
{
  TEST_CASE("Scenario-Stage-Block mapping")
  {
    using TestTraits = UidToVectorIdx<ScenarioUid, StageUid, BlockUid>;

    SUBCASE("Type traits")
    {
      CHECK(std::is_same_v<TestTraits::IndexKey,
                           std::tuple<Index, Index, Index>>);
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
}

TEST_SUITE("Edge Cases")
{
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
}

}  // namespace gtopt
