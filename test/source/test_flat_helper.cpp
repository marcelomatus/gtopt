#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
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
    CHECK(helper.is_first_scenario(make_uid<Scenario>(0)));
  }

  SUBCASE("Stage accessors")
  {
    CHECK(helper.active_stages().size() == 1);
    CHECK(helper.active_stage_count() == 1);
    CHECK(helper.is_first_stage(make_uid<Stage>(0)));
    CHECK(helper.is_last_stage(make_uid<Stage>(0)));
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
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)}] =
        10;
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(1)}] =
        20;

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
        {make_uid<Block>(0), 10},
        {make_uid<Block>(1), 20},
    };

    holder[{make_uid<Scenario>(0), make_uid<Stage>(0)}] = blocks;

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
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0)}] = 42;

    const scenario_stage_factor_matrix_t factor;
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(42.0));
  }

  SUBCASE("TIndexHolder")
  {
    TIndexHolder holder;
    holder[make_uid<Stage>(0)] = 99;

    const stage_factor_matrix_t factor {};
    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

    REQUIRE(values.size() == 1);
    REQUIRE(valid.size() == 0);
    CHECK(values[0] == doctest::Approx(99.0));
  }

  SUBCASE("With Factor")
  {
    GSTBIndexHolder holder;
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)}] =
        10;

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
    CHECK_FALSE(helper.is_first_scenario(make_uid<Scenario>(0)));
    CHECK_FALSE(helper.is_first_stage(make_uid<Stage>(0)));
    CHECK_FALSE(helper.is_last_stage(make_uid<Stage>(0)));

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
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)}] =
        10;
    holder[{make_uid<Scenario>(0), make_uid<Stage>(1), make_uid<Block>(1)}] =
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
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)}] =
        10;

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

