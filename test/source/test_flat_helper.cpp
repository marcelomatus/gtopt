#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

/**
 * @test Verify active element accessor functionality
 *
 * Tests that FlatHelper correctly:
 * - Reports counts of active scenarios/stages/blocks
 * - Identifies first/last elements
 * - Provides const access to active element collections
 */
TEST_CASE("Active Elements Accessors")
{
  OptionsLP options;
  Simulation psimulation;
  SimulationLP simulation {psimulation, options};

  const std::vector<ScenarioUid> active_scenarios = {ScenarioUid {0}};
  const std::vector<StageUid> active_stages = {StageUid {0}};
  const std::vector<std::vector<BlockUid>> active_stage_blocks = {
      {BlockUid {0}, BlockUid {1}}};
  const std::vector<BlockUid> active_blocks = {BlockUid {0}, BlockUid {1}};

  const FlatHelper helper(simulation,
                          active_scenarios,
                          active_stages,
                          active_stage_blocks,
                          active_blocks);

  SUBCASE("Scenario accessors")
  {
    CHECK(helper.active_scenarios().size() == 1);
    CHECK(helper.active_scenario_count() == 1);
    CHECK(helper.is_first_scenario(ScenarioUid {0}));
  }

  SUBCASE("Stage accessors")
  {
    CHECK(helper.active_stages().size() == 1);
    CHECK(helper.active_stage_count() == 1);
    CHECK(helper.is_first_stage(StageUid {0}));
    CHECK(helper.is_last_stage(StageUid {0}));
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
  std::vector<ScenarioUid> active_scenarios = {ScenarioUid {0}};
  std::vector<StageUid> active_stages = {StageUid {0}};
  std::vector<std::vector<BlockUid>> active_stage_blocks = {
      {BlockUid {0}, BlockUid {1}}};
  std::vector<BlockUid> active_blocks = {BlockUid {0}, BlockUid {1}};

  OptionsLP options;
  Simulation psimulation;
  SimulationLP simulation {psimulation, options};

  FlatHelper helper(simulation,
                    active_scenarios,
                    active_stages,
                    active_stage_blocks,
                    active_blocks);

  SUBCASE("GSTBIndexHolder")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {1}}] = 20;

    const block_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(10.0));
    CHECK(values[1] == doctest::Approx(20.0));
  }

  SUBCASE("STBIndexHolder")
  {
    STBIndexHolder holder;
    const IndexHolder0<BlockUid> blocks = {{BlockUid {0}, 10},
                                           {BlockUid {1}, 20}};

    holder[{ScenarioUid {0}, StageUid {0}}] = blocks;

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
    holder[{ScenarioUid {0}, StageUid {0}}] = 42;

    const scenario_stage_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(42.0));
  }

  SUBCASE("TIndexHolder")
  {
    TIndexHolder holder;
    holder[StageUid {0}] = 99;

    const stage_factor_matrix_t factor {};
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(99.0));
  }

  SUBCASE("With Factor")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;

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
    OptionsLP options;
    Simulation psimulation;
    SimulationLP simulation {psimulation, options};

    FlatHelper helper(simulation, {}, {}, {}, {});

    CHECK(helper.active_scenario_count() == 0);
    CHECK(helper.active_stage_count() == 0);
    CHECK(helper.active_block_count() == 0);
    CHECK_FALSE(helper.is_first_scenario(ScenarioUid {0}));
    CHECK_FALSE(helper.is_first_stage(StageUid {0}));
    CHECK_FALSE(helper.is_last_stage(StageUid {0}));

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
    OptionsLP options;
    Simulation psimulation;
    SimulationLP simulation {psimulation, options};

    // Only some scenarios/stages active
    FlatHelper helper(simulation,
                      {ScenarioUid {0}},
                      {StageUid {0}},
                      {{BlockUid {0}}},  // Only first block active
                      {BlockUid {0}});

    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {1}}] =
        20;  // Inactive block

    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });
    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(10.0));
  }

  SUBCASE("Missing Values")
  {
    std::vector<ScenarioUid> active_scenarios = {ScenarioUid {0},
                                                 ScenarioUid {1}};
    std::vector<StageUid> active_stages = {StageUid {0}, StageUid {1}};
    std::vector<std::vector<BlockUid>> active_stage_blocks = {{BlockUid {0}},
                                                              {BlockUid {1}}};
    std::vector<BlockUid> active_blocks = {BlockUid {0}, BlockUid {1}};

    OptionsLP options;
    Simulation psimulation;
    SimulationLP simulation {psimulation, options};
    FlatHelper helper(simulation,
                      active_scenarios,
                      active_stages,
                      active_stage_blocks,
                      active_blocks);

    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;

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
  std::vector<ScenarioUid> scenarios = {ScenarioUid {0}};
  std::vector<StageUid> stages = {StageUid {0}};
  std::vector<std::vector<BlockUid>> stage_blocks = {{BlockUid {0}}};
  std::vector<BlockUid> blocks = {BlockUid {0}};

  OptionsLP options;
  Simulation psimulation;
  SimulationLP simulation {psimulation, options};

  SUBCASE("Move Construction")
  {
    FlatHelper original(simulation, scenarios, stages, stage_blocks, blocks);
    FlatHelper moved(std::move(original));

    CHECK(moved.active_scenario_count() == 1);
    CHECK(moved.active_stage_count() == 1);
    CHECK(moved.active_block_count() == 1);
  }

  SUBCASE("Move Assignment")
  {
    FlatHelper original(simulation, scenarios, stages, stage_blocks, blocks);
    FlatHelper moved(simulation,
                     {ScenarioUid {0}},
                     {StageUid {0}},
                     {{BlockUid {0}}},
                     {BlockUid {0}});
    moved = std::move(original);

    CHECK(moved.active_scenario_count() == 1);
    CHECK(moved.active_stage_count() == 1);
    CHECK(moved.active_block_count() == 1);
  }
}
TEST_CASE("FlatHelper Const Correctness")
{
  const std::vector<ScenarioUid> scenarios = {ScenarioUid {0}};
  const std::vector<StageUid> stages = {StageUid {0}};
  const std::vector<std::vector<BlockUid>> stage_blocks = {{BlockUid {0}}};
  const std::vector<BlockUid> blocks = {BlockUid {0}};

  OptionsLP options;
  Simulation psimulation;
  SimulationLP simulation {psimulation, options};

  const FlatHelper helper(simulation, scenarios, stages, stage_blocks, blocks);

  SUBCASE("Const Accessors")
  {
    CHECK_NOTHROW(auto&& _ = helper.active_scenarios());
    CHECK_NOTHROW(auto&& _ = helper.active_stages());
    CHECK_NOTHROW(auto&& _ = helper.active_stage_blocks());
    CHECK_NOTHROW(auto&& _ = helper.active_blocks());
  }

  SUBCASE("Const Flat Methods")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;

    CHECK_NOTHROW(helper.flat(holder, [](auto v) { return v; }));
  }
}
TEST_CASE("FlatHelper Template Constraints")
{
  OptionsLP options;
  Simulation psimulation;
  SimulationLP simulation {psimulation, options};

  FlatHelper helper(simulation,
                    {ScenarioUid {0}},
                    {StageUid {0}},
                    {{BlockUid {0}}},
                    {BlockUid {0}});
  GSTBIndexHolder holder;
  holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;

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
