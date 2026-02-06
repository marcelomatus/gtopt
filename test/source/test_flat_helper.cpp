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
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
      .stage_array = {{.uid = Uid {0}}},
      .scenario_array = {{.uid = Uid {0}}},

  };
  const SimulationLP simulation {psimulation, options};

  const FlatHelper helper(simulation);

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
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
      .stage_array = {{.uid = Uid {0}}},
      .scenario_array = {{.uid = Uid {0}}},

  };

  const SimulationLP simulation {psimulation, options};

  const FlatHelper helper(simulation);

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
    STBIndexHolder<Index> holder;
    const IndexHolder0<BlockUid> blocks = {
        {BlockUid {0}, 10},
        {BlockUid {1}, 20},
    };

    holder[{ScenarioUid {0}, StageUid {0}}] = blocks;

    const block_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == 10);
    CHECK(values[1] == 20);
  }

  SUBCASE("STIndexHolder")
  {
    STIndexHolder<Index> holder;
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
    const OptionsLP options;
    const Simulation psimulation;
    const SimulationLP simulation {psimulation, options};

    const FlatHelper helper(simulation);

    CHECK(helper.active_scenario_count() == 0);
    CHECK(helper.active_stage_count() == 0);
    CHECK(helper.active_block_count() == 0);
    CHECK_FALSE(helper.is_first_scenario(ScenarioUid {0}));
    CHECK_FALSE(helper.is_first_stage(StageUid {0}));
    CHECK_FALSE(helper.is_last_stage(StageUid {0}));

    // Test all flat() variants with empty inputs
    const GSTBIndexHolder gstb_holder;
    auto [gstb_values, gstb_valid] =
        helper.flat(gstb_holder, [](auto v) { return v; });
    CHECK(gstb_values.empty());
    CHECK(gstb_valid.empty());

    const STBIndexHolder<Index> stb_holder;
    auto [stb_values, stb_valid] =
        helper.flat(stb_holder, [](auto v) { return v; });
    CHECK(stb_values.empty());
    CHECK(stb_valid.empty());

    const STIndexHolder<Index> st_holder;
    auto [st_values, st_valid] =
        helper.flat(st_holder, [](auto v) { return v; });
    CHECK(st_values.empty());
    CHECK(st_valid.empty());

    const TIndexHolder t_holder;
    auto [t_values, t_valid] = helper.flat(t_holder, [](auto v) { return v; });
    CHECK(t_values.empty());
    CHECK(t_valid.empty());
  }

  SUBCASE("Partial Active Elements")
  {
    const OptionsLP options;
    const Simulation psimulation = {
        .block_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
        .stage_array =
            {
                {
                    .uid = Uid {0},
                    .active = 1,
                    .first_block = 0,
                    .count_block = 1,
                },
                {
                    .uid = Uid {1},
                    .active = 0,
                    .first_block = 1,
                    .count_block = 1,
                },
            },
        .scenario_array = {{.uid = Uid {0}}},

    };

    const SimulationLP simulation {psimulation, options};

    // Only some scenarios/stages active
    const FlatHelper helper(simulation);

    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;
    holder[{ScenarioUid {0}, StageUid {1}, BlockUid {1}}] =
        20;  // Inactive block

    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });
    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(10.0));
  }

  SUBCASE("Missing Values")
  {
    const OptionsLP options;
    const Simulation psimulation = {
        .block_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
        .stage_array = {{.uid = Uid {0}}},
        .scenario_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
    };

    const SimulationLP simulation {psimulation, options};
    const FlatHelper helper(simulation);

    GSTBIndexHolder holder;
    holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;

    const block_factor_matrix_t factor;
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
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {0}}},
      .stage_array = {{.uid = Uid {0}}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};

  SUBCASE("Move Construction")
  {
    FlatHelper original(simulation);
    const FlatHelper moved(std::move(original));

    CHECK(moved.active_scenario_count() == 1);
    CHECK(moved.active_stage_count() == 1);
    CHECK(moved.active_block_count() == 1);
  }

  SUBCASE("Move Assignment")
  {
    FlatHelper original(simulation);
    const SimulationLP simulation2 {{}, options};  // NOLINT

    FlatHelper moved(simulation2);
    moved = std::move(original);

    CHECK(moved.active_scenario_count() == 1);
    CHECK(moved.active_stage_count() == 1);
    CHECK(moved.active_block_count() == 1);
  }
}
TEST_CASE("FlatHelper Const Correctness")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {0}}},
      .stage_array = {{.uid = Uid {0}}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const SimulationLP simulation {psimulation, options};

  const FlatHelper helper(simulation);

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

    CHECK_NOTHROW(auto&& _ = helper.flat(holder, [](auto v) { return v; }));
  }
}
TEST_CASE("FlatHelper Template Constraints")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {0}}},
      .stage_array = {{.uid = Uid {0}}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};

  const FlatHelper helper(simulation);
  GSTBIndexHolder holder;
  holder[{ScenarioUid {0}, StageUid {0}, BlockUid {0}}] = 10;

  SUBCASE("Valid Projection")
  {
    CHECK_NOTHROW(auto&& _ =
                      helper.flat(holder, [](double v) { return v * 2; }));
  }

  SUBCASE("Invalid Projection")
  {
    // Should fail to compile - uncomment to verify
    // CHECK_THROWS(helper.flat(holder, [](std::string s) { return s; }));
  }
}