// Regression test for the P1 zero-bound skip contract.
//
// `GeneratorLP` / `WaterwayLP` / `DemandLP` may elide per-block LP
// columns when both dispatch bounds collapse to `[0, 0]`.  When SOME
// blocks of an (s, t) cell are elided but others survive, the outer
// (s, t) key in the holder is present and the inner BIndexHolder is
// non-empty — but it lacks the elided block uids.  Pre-2026-05-15
// the flat() helper called `stiter->second.at(buid)` unconditionally,
// which threw `std::out_of_range` on the elided blocks and caused
// `write_out()` to crash on the juan/IPLP case during the SDDP
// simulation pass.  The fixed helper uses `find()` and falls through
// to `valid[idx] = false` for missing blocks — same behaviour as the
// outer-key-missing case.
TEST_CASE("Flat Helper - STBIndexHolder tolerates partial-block elision")
{
  const PlanningOptionsLP options;
  const Simulation psimulation = {
      .block_array =
          {
              {
                  .uid = Uid {0},
              },
              {
                  .uid = Uid {1},
              },
          },
      .stage_array = {{.uid = Uid {0}}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  SUBCASE("STBIndexHolder - inner map missing one block (no factor)")
  {
    // Outer (scenario 0, stage 0) is present, inner map has ONLY
    // Block 0 — Block 1 was elided by a P1 zero-bound skip.  Pre-fix
    // this threw std::out_of_range on `at(Block 1)`.
    STBIndexHolder<Index> holder;
    const IndexHolder0<BlockUid> partial = {
        {make_uid<Block>(0), 10},
    };
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0)}] = partial;

    auto [values, valid] = helper.flat(holder, [](auto v) { return v; });
    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 2);
    CHECK(values[0] == doctest::Approx(10.0));
    CHECK(values[1] == doctest::Approx(0.0));
    CHECK(valid[0] == true);
    CHECK(valid[1] == false);
  }

  SUBCASE("STBIndexHolder - inner map missing one block (with factor)")
  {
    // Same as above but with a non-empty per-block factor matrix to
    // exercise the multiplicative path (values[idx] = value * factor).
    STBIndexHolder<Index> holder;
    const IndexHolder0<BlockUid> partial = {
        {make_uid<Block>(1), 20},
    };
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0)}] = partial;

    block_factor_matrix_t factor(1, 1);
    factor[0][0] = {3.0, 5.0};  // per-block factors for blocks 0,1

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);
    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 2);
    CHECK(values[0] == doctest::Approx(0.0));
    CHECK(values[1] == doctest::Approx(100.0));  // 20 × 5.0
    CHECK(valid[0] == false);
    CHECK(valid[1] == true);
  }

  SUBCASE("STBIndexHolder - st_scale overload also tolerates elision")
  {
    // Same scenario but exercising the st_scale overload (line ~357
    // in flat_helper.hpp), used by StorageLP::add_to_output to
    // back-scale daily-cycle volume-balance duals.
    STBIndexHolder<Index> holder;
    const IndexHolder0<BlockUid> partial = {
        {make_uid<Block>(0), 8},
    };
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0)}] = partial;

    block_factor_matrix_t factor;
    STIndexHolder<double> st_scale;
    st_scale[{make_uid<Scenario>(0), make_uid<Stage>(0)}] = 2.0;

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor, st_scale);
    REQUIRE(values.size() == 2);
    REQUIRE(valid.size() == 2);
    CHECK(values[0] == doctest::Approx(16.0));  // 8 × 2.0 (ss)
    CHECK(values[1] == doctest::Approx(0.0));
    CHECK(valid[0] == true);
    CHECK(valid[1] == false);
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
    holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)}] =
        10;

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
  holder[{make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)}] = 10;

  SUBCASE("Valid Projection")
  {
    CHECK_NOTHROW(auto&& _ =
                      helper.flat(holder, [](double v) { return v * 2; }));
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
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(10)}] =
        100;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(20)}] =
        200;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(30)}] =
        300;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5), make_uid<Block>(10)}] =
        400;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5), make_uid<Block>(20)}] =
        500;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(7), make_uid<Block>(30)}] =
        600;

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
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(10)}] =
        100;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(20)}] =
        200;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(30)}] =
        300;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5), make_uid<Block>(10)}] =
        400;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5), make_uid<Block>(20)}] =
        500;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(7), make_uid<Block>(30)}] =
        600;

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
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5)}] = {
        {make_uid<Block>(10), 100},
        {make_uid<Block>(20), 200},
    };
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7)}] = {
        {make_uid<Block>(30), 300},
    };
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5)}] = {
        {make_uid<Block>(10), 400},
        {make_uid<Block>(20), 500},
    };
    holder[{make_uid<Scenario>(8), make_uid<Stage>(7)}] = {
        {make_uid<Block>(30), 600},
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
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5)}] = 100;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7)}] = 200;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5)}] = 300;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(7)}] = 400;

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
    holder[make_uid<Stage>(5)] = 100;
    holder[make_uid<Stage>(7)] = 200;

    stage_factor_matrix_t factor = {2.0, 3.0};

    auto [values, valid] =
        helper.flat(holder, [](auto v) { return v; }, factor);

    REQUIRE(values.size() == 2);

    CHECK(values[0] == doctest::Approx(200.0));
    CHECK(values[1] == doctest::Approx(600.0));
  }
}

// ---------------------------------------------------------------------------
// Indexed-sparse-fill ⇔ brute-force-dense-scan equivalence pins
// ---------------------------------------------------------------------------
//
// flat() was rewritten (2026-07) to fill only the holder's own slots via
// precomputed uid→position maps instead of scanning the whole
// scenario × block grid per element-field stream (the per-cell write-out
// hot path).  These reference implementations replicate the historical
// dense triple loop verbatim; the test pins the new indexed fill to be
// bit-identical to it — same vector sizes, same slot positions, same
// defaults for non-owned slots, same empty-vector elisions.

