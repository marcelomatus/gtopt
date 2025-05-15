#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

TEST_CASE("Active Elements Accessors")
{
  std::vector<ScenarioIndex> active_scenarios = {ScenarioIndex {0}};
  std::vector<StageIndex> active_stages = {StageIndex {0}};
  std::vector<std::vector<BlockIndex>> active_stage_blocks = {
      {BlockIndex {0}, BlockIndex {1}}};
  std::vector<BlockIndex> active_blocks = {BlockIndex {0}, BlockIndex {1}};

  FlatHelper helper(
      active_scenarios, active_stages, active_stage_blocks, active_blocks);

  SUBCASE("Scenario accessors")
  {
    CHECK(helper.active_scenarios().size() == 1);
    CHECK(helper.active_scenario_count() == 1);
    CHECK(helper.is_first_scenario(ScenarioIndex {0}));
  }

  SUBCASE("Stage accessors")
  {
    CHECK(helper.active_stages().size() == 1);
    CHECK(helper.active_stage_count() == 1);
    CHECK(helper.is_first_stage(StageIndex {0}));
    CHECK(helper.is_last_stage(StageIndex {0}));
  }

  SUBCASE("Block accessors")
  {
    CHECK(helper.active_blocks().size() == 2);
    CHECK(helper.active_block_count() == 2);
    CHECK(helper.active_stage_blocks().size() == 1);
    CHECK(helper.active_stage_blocks()[0].size() == 2);
  }
}

TEST_CASE("Flat helper - Flat Methods")
{
  std::vector<ScenarioIndex> active_scenarios = {ScenarioIndex {0}};
  std::vector<StageIndex> active_stages = {StageIndex {0}};
  std::vector<std::vector<BlockIndex>> active_stage_blocks = {
      {BlockIndex {0}, BlockIndex {1}}};
  std::vector<BlockIndex> active_blocks = {BlockIndex {0}, BlockIndex {1}};

  FlatHelper helper(
      active_scenarios, active_stages, active_stage_blocks, active_blocks);

  SUBCASE("GSTBIndexHolder")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {0}}] = 10;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {1}}] = 20;

    block_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(10.0));
    CHECK(values[1] == doctest::Approx(20.0));
  }

  SUBCASE("STBIndexHolder")
  {
    STBIndexHolder holder;
    IndexHolder0<BlockIndex> blocks = {10, 20};

    holder[{ScenarioIndex {0}, StageIndex {0}}] = blocks;

    block_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == 10);
    CHECK(values[1] == 20);
  }

  SUBCASE("STIndexHolder")
  {
    STIndexHolder holder;
    holder[{ScenarioIndex {0}, StageIndex {0}}] = 42;

    scenario_stage_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(42.0));
  }

  SUBCASE("TIndexHolder")
  {
    TIndexHolder holder;
    holder[StageIndex {0}] = 99;

    stage_factor_matrix_t factor {};
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(99.0));
  }

  SUBCASE("With Factor")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {0}}] = 10;

    block_factor_matrix_t factor(boost::extents[1][1]);
    factor[0][0] = {2.0};  // Factor for block 0

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);

    REQUIRE(values.size() == 2);
    CHECK(values[0] == doctest::Approx(20.0));  // 10.0 * 2.0
    CHECK(values[1] == doctest::Approx(0.0));  // No value set
    CHECK(valid[0] == true);
    CHECK(valid[1] == false);
  }
}

