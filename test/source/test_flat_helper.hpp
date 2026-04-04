#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>

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
  using namespace gtopt;
  const PlanningOptionsLP options;
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
  const PlanningOptionsLP options;
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

    block_factor_matrix_t factor(1, 1);
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
    const PlanningOptionsLP options;
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
    const PlanningOptionsLP options;
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
    const PlanningOptionsLP options;
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
  const PlanningOptionsLP options;
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
  const PlanningOptionsLP options;
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
  const PlanningOptionsLP options;
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

TEST_CASE("FlatHelper - stb_uids basic")
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {10}}, {.uid = Uid {20}}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.stb_uids();

  REQUIRE(uids.scenario_uids.size() == 2);
  REQUIRE(uids.stage_uids.size() == 2);
  REQUIRE(uids.block_uids.size() == 2);

  // Each block should have the same scenario and stage
  CHECK(uids.scenario_uids[0] == Uid {0});
  CHECK(uids.scenario_uids[1] == Uid {0});
  CHECK(uids.stage_uids[0] == Uid {1});
  CHECK(uids.stage_uids[1] == Uid {1});
  CHECK(uids.block_uids[0] == Uid {10});
  CHECK(uids.block_uids[1] == Uid {20});
}

TEST_CASE("FlatHelper - stb_uids multi-stage")
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
              {.uid = Uid {3}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {10}, .first_block = 0, .count_block = 1},
              {.uid = Uid {20}, .first_block = 1, .count_block = 2},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.stb_uids();

  REQUIRE(uids.scenario_uids.size() == 3);
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.block_uids[0] == Uid {1});
  CHECK(uids.stage_uids[1] == Uid {20});
  CHECK(uids.block_uids[1] == Uid {2});
  CHECK(uids.stage_uids[2] == Uid {20});
  CHECK(uids.block_uids[2] == Uid {3});
}

TEST_CASE("FlatHelper - st_uids basic")
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {1}}, {.uid = Uid {2}}},
      .stage_array =
          {
              {.uid = Uid {10}, .first_block = 0, .count_block = 1},
              {.uid = Uid {20}, .first_block = 1, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.st_uids();

  REQUIRE(uids.scenario_uids.size() == 2);
  REQUIRE(uids.stage_uids.size() == 2);

  CHECK(uids.scenario_uids[0] == Uid {0});
  CHECK(uids.scenario_uids[1] == Uid {0});
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.stage_uids[1] == Uid {20});
}

TEST_CASE("FlatHelper - st_uids multi-scenario")
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {1}}},
      .stage_array = {{.uid = Uid {10}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.st_uids();

  REQUIRE(uids.scenario_uids.size() == 2);
  CHECK(uids.scenario_uids[0] == Uid {0});
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.scenario_uids[1] == Uid {1});
  CHECK(uids.stage_uids[1] == Uid {10});
}

TEST_CASE("FlatHelper - t_uids basic")
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {1}}, {.uid = Uid {2}}},
      .stage_array =
          {
              {.uid = Uid {10}, .first_block = 0, .count_block = 1},
              {.uid = Uid {20}, .first_block = 1, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.t_uids();

  REQUIRE(uids.stage_uids.size() == 2);
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.stage_uids[1] == Uid {20});
}

TEST_CASE("FlatHelper - uids with empty simulation")
{
  const PlanningOptionsLP options;
  const Simulation psimulation;
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto stb = helper.stb_uids();
  CHECK(stb.scenario_uids.empty());
  CHECK(stb.stage_uids.empty());
  CHECK(stb.block_uids.empty());

  auto st = helper.st_uids();
  CHECK(st.scenario_uids.empty());
  CHECK(st.stage_uids.empty());

  auto t = helper.t_uids();
  CHECK(t.stage_uids.empty());
}

// ---------------------------------------------------------------------------
// GSTB flat with non-zero UIDs and scaling factors
// ---------------------------------------------------------------------------

