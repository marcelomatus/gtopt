// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/element_index.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("BlockLP default construction")
{
  using namespace gtopt;
  constexpr BlockLP block;

  SUBCASE("Default constructed BlockLP has unknown index")
  {
    CHECK(block.index() == BlockIndex {unknown_index});
  }

  SUBCASE("Default constructed BlockLP has default Block")
  {
    CHECK(block.uid() == make_uid<Block>(unknown_uid));
    CHECK(block.duration() == 0);
  }
}

TEST_CASE("BlockLP construction with parameters")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};
  constexpr BlockLP block(test_block, test_index);

  SUBCASE("Constructor properly initializes members")
  {
    CHECK(block.uid() == make_uid<Block>(test_block.uid));
    CHECK(block.duration() == test_block.duration);
    CHECK(block.index() == test_index);
  }

  SUBCASE("Constructor with default index")
  {
    constexpr BlockLP default_index_block(test_block);
    CHECK(default_index_block.index() == BlockIndex {unknown_index});
  }
}

TEST_CASE("BlockLP move semantics")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  SUBCASE("Move construction")
  {
    BlockLP original(test_block, test_index);
    const BlockLP moved(std::move(original));
    CHECK(moved.uid() == make_uid<Block>(test_block.uid));
    CHECK(moved.index() == test_index);
  }

  SUBCASE("Move assignment")
  {
    BlockLP original(test_block, test_index);
    BlockLP moved;
    moved = std::move(original);
    CHECK(moved.uid() == make_uid<Block>(test_block.uid));
    CHECK(moved.index() == test_index);
  }
}

TEST_CASE("BlockLP copy semantics")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  const BlockLP original(test_block, test_index);

  SUBCASE("Copy construction")
  {
    const BlockLP copy(  // NOLINT(performance-unnecessary-copy-initialization)
        original);
    CHECK(copy.uid() == original.uid());
    CHECK(copy.index() == original.index());
  }

  SUBCASE("Copy assignment")
  {
    BlockLP copy;
    copy = original;
    CHECK(copy.uid() == original.uid());
    CHECK(copy.index() == original.index());
  }
}

TEST_CASE("BlockLP constexpr usage")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  constexpr BlockLP block(test_block, test_index);

  SUBCASE("Constexpr construction")
  {
    static_assert(block.uid() == make_uid<Block>(test_block.uid));
    static_assert(block.index() == test_index);
  }

  SUBCASE("Constexpr methods")
  {
    constexpr auto uid = block.uid();
    constexpr auto idx = block.index();
    CHECK(uid == make_uid<Block>(test_block.uid));
    CHECK(idx == test_index);
  }
}

TEST_CASE("BlockLP noexcept guarantees")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  SUBCASE("Methods are noexcept")
  {
    const BlockLP block(test_block, test_index);
    CHECK(noexcept(block.uid()));
    CHECK(noexcept(block.duration()));
    CHECK(noexcept(block.index()));
  }

  SUBCASE("Special members are noexcept")
  {
    CHECK(std::is_nothrow_move_constructible_v<BlockLP>);
    CHECK(std::is_nothrow_move_assignable_v<BlockLP>);
    CHECK(std::is_nothrow_destructible_v<BlockLP>);
  }
}

TEST_SUITE("Scenario")
{
  TEST_CASE("is_active")
  {
    SUBCASE("default constructed is active")
    {
      const Scenario scenario;
      CHECK(scenario.is_active());
    }

    SUBCASE("explicitly active")
    {
      const Scenario scenario {.active = true};
      CHECK(scenario.is_active());
    }

    SUBCASE("explicitly inactive")
    {
      const Scenario scenario {.active = false};
      CHECK_FALSE(scenario.is_active());
    }

    SUBCASE("with other fields set")
    {
      const Scenario scenario {
          .uid = Uid {123},
          .name = "Test Scenario",
          .active = false,
          .probability_factor = 0.5,
      };
      CHECK_FALSE(scenario.is_active());
    }
  }
}