namespace
{

template<typename Value, typename Proj>
auto dense_ref_gstb(const FlatHelper& h,
                    const GSTBIndexHolder<Value>& hstb,
                    Proj proj,
                    const block_factor_matrix_t& factor = {})
{
  if (hstb.empty()) {
    return std::pair {std::vector<double> {}, std::vector<bool> {}};
  }
  const auto size = h.active_scenario_count() * h.active_block_count();
  std::vector<double> values(size);
  std::vector<bool> valid(size, false);
  bool need_values = false;
  bool need_valids = false;
  size_t idx = 0;
  size_t count = 0;
  for (size_t si = 0; si < h.active_scenarios().size(); ++si) {
    for (size_t ti = 0; ti < h.active_stages().size(); ++ti) {
      const auto& blocks = h.active_stage_blocks()[ti];
      for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const auto it = hstb.find(
            {h.active_scenarios()[si], h.active_stages()[ti], blocks[bi]});
        if (it != hstb.end()) {
          const auto value = proj(it->second);
          values[idx] = factor.empty() ? value : value * factor[si][ti][bi];
          valid[idx] = true;
          ++count;
          need_values = true;
        }
        need_valids |= count != ++idx;
      }
    }
  }
  return std::pair {need_values ? std::move(values) : std::vector<double> {},
                    need_valids ? std::move(valid) : std::vector<bool> {}};
}

template<typename Value, typename Proj>
auto dense_ref_stb(const FlatHelper& h,
                   const STBIndexHolder<Value>& hstb,
                   Proj proj,
                   const block_factor_matrix_t& factor = {},
                   const STIndexHolder<double>& st_scale = {})
{
  if (hstb.empty()) {
    return std::pair {std::vector<double> {}, std::vector<bool> {}};
  }
  const auto size = h.active_scenario_count() * h.active_block_count();
  std::vector<double> values(size);
  std::vector<bool> valid(size, false);
  bool need_values = false;
  bool need_valids = false;
  size_t idx = 0;
  size_t count = 0;
  for (size_t si = 0; si < h.active_scenarios().size(); ++si) {
    for (size_t ti = 0; ti < h.active_stages().size(); ++ti) {
      const auto stiter =
          hstb.find({h.active_scenarios()[si], h.active_stages()[ti]});
      const auto has_stuid = stiter != hstb.end() && !stiter->second.empty();
      const auto ss_iter =
          st_scale.find({h.active_scenarios()[si], h.active_stages()[ti]});
      const double ss = (ss_iter != st_scale.end()) ? ss_iter->second : 1.0;
      const auto& blocks = h.active_stage_blocks()[ti];
      for (size_t bi = 0; bi < blocks.size(); ++bi) {
        if (has_stuid) {
          if (auto it = stiter->second.find(blocks[bi]);
              it != stiter->second.end())
          {
            const auto value = proj(it->second);
            values[idx] =
                factor.empty() ? value * ss : value * ss * factor[si][ti][bi];
            valid[idx] = true;
            ++count;
            need_values = true;
          }
        }
        need_valids |= count != ++idx;
      }
    }
  }
  return std::pair {need_values ? std::move(values) : std::vector<double> {},
                    need_valids ? std::move(valid) : std::vector<bool> {}};
}

template<typename Value, typename Proj>
auto dense_ref_st(const FlatHelper& h,
                  const STIndexHolder<Value>& hst,
                  Proj proj,
                  const scenario_stage_factor_matrix_t& factor = {})
{
  if (hst.empty()) {
    return std::pair {std::vector<double> {}, std::vector<bool> {}};
  }
  const auto size = h.active_scenario_count() * h.active_stage_count();
  std::vector<double> values(size);
  std::vector<bool> valid(size, false);
  bool need_values = false;
  bool need_valids = false;
  size_t idx = 0;
  size_t count = 0;
  for (size_t si = 0; si < h.active_scenarios().size(); ++si) {
    for (size_t ti = 0; ti < h.active_stages().size(); ++ti) {
      const auto it =
          hst.find({h.active_scenarios()[si], h.active_stages()[ti]});
      if (it != hst.end()) {
        const auto value = proj(it->second);
        values[idx] = factor.empty() ? value : value * factor[si][ti];
        valid[idx] = true;
        ++count;
        need_values = true;
      }
      need_valids |= count != ++idx;
    }
  }
  return std::pair {need_values ? std::move(values) : std::vector<double> {},
                    need_valids ? std::move(valid) : std::vector<bool> {}};
}