TEST_CASE("Flat Helper - Edge Cases")
{
  SUBCASE("Empty Active Elements")
  {
    FlatHelper helper({}, {}, {}, {});

    CHECK(helper.active_scenario_count() == 0);
    CHECK(helper.active_stage_count() == 0);
    CHECK(helper.active_block_count() == 0);
    CHECK_FALSE(helper.is_first_scenario(ScenarioIndex {0}));
    CHECK_FALSE(helper.is_first_stage(StageIndex {0}));
    CHECK_FALSE(helper.is_last_stage(StageIndex {0}));

    // Test all flat() variants with empty inputs
    GSTBIndexHolder gstb_holder;
    auto [gstb_values, gstb_valid] =
        helper.flat(gstb_holder, [](auto v) { return v; });
    CHECK(gstb_values.empty());
    CHECK(gstb_valid.empty());

    STBIndexHolder stb_holder;
    auto [stb_values, stb_valid] =
        helper.flat(stb_holder, [](auto v) { return v; });
    CHECK(stb_values.empty());
    CHECK(stb_valid.empty());

    STIndexHolder st_holder;
    auto [st_values, st_valid] =
        helper.flat(st_holder, [](auto v) { return v; });
    CHECK(st_values.empty());
    CHECK(st_valid.empty());

    TIndexHolder t_holder;
    auto [t_values, t_valid] = helper.flat(t_holder, [](auto v) { return v; });
    CHECK(t_values.empty());
    CHECK(t_valid.empty());
  }

  SUBCASE("Partial Active Elements")
  {
    // Only some scenarios/stages active
    FlatHelper helper({ScenarioIndex {0}},
                      {StageIndex {0}},
                      {{BlockIndex {0}}},  // Only first block active
                      {BlockIndex {0}});

    GSTBIndexHolder holder;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {0}}] = 10;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {1}}] =
        20;  // Inactive block

    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });
    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 1);
    CHECK(values[0] == doctest::Approx(10.0));
    CHECK(valid[0] == true);
  }

  SUBCASE("Missing Values")
  {
    std::vector<ScenarioIndex> active_scenarios = {ScenarioIndex {0},
                                                   ScenarioIndex {1}};
    std::vector<StageIndex> active_stages = {StageIndex {0}, StageIndex {1}};
    std::vector<std::vector<BlockIndex>> active_stage_blocks = {
        {BlockIndex {0}}, {BlockIndex {1}}};
    std::vector<BlockIndex> active_blocks = {BlockIndex {0}, BlockIndex {1}};

    FlatHelper helper(
        active_scenarios, active_stages, active_stage_blocks, active_blocks);

    GSTBIndexHolder holder;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {0}}] = 10;

    block_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 4);  // 2 scenarios * 2 blocks
    CHECK(values[0] == doctest::Approx(10.0));
    CHECK(values[1] == doctest::Approx(0.0));
    CHECK(values[2] == doctest::Approx(0.0));
    CHECK(values[3] == doctest::Approx(0.0));
    CHECK(valid[0] == true);
    CHECK(valid[1] == false);
    CHECK(valid[2] == false);
    CHECK(valid[3] == false);
  }
}
TEST_CASE("FlatHelper Move Semantics")
{
  std::vector<ScenarioIndex> scenarios = {ScenarioIndex {0}};
  std::vector<StageIndex> stages = {StageIndex {0}};
  std::vector<std::vector<BlockIndex>> stage_blocks = {{BlockIndex {0}}};
  std::vector<BlockIndex> blocks = {BlockIndex {0}};

  SUBCASE("Move Construction")
  {
    FlatHelper original(scenarios, stages, stage_blocks, blocks);
    FlatHelper moved(std::move(original));

    CHECK(moved.active_scenario_count() == 1);
    CHECK(moved.active_stage_count() == 1);
    CHECK(moved.active_block_count() == 1);
  }

  SUBCASE("Move Assignment")
  {
    FlatHelper original(scenarios, stages, stage_blocks, blocks);
    FlatHelper moved;
    moved = std::move(original);

    CHECK(moved.active_scenario_count() == 1);
    CHECK(moved.active_stage_count() == 1);
    CHECK(moved.active_block_count() == 1);
  }
}
TEST_CASE("FlatHelper Const Correctness")
{
  const std::vector<ScenarioIndex> scenarios = {ScenarioIndex {0}};
  const std::vector<StageIndex> stages = {StageIndex {0}};
  const std::vector<std::vector<BlockIndex>> stage_blocks = {{BlockIndex {0}}};
  const std::vector<BlockIndex> blocks = {BlockIndex {0}};

  const FlatHelper helper(scenarios, stages, stage_blocks, blocks);

  SUBCASE("Const Accessors")
  {
    CHECK_NOTHROW(helper.active_scenarios());
    CHECK_NOTHROW(helper.active_stages());
    CHECK_NOTHROW(helper.active_stage_blocks());
    CHECK_NOTHROW(helper.active_blocks());
  }

  SUBCASE("Const Flat Methods")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {0}}] = 10.0;

    CHECK_NOTHROW(helper.flat(holder, [](auto v) { return v; }));
  }
}
TEST_CASE("FlatHelper Template Constraints")
{
  FlatHelper helper({ScenarioIndex {0}},
                    {StageIndex {0}},
                    {{BlockIndex {0}}},
                    {BlockIndex {0}});
  GSTBIndexHolder holder;
  holder[{ScenarioIndex {0}, StageIndex {0}, BlockIndex {0}}] = 10.0;

  SUBCASE("Valid Projection")
  {
    CHECK_NOTHROW(helper.flat(holder, [](double v) { return v * 2; }));
  }

  SUBCASE("Invalid Projection")
  {
    // Should fail to compile - uncomment to verify
    // CHECK_THROWS(helper.flat(holder, [](std::string s) { return s; }));
  }
}