TEST_CASE("FlatHelper - GSTB flat with non-zero UIDs and factors")  // NOLINT
{
  const PlanningOptionsLP options;
  // Use non-zero, non-contiguous UIDs to verify index-based factor lookup
  const Simulation psimulation = {
      .block_array =
          {
              {.uid = Uid {10}, .duration = 1},
              {.uid = Uid {20}, .duration = 2},
              {.uid = Uid {30}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {5}, .first_block = 0, .count_block = 2},
              {.uid = Uid {7}, .first_block = 2, .count_block = 1},
          },
      .scenario_array =
          {
              {.uid = Uid {3}},
              {.uid = Uid {8}},
          },
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  SUBCASE("GSTB flat without factor works with non-zero UIDs")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioUid {3}, StageUid {5}, BlockUid {10}}] = 100;
    holder[{ScenarioUid {3}, StageUid {5}, BlockUid {20}}] = 200;
    holder[{ScenarioUid {3}, StageUid {7}, BlockUid {30}}] = 300;
    holder[{ScenarioUid {8}, StageUid {5}, BlockUid {10}}] = 400;
    holder[{ScenarioUid {8}, StageUid {5}, BlockUid {20}}] = 500;
    holder[{ScenarioUid {8}, StageUid {7}, BlockUid {30}}] = 600;

    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    // 2 scenarios × 3 blocks = 6 values
    REQUIRE(values.size() == 6);
    // All present → no valid vector needed
    REQUIRE(valid.empty());

    CHECK(values[0] == doctest::Approx(100.0));
    CHECK(values[1] == doctest::Approx(200.0));
    CHECK(values[2] == doctest::Approx(300.0));
    CHECK(values[3] == doctest::Approx(400.0));
    CHECK(values[4] == doctest::Approx(500.0));
    CHECK(values[5] == doctest::Approx(600.0));
  }

  SUBCASE("GSTB flat with factor uses index-based lookup, not UID-based")
  {
    GSTBIndexHolder holder;
    holder[{ScenarioUid {3}, StageUid {5}, BlockUid {10}}] = 100;
    holder[{ScenarioUid {3}, StageUid {5}, BlockUid {20}}] = 200;
    holder[{ScenarioUid {3}, StageUid {7}, BlockUid {30}}] = 300;
    holder[{ScenarioUid {8}, StageUid {5}, BlockUid {10}}] = 400;
    holder[{ScenarioUid {8}, StageUid {5}, BlockUid {20}}] = 500;
    holder[{ScenarioUid {8}, StageUid {7}, BlockUid {30}}] = 600;

    // Factor matrix: 2 scenarios × 2 stages, each stage has its block factors
    block_factor_matrix_t factor(2, 2);
    factor[0][0] = {2.0, 3.0};  // scenario idx 0, stage idx 0: 2 blocks
    factor[0][1] = {4.0};  // scenario idx 0, stage idx 1: 1 block
    factor[1][0] = {0.5, 0.25};  // scenario idx 1, stage idx 0: 2 blocks
    factor[1][1] = {10.0};  // scenario idx 1, stage idx 1: 1 block

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);

    REQUIRE(values.size() == 6);
    REQUIRE(valid.empty());

    // scenario 0 (uid=3), stage 0 (uid=5), block 0 (uid=10): 100 * 2.0
    CHECK(values[0] == doctest::Approx(200.0));
    // scenario 0 (uid=3), stage 0 (uid=5), block 1 (uid=20): 200 * 3.0
    CHECK(values[1] == doctest::Approx(600.0));
    // scenario 0 (uid=3), stage 1 (uid=7), block 0 (uid=30): 300 * 4.0
    CHECK(values[2] == doctest::Approx(1200.0));
    // scenario 1 (uid=8), stage 0 (uid=5), block 0 (uid=10): 400 * 0.5
    CHECK(values[3] == doctest::Approx(200.0));
    // scenario 1 (uid=8), stage 0 (uid=5), block 1 (uid=20): 500 * 0.25
    CHECK(values[4] == doctest::Approx(125.0));
    // scenario 1 (uid=8), stage 1 (uid=7), block 0 (uid=30): 600 * 10.0
    CHECK(values[5] == doctest::Approx(6000.0));
  }

  SUBCASE("STB flat with factor and non-zero UIDs")
  {
    STBIndexHolder<Index> holder;
    holder[{ScenarioUid {3}, StageUid {5}}] = {
        {BlockUid {10}, 100},
        {BlockUid {20}, 200},
    };
    holder[{ScenarioUid {3}, StageUid {7}}] = {
        {BlockUid {30}, 300},
    };
    holder[{ScenarioUid {8}, StageUid {5}}] = {
        {BlockUid {10}, 400},
        {BlockUid {20}, 500},
    };
    holder[{ScenarioUid {8}, StageUid {7}}] = {
        {BlockUid {30}, 600},
    };

    block_factor_matrix_t factor(2, 2);
    factor[0][0] = {2.0, 3.0};
    factor[0][1] = {4.0};
    factor[1][0] = {0.5, 0.25};
    factor[1][1] = {10.0};

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);

    REQUIRE(values.size() == 6);

    CHECK(values[0] == doctest::Approx(200.0));
    CHECK(values[1] == doctest::Approx(600.0));
    CHECK(values[2] == doctest::Approx(1200.0));
    CHECK(values[3] == doctest::Approx(200.0));
    CHECK(values[4] == doctest::Approx(125.0));
    CHECK(values[5] == doctest::Approx(6000.0));
  }

  SUBCASE("ST flat with factor and non-zero UIDs")
  {
    STIndexHolder<Index> holder;
    holder[{ScenarioUid {3}, StageUid {5}}] = 100;
    holder[{ScenarioUid {3}, StageUid {7}}] = 200;
    holder[{ScenarioUid {8}, StageUid {5}}] = 300;
    holder[{ScenarioUid {8}, StageUid {7}}] = 400;

    scenario_stage_factor_matrix_t factor(2, 2);
    factor[0][0] = 2.0;
    factor[0][1] = 3.0;
    factor[1][0] = 0.5;
    factor[1][1] = 0.25;

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);

    REQUIRE(values.size() == 4);

    CHECK(values[0] == doctest::Approx(200.0));
    CHECK(values[1] == doctest::Approx(600.0));
    CHECK(values[2] == doctest::Approx(150.0));
    CHECK(values[3] == doctest::Approx(100.0));
  }

  SUBCASE("T flat with factor and non-zero UIDs")
  {
    TIndexHolder holder;
    holder[StageUid {5}] = 100;
    holder[StageUid {7}] = 200;

    stage_factor_matrix_t factor = {2.0, 3.0};

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);

    REQUIRE(values.size() == 2);

    CHECK(values[0] == doctest::Approx(200.0));
    CHECK(values[1] == doctest::Approx(600.0));
  }
}

TEST_CASE("FlatHelper - GSTB flat sparse with non-zero UIDs")  // NOLINT
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array =
          {
              {.uid = Uid {10}, .duration = 1},
              {.uid = Uid {20}, .duration = 2},
          },
      .stage_array =
          {
              {.uid = Uid {5}, .first_block = 0, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {3}},
          },
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  // Only populate one of two blocks
  GSTBIndexHolder holder;
  holder[{ScenarioUid {3}, StageUid {5}, BlockUid {10}}] = 100;
  // BlockUid {20} is missing → sparse

  block_factor_matrix_t factor(1, 1);
  factor[0][0] = {2.0, 3.0};

  auto [values, valid] = helper.flat(holder, [](auto v) { return v; }, factor);

  REQUIRE(values.size() == 2);
  REQUIRE(valid.size() == 2);

  CHECK(values[0] == doctest::Approx(200.0));  // 100 * 2.0
  CHECK(valid[0] == true);
  CHECK(values[1] == doctest::Approx(0.0));  // missing
  CHECK(valid[1] == false);
}