template<typename Value, typename Proj>
auto dense_ref_t(const FlatHelper& h,
                 const TIndexHolder<Value>& ht,
                 Proj proj,
                 const stage_factor_matrix_t& factor = {})
{
  if (ht.empty()) {
    return std::pair {std::vector<double> {}, std::vector<bool> {}};
  }
  const auto size = h.active_stage_count();
  std::vector<double> values(size);
  std::vector<bool> valid(size, false);
  bool need_values = false;
  bool need_valids = false;
  size_t idx = 0;
  size_t count = 0;
  for (size_t ti = 0; ti < h.active_stages().size(); ++ti) {
    const auto it = ht.find(h.active_stages()[ti]);
    if (it != ht.end()) {
      const auto value = proj(it->second);
      values[idx] = factor.empty() ? value : value * factor[ti];
      valid[idx] = true;
      ++count;
      need_values = true;
    }
    need_valids |= count != ++idx;
  }
  return std::pair {need_values ? std::move(values) : std::vector<double> {},
                    need_valids ? std::move(valid) : std::vector<bool> {}};
}

// Structural equality: same sizes, same valid mask, same values slot by
// slot (values produced by identical arithmetic on both paths).
void check_flat_equal(
    const std::pair<std::vector<double>, std::vector<bool>>& got,
    const std::pair<std::vector<double>, std::vector<bool>>& expected)
{
  const auto& [gv, gm] = got;
  const auto& [ev, em] = expected;
  REQUIRE(gv.size() == ev.size());
  REQUIRE(gm.size() == em.size());
  CHECK(gm == em);
  for (size_t i = 0; i < gv.size(); ++i) {
    CHECK(gv[i] == doctest::Approx(ev[i]));
  }
}

}  // namespace

