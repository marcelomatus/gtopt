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

    GSTBIndexHolder holder;
    block_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });
    CHECK(values.empty());
    CHECK(valid.empty());
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
