/**
 * @file test_phase_lp.cpp
 * @brief Unit tests for PhaseLP class
 */

#include <doctest/doctest.h>
#include <gtopt/phase_lp.hpp>

using namespace gtopt;

TEST_CASE("PhaseLP construction")
{
  SUBCASE("Default construction")
  {
    const PhaseLP phase;
    CHECK(phase.duration() == 0.0);
    CHECK(phase.index() == unknown_index);
    CHECK(phase.stages().empty());
  }

  SUBCASE("Construction with stages")
  {
    const OptionsLP options;
    const std::vector<Stage> stages = {Stage {.uid = 1,
                                              .active = true,
                                              .first_block = 0,
                                              .count_block = 2,
                                              .discount_factor = std::nullopt},
                                       Stage {.uid = 2,
                                              .active = true,
                                              .first_block = 2,
                                              .count_block = 3,
                                              .discount_factor = std::nullopt}};
    std::vector<Block> blocks = {
        Block {.uid = 0, .duration = 1.0},
        Block {.uid = 0, .duration = 2.0},  // stage 1 blocks
        Block {.uid = 0, .duration = 3.0},
        Block {.uid = 0, .duration = 4.0},
        Block {.uid = 0, .duration = 5.0}  // stage 2 blocks
    };

    const Phase phase {
        .uid = 1, .active = true, .first_stage = 0, .count_stage = 2};
    const PhaseLP phase_lp(phase, options, stages, blocks, PhaseIndex {1});

    CHECK(phase_lp.uid() == PhaseUid {1});
    CHECK(phase_lp.index() == 1);
    CHECK(phase_lp.stages().size() == 2);
  }
}

TEST_CASE("PhaseLP duration calculation")
{
  const OptionsLP options;
  std::vector<Stage> stages = {Stage {.uid = 1,
                                      .active = true,
                                      .first_block = 0,
                                      .count_block = 2,
                                      .discount_factor = std::nullopt},
                               Stage {.uid = 2,
                                      .active = true,
                                      .first_block = 2,
                                      .count_block = 3,
                                      .discount_factor = std::nullopt}};

  std::vector<Block> blocks = {
      Block {.duration = 1.0},
      Block {.duration = 2.0},  // stage 1 total: 3.0
      Block {.duration = 3.0},
      Block {.duration = 4.0},
      Block {.duration = 5.0}  // stage 2 total: 12.0
  };

  const Phase phase {.uid = 1, .first_stage = 0, .count_stage = 2};
  const PhaseLP phase_lp(phase, options, stages, blocks);

  // Total duration should be sum of all stage durations (3.0 + 12.0)
  CHECK(phase_lp.duration() == doctest::Approx(15.0));
}

TEST_CASE("PhaseLP stage access")
{
  const OptionsLP options;
  std::vector<Stage> stages = {Stage {.uid = 1,
                                      .active = true,
                                      .first_block = 0,
                                      .count_block = 1,
                                      .discount_factor = std::nullopt},
                               Stage {.uid = 2,
                                      .active = true,
                                      .first_block = 1,
                                      .count_block = 1,
                                      .discount_factor = std::nullopt},
                               Stage {.uid = 3,
                                      .active = false,
                                      .first_block = 2,
                                      .count_block = 1,
                                      .discount_factor = std::nullopt}};

  std::vector<Block> blocks = {Block {.duration = 1.0},
                               Block {.duration = 2.0},
                               Block {.duration = 3.0}};

  SUBCASE("Full phase")
  {
    const Phase phase {.uid = 1, .first_stage = 0, .count_stage = 3};
    const PhaseLP phase_lp(phase, options, stages, blocks);

    CHECK(phase_lp.stages().size() == 2);  // Only active stages
    CHECK(phase_lp.first_stage() == 0);
    CHECK(phase_lp.count_stage() == 3);
  }

  SUBCASE("Partial phase")
  {
    const Phase phase {.uid = 1, .first_stage = 1, .count_stage = 2};
    const PhaseLP phase_lp(phase, options, stages, blocks);

    CHECK(phase_lp.stages().size() == 1);  // Only one active stage in range
    CHECK(phase_lp.first_stage() == 1);
    CHECK(phase_lp.count_stage() == 2);
  }
}

TEST_CASE("PhaseLP active status")
{
  const OptionsLP options;
  std::vector<Stage> stages = {Stage {.uid = 1,
                                      .active = true,
                                      .first_block = 0,
                                      .count_block = 1,
                                      .discount_factor = std::nullopt}};
  std::vector<Block> blocks = {Block {.duration = 1.0}};

  SUBCASE("Active phase")
  {
    const Phase phase {
        .uid = 1, .active = true, .first_stage = 0, .count_stage = 1};
    const PhaseLP phase_lp(phase, options, stages, blocks);
    CHECK(phase_lp.is_active());
  }

  SUBCASE("Inactive phase")
  {
    const Phase phase {
        .uid = 1, .active = false, .first_stage = 0, .count_stage = 1};
    const PhaseLP phase_lp(phase, options, stages, blocks);
    CHECK_FALSE(phase_lp.is_active());
  }

  SUBCASE("Default active status")
  {
    const Phase phase {
        .uid = 1, .active = true, .first_stage = 0, .count_stage = 1};
    const PhaseLP phase_lp(phase, options, stages, blocks);
    CHECK(phase_lp.is_active());  // Default should be active
  }
}

TEST_CASE("PhaseLP construction from simulation")
{
  const OptionsLP options;
  Simulation simulation;
  simulation.stage_array = {Stage {.uid = 1,
                                   .active = true,
                                   .first_block = 0,
                                   .count_block = 1,
                                   .discount_factor = std::nullopt}};
  simulation.block_array = {Block {.duration = 1.0}};

  const Phase phase {.uid = 1, .first_stage = 0, .count_stage = 1};
  const PhaseLP phase_lp(phase, options, simulation);

  CHECK(phase_lp.stages().size() == 1);
  CHECK(phase_lp.duration() == doctest::Approx(1.0));
}