TEST_CASE("FlatHelper - indexed sparse fill matches dense reference")  // NOLINT
{
  const PlanningOptionsLP options;
  // 2 scenarios (non-contiguous uids), 2 active stages + 1 inactive stage,
  // uneven block counts — the layout the slot index maps must reproduce.
  const Simulation psimulation = {
      .block_array =
          {
              {.uid = Uid {10}, .duration = 1},
              {.uid = Uid {20}, .duration = 2},
              {.uid = Uid {30}, .duration = 3},
              {.uid = Uid {40}, .duration = 1},
          },
      .stage_array =
          {
              {.uid = Uid {5}, .first_block = 0, .count_block = 2},
              {.uid = Uid {7}, .first_block = 2, .count_block = 1},
              {
                  .uid = Uid {9},
                  .active = 0,
                  .first_block = 3,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {.uid = Uid {3}},
              {.uid = Uid {8}},
          },
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);
  const auto id = [](auto v) { return static_cast<double>(v); };

  block_factor_matrix_t bfactor(2, 2);
  bfactor[0][0] = {2.0, 3.0};
  bfactor[0][1] = {4.0};
  bfactor[1][0] = {0.5, 0.25};
  bfactor[1][1] = {10.0};

  SUBCASE("GSTB - stray keys, partial blocks, with and without factor")
  {
    GSTBIndexHolder holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(10)}] =
        100;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(30)}] =
        300;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5), make_uid<Block>(20)}] =
        500;
    // Stray keys that the dense scan never visits: inactive stage 9,
    // unknown scenario 99, block 30 filed under the wrong stage 5.
    holder[{make_uid<Scenario>(3), make_uid<Stage>(9), make_uid<Block>(40)}] =
        901;
    holder[{make_uid<Scenario>(99), make_uid<Stage>(5), make_uid<Block>(10)}] =
        902;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(30)}] =
        903;

    check_flat_equal(helper.flat(holder, id),
                     dense_ref_gstb(helper, holder, id));
    check_flat_equal(helper.flat(holder, id, bfactor),
                     dense_ref_gstb(helper, holder, id, bfactor));
  }

  SUBCASE("GSTB - full grid coverage elides the valid vector")
  {
    GSTBIndexHolder holder;
    Index v = 0;
    for (auto suid : {3, 8}) {
      for (const auto& [tuid, buids] :
           {
               std::pair {5, std::vector {10, 20}},
               std::pair {7, std::vector {30}},
           })
      {
        for (auto buid : buids) {
          holder[{make_uid<Scenario>(suid),
                  make_uid<Stage>(tuid),
                  make_uid<Block>(buid)}] = ++v;
        }
      }
    }
    auto got = helper.flat(holder, id);
    check_flat_equal(got, dense_ref_gstb(helper, holder, id));
    CHECK(got.second.empty());  // all slots hit → no valid mask
  }

  SUBCASE("GSTB - only stray keys: empty values, full all-false valid")
  {
    GSTBIndexHolder holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(9), make_uid<Block>(40)}] =
        901;
    auto got = helper.flat(holder, id);
    check_flat_equal(got, dense_ref_gstb(helper, holder, id));
    CHECK(got.first.empty());  // no hit → values elided
    CHECK(got.second.size() == 6);  // but mask spans the grid, all false
  }

  SUBCASE("STB - partial inner maps, stray keys, factor and st_scale")
  {
    STBIndexHolder<Index> holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5)}] = {
        {make_uid<Block>(10), 100},
        {make_uid<Block>(30), 903},  // wrong-stage block, skipped
    };
    holder[{make_uid<Scenario>(8), make_uid<Stage>(7)}] = {
        {make_uid<Block>(30), 600},
    };
    holder[{make_uid<Scenario>(8), make_uid<Stage>(9)}] = {
        {make_uid<Block>(40), 904},  // inactive stage, skipped
    };
    holder[{make_uid<Scenario>(99), make_uid<Stage>(5)}] = {
        {make_uid<Block>(10), 905},  // unknown scenario, skipped
    };

    check_flat_equal(helper.flat(holder, id),
                     dense_ref_stb(helper, holder, id));
    check_flat_equal(helper.flat(holder, id, bfactor),
                     dense_ref_stb(helper, holder, id, bfactor));

    STIndexHolder<double> st_scale;
    st_scale[{make_uid<Scenario>(3), make_uid<Stage>(5)}] = 2.5;
    check_flat_equal(helper.flat(holder, id, bfactor, st_scale),
                     dense_ref_stb(helper, holder, id, bfactor, st_scale));
  }

  SUBCASE("ST - stray keys and factor")
  {
    STIndexHolder<Index> holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7)}] = 200;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5)}] = 300;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(9)}] = 906;  // inactive
    holder[{make_uid<Scenario>(99), make_uid<Stage>(5)}] = 907;  // unknown

    scenario_stage_factor_matrix_t factor(2, 2);
    factor[0][0] = 2.0;
    factor[0][1] = 3.0;
    factor[1][0] = 0.5;
    factor[1][1] = 0.25;

    check_flat_equal(helper.flat(holder, id), dense_ref_st(helper, holder, id));
    check_flat_equal(helper.flat(holder, id, factor),
                     dense_ref_st(helper, holder, id, factor));
  }

  SUBCASE("T - stray keys and factor")
  {
    TIndexHolder holder;
    holder[make_uid<Stage>(7)] = 200;
    holder[make_uid<Stage>(9)] = 908;  // inactive stage, skipped

    const stage_factor_matrix_t factor = {2.0, 3.0};

    check_flat_equal(helper.flat(holder, id), dense_ref_t(helper, holder, id));
    check_flat_equal(helper.flat(holder, id, factor),
                     dense_ref_t(helper, holder, id, factor));
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
  holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(10)}] =
      100;
  // make_uid<Block>(20) is missing → sparse

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