TEST_SUITE("ScenarioLP")
{
  TEST_CASE("ScenarioLP is_active")
  {
    SUBCASE("default constructed is active")
    {
      const ScenarioLP scenario_lp;
      CHECK(scenario_lp.is_active());
    }

    SUBCASE("active scenario")
    {
      Scenario scenario {.active = true};
      const ScenarioLP scenario_lp(std::move(scenario));
      CHECK(scenario_lp.is_active());
    }

    SUBCASE("inactive scenario")
    {
      Scenario scenario {.active = false};
      const ScenarioLP scenario_lp(std::move(scenario));
      CHECK_FALSE(scenario_lp.is_active());
    }

    SUBCASE("with index and scene_index")
    {
      Scenario scenario {.active = false};
      const ScenarioLP scenario_lp(
          std::move(scenario), ScenarioIndex {1}, SceneIndex {2});
      CHECK_FALSE(scenario_lp.is_active());
    }
  }

  TEST_CASE("other methods")
  {
    Scenario scenario {
        .uid = Uid {123},
        .name = "Test Scenario",
        .active = true,
        .probability_factor = 0.75,
    };
    const ScenarioLP scenario_lp(
        std::move(scenario), ScenarioIndex {1}, SceneIndex {2});

    SUBCASE("uid")
    {
      CHECK(scenario_lp.uid() == make_uid<Scenario>(123));
    }

    SUBCASE("probability_factor")
    {
      CHECK(scenario_lp.probability_factor() == 0.75);
    }

    SUBCASE("index")
    {
      CHECK(scenario_lp.index() == ScenarioIndex {1});
    }

    SUBCASE("scene_index")
    {
      CHECK(scenario_lp.scene_index() == SceneIndex {2});
    }

    SUBCASE("is_first")
    {
      CHECK_FALSE(scenario_lp.is_first());
      const ScenarioLP first_scenario(
          Scenario(), first_scenario_index(), first_scene_index());
      CHECK(first_scenario.is_first());
    }
  }
}

TEST_SUITE("SceneLP")
{
  TEST_CASE("Default construction")
  {
    const SceneLP scene;
    CHECK(scene.index() == ElementIndex<SceneLP> {unknown_index});
    CHECK(scene.count_scenario() == -1);
    CHECK(scene.first_scenario() == first_scenario_index());
    CHECK(scene.is_active());
  }

  TEST_CASE("Construction with scene and scenarios")
  {
    const Scene scene {
        .uid = 1,
        .name = "test_scene",
        .active = true,
        .first_scenario = 0,
        .count_scenario = 2,
    };

    std::vector<Scenario> scenarios {
        Scenario {.uid = 1, .active = true},
        Scenario {.uid = 2, .active = true},
        Scenario {.uid = 3, .active = false},
    };

    const SceneLP scene_lp(scene, scenarios, SceneIndex {1});

    CHECK(scene_lp.index() == SceneIndex {1});
    CHECK(scene_lp.is_active());
    CHECK(scene_lp.first_scenario() == first_scenario_index());
    CHECK(scene_lp.count_scenario() == 2);
  }

  TEST_CASE("Construction with simulation")
  {
    const Scene scene {
        .uid = 2,
        .name = "simulation_scene",
        .active = true,
        .first_scenario = 1,
        .count_scenario = 1,
    };

    const std::vector<Scenario> scenarios {
        Scenario {.uid = 1, .active = false},
        Scenario {.uid = 2, .active = true},
        Scenario {.uid = 3, .active = true},
    };

    SceneIndex index {2};
    const SceneLP scene_lp(scene, scenarios, index);

    CHECK(scene_lp.index() == index);
    CHECK(scene_lp.is_active());
    CHECK(scene_lp.first_scenario() == ScenarioIndex {1});
    CHECK(scene_lp.count_scenario() == 1);
  }

  TEST_CASE("Inactive scene")
  {
    const Scene scene {
        .uid = 3,
        .name = "inactive_scene",
        .active = false,
        .first_scenario = 0,
        .count_scenario = 2,
    };

    std::vector<Scenario> scenarios {
        Scenario {.uid = 1, .active = true},
        Scenario {.uid = 2, .active = true},
    };

    SceneIndex index {3};
    const SceneLP scene_lp(scene, scenarios, index);

    CHECK(scene_lp.index() == index);
    CHECK_FALSE(scene_lp.is_active());
  }

  TEST_CASE("SceneLP edge cases")
  {
    SUBCASE("Empty scenario list")
    {
      const Scene scene {
          .uid = 4,
          .name = "empty_scene",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 0,
      };

      const SceneLP scene_lp(scene, std::vector<Scenario> {});

      CHECK(scene_lp.count_scenario() == 0);
      CHECK(scene_lp.first_scenario() == first_scenario_index());
    }
  }
}

TEST_SUITE("StageLP")
{
  TEST_CASE("Construction and basic properties")
  {
    std::vector<Block> blocks = {
        Block {.uid = 1, .duration = 12.0},
        Block {.uid = 2, .duration = 24.0},
        Block {.uid = 3, .duration = 24.0},
        Block {.uid = 4, .duration = 12.0},
    };

    const Stage stage {
        .uid = 42,
        .active = true,
        .first_block = 1,
        .count_block = 2,
        .discount_factor = 0.9,
    };

    const StageLP stage_lp(stage, blocks, 0.05, StageIndex {1}, PhaseIndex {2});

    SUBCASE("Basic properties")
    {
      CHECK(stage_lp.stage().uid == 42);
      CHECK(stage_lp.index() == StageIndex {1});
      CHECK(stage_lp.phase_index() == PhaseIndex {2});
      CHECK(stage_lp.is_active() == true);
      CHECK(stage_lp.uid() == StageUid {42});
    }

    SUBCASE("Duration calculations")
    {
      CHECK(stage_lp.duration() == doctest::Approx(48.0));  // 24 + 24
      CHECK(stage_lp.timeinit()
            == doctest::Approx(12.0));  // first block's duration
    }

    SUBCASE("Discount factor calculations")
    {
      // With annual discount rate 5% and timeinit 12h (0.5 day)
      // daily rate = (1 + 0.05)^(1/365) - 1 ≈ 0.000134
      // discount factor for 0.5 day ≈ 1 - 0.000134/2 ≈ 0.999933
      // Combined with stage discount 0.9 → 0.9 * 0.999933 ≈ 0.89994
      CHECK(stage_lp.discount_factor()
            == doctest::Approx(0.9 * 0.999933).epsilon(1e-6));
    }

    SUBCASE("Block access")
    {
      const auto& stage_blocks = stage_lp.blocks();
      REQUIRE(stage_blocks.size() == 2);
      CHECK(stage_blocks[0].uid() == make_uid<Block>(2));
      CHECK(stage_blocks[1].uid() == make_uid<Block>(3));
    }
  }

  TEST_CASE("Edge cases")
  {
    std::vector<Block> blocks = {Block {.uid = 1, .duration = 24.0}};

    SUBCASE("Empty stage")
    {
      const Stage stage {.first_block = 0, .count_block = 0};

      const StageLP stage_lp(stage, blocks);

      CHECK(stage_lp.blocks().empty());
      CHECK(stage_lp.duration() == doctest::Approx(0.0));
      CHECK(stage_lp.timeinit() == doctest::Approx(0.0));
    }

    SUBCASE("Inactive stage")
    {
      const Stage stage {.active = false, .first_block = 0, .count_block = 1};

      const StageLP stage_lp(stage, blocks);

      CHECK(stage_lp.is_active() == false);
    }

    SUBCASE("Default discount factor")
    {
      const Stage stage {.first_block = 0, .count_block = 1};

      const StageLP stage_lp(stage, blocks, 0.0);

      CHECK(stage_lp.discount_factor() == doctest::Approx(1.0));
    }

    SUBCASE("No stage discount factor")
    {
      const Stage stage {.first_block = 0, .count_block = 1};

      const StageLP stage_lp(stage, blocks, 0.05);

      CHECK(stage_lp.discount_factor() == doctest::Approx(1.0).epsilon(1e-6));
    }
  }

  TEST_CASE("Move semantics")
  {
    std::vector<Block> blocks = {Block {.uid = 1, .duration = 24.0}};

    const Stage stage {.uid = 10, .first_block = 0, .count_block = 1};

    StageLP original(stage, blocks);
    const StageLP moved(std::move(original));

    CHECK(moved.uid() == StageUid {10});
    CHECK(moved.blocks().size() == 1);
    CHECK(moved.duration() == doctest::Approx(24.0));
  }
}