// ---------------------------------------------------------------------------
// flat_sparse ⇔ dense flat long-form emission equivalence pins (P1)
// ---------------------------------------------------------------------------
//
// The write-out path no longer materialises the dense flat() vectors:
// OutputContext::add_field* collects flat_sparse() (grid-slot, value)
// entries directly and the long-form writer labels each sample via the
// slot index.  These pins fix the sparse contract against the dense
// flat() reference: the entries are exactly the dense vector's hit
// slots, in ascending slot order (the dense scan's row order), with
// identical values — for every holder kind, with and without factors,
// including explicit zero values (kept by flat_sparse, dropped later by
// the writer after any integer snapping) and regardless of the holder's
// key order relative to the active slot order.

namespace
{

// Project the dense flat() result back to (slot, value) pairs over the
// hit slots — the exact row set + order the historical wide→long
// conversion iterated (its zero-value drop applies identically to both
// representations at write time).
auto dense_to_entries(
    const std::pair<std::vector<double>, std::vector<bool>>& dense)
    -> SparseEntries
{
  const auto& [values, valid] = dense;
  SparseEntries out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (valid.empty() || valid[i]) {
      out.emplace_back(static_cast<uint32_t>(i), values[i]);
    }
  }
  return out;
}

void check_entries_equal(const SparseEntries& got,
                         const SparseEntries& expected)
{
  REQUIRE(got.size() == expected.size());
  CHECK(std::ranges::is_sorted(got, {}, &SparseEntry::first));
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].first == expected[i].first);
    CHECK(got[i].second == doctest::Approx(expected[i].second));
  }
}

}  // namespace

TEST_CASE("FlatHelper - flat_sparse matches dense flat reference")  // NOLINT
{
  const PlanningOptionsLP options;

  // Same layout as the dense-reference pin above: 2 scenarios, 2 active
  // stages + 1 inactive, uneven block counts.  The scenario_array order
  // is reversed in the second subcase so the ACTIVE scenario order
  // differs from the holder's uid-sorted key order — pinning that
  // flat_sparse orders entries by grid slot, not by holder key.
  bool reversed_scenarios = false;
  SUBCASE("active scenario order == uid order")
  {
    reversed_scenarios = false;
  }
  SUBCASE("active scenario order reversed vs uid order")
  {
    reversed_scenarios = true;
  }

  Simulation psimulation = {
      .block_array =
          {
              {.uid = Uid {10}, .duration = 1},
              {.uid = Uid {20}, .duration = 2},
              {.uid = Uid {30}, .duration = 3},
              {.uid = Uid {40}, .duration = 1},
          },
      .stage_array =
          {
              {.uid = Uid {5}, .first_block = 0, .count_block = 2},
              {.uid = Uid {7}, .first_block = 2, .count_block = 1},
              {
                  .uid = Uid {9},
                  .active = 0,
                  .first_block = 3,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {.uid = Uid {3}},
              {.uid = Uid {8}},
          },
  };
  if (reversed_scenarios) {
    std::ranges::reverse(psimulation.scenario_array);
  }

  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);
  const auto id = [](auto v) { return static_cast<double>(v); };

  block_factor_matrix_t bfactor(2, 2);
  bfactor[0][0] = {2.0, 3.0};
  bfactor[0][1] = {4.0};
  bfactor[1][0] = {0.5, 0.25};
  bfactor[1][1] = {10.0};

  // GSTB: stray keys, partial grid, and an explicit ZERO value (kept as
  // an entry — the long-form writer drops zeros at emission time).
  {
    GSTBIndexHolder holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(10)}] =
        100;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(30)}] =
        0;  // explicit zero
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5), make_uid<Block>(20)}] =
        500;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(9), make_uid<Block>(40)}] =
        901;  // inactive stage
    holder[{make_uid<Scenario>(99), make_uid<Stage>(5), make_uid<Block>(10)}] =
        902;  // unknown scenario
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5), make_uid<Block>(30)}] =
        903;  // block filed under the wrong stage

    check_entries_equal(helper.flat_sparse(holder, id),
                        dense_to_entries(helper.flat(holder, id)));
    check_entries_equal(helper.flat_sparse(holder, id, bfactor),
                        dense_to_entries(helper.flat(holder, id, bfactor)));
  }

  // GSTB: full grid coverage (dense elides the valid mask → every slot
  // is a row; the sparse form must carry all of them, slot-ascending).
  {
    GSTBIndexHolder holder;
    Index v = 0;
    for (auto suid : {3, 8}) {
      for (const auto& [tuid, buids] :
           {
               std::pair {5, std::vector {10, 20}},
               std::pair {7, std::vector {30}},
           })
      {
        for (auto buid : buids) {
          holder[{make_uid<Scenario>(suid),
                  make_uid<Stage>(tuid),
                  make_uid<Block>(buid)}] = ++v;
        }
      }
    }
    const auto sparse = helper.flat_sparse(holder, id);
    CHECK(sparse.size() == 6);
    check_entries_equal(sparse, dense_to_entries(helper.flat(holder, id)));
  }

  // GSTB: only stray keys → no entries at all (dense: elided values +
  // all-false mask; long form: zero rows).
  {
    GSTBIndexHolder holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(9), make_uid<Block>(40)}] =
        901;
    CHECK(helper.flat_sparse(holder, id).empty());
  }

  // STB: partial inner maps, stray keys, factor and st_scale variants.
  {
    STBIndexHolder<Index> holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(5)}] = {
        {make_uid<Block>(10), 100},
        {make_uid<Block>(30), 903},  // wrong-stage block, skipped
    };
    holder[{make_uid<Scenario>(8), make_uid<Stage>(7)}] = {
        {make_uid<Block>(30), 600},
    };
    holder[{make_uid<Scenario>(8), make_uid<Stage>(9)}] = {
        {make_uid<Block>(40), 904},  // inactive stage, skipped
    };
    holder[{make_uid<Scenario>(99), make_uid<Stage>(5)}] = {
        {make_uid<Block>(10), 905},  // unknown scenario, skipped
    };

    check_entries_equal(helper.flat_sparse(holder, id),
                        dense_to_entries(helper.flat(holder, id)));
    check_entries_equal(helper.flat_sparse(holder, id, bfactor),
                        dense_to_entries(helper.flat(holder, id, bfactor)));

    STIndexHolder<double> st_scale;
    st_scale[{make_uid<Scenario>(3), make_uid<Stage>(5)}] = 2.5;
    check_entries_equal(
        helper.flat_sparse(holder, id, bfactor, st_scale),
        dense_to_entries(helper.flat(holder, id, bfactor, st_scale)));
  }

  // ST: stray keys and factor.
  {
    STIndexHolder<Index> holder;
    holder[{make_uid<Scenario>(3), make_uid<Stage>(7)}] = 200;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(5)}] = 300;
    holder[{make_uid<Scenario>(8), make_uid<Stage>(9)}] = 906;  // inactive
    holder[{make_uid<Scenario>(99), make_uid<Stage>(5)}] = 907;  // unknown

    scenario_stage_factor_matrix_t factor(2, 2);
    factor[0][0] = 2.0;
    factor[0][1] = 3.0;
    factor[1][0] = 0.5;
    factor[1][1] = 0.25;

    check_entries_equal(helper.flat_sparse(holder, id),
                        dense_to_entries(helper.flat(holder, id)));
    check_entries_equal(helper.flat_sparse(holder, id, factor),
                        dense_to_entries(helper.flat(holder, id, factor)));
  }

  // T: stray keys and factor.
  {
    TIndexHolder holder;
    holder[make_uid<Stage>(7)] = 200;
    holder[make_uid<Stage>(9)] = 908;  // inactive stage, skipped

    const stage_factor_matrix_t factor = {2.0, 3.0};

    check_entries_equal(helper.flat_sparse(holder, id),
                        dense_to_entries(helper.flat(holder, id)));
    check_entries_equal(helper.flat_sparse(holder, id, factor),
                        dense_to_entries(helper.flat(holder, id, factor)));
  }
}
