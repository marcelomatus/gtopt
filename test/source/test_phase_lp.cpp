/**
 * @file test_phase_lp.cpp
 * @brief Unit tests for PhaseLP class
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <gtopt/phase_lp.hpp>
#include <gtopt/basic_types.hpp>
#include <vector>

using namespace gtopt;

TEST_CASE("PhaseLP construction")
{
    SUBCASE("Default construction")
    {
        PhaseLP phase;
        CHECK(phase.duration() == 0.0);
        CHECK(phase.index() == unknown_index);
        CHECK(phase.stages().empty());
    }

    SUBCASE("Construction with stages")
    {
        OptionsLP options;
        std::vector<Stage> stages = {
            Stage{.uid = "s1", .first_block = 0, .count_block = 2, .active = true},
            Stage{.uid = "s2", .first_block = 2, .count_block = 3, .active = true}
        };
        
        std::vector<Block> blocks = {
            Block{.duration = 1.0},
            Block{.duration = 2.0},  // stage 1 blocks
            Block{.duration = 3.0},
            Block{.duration = 4.0},
            Block{.duration = 5.0}   // stage 2 blocks
        };

        Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 2};
        PhaseLP phase_lp(phase, options, stages, blocks, PhaseIndex{1});

        CHECK(phase_lp.uid().value == "p1");
        CHECK(phase_lp.index() == 1);
        CHECK(phase_lp.stages().size() == 2);
    }
}

TEST_CASE("PhaseLP duration calculation")
{
    OptionsLP options;
    std::vector<Stage> stages = {
        Stage{.uid = "s1", .first_block = 0, .count_block = 2, .active = true},
        Stage{.uid = "s2", .first_block = 2, .count_block = 3, .active = true}
    };
    
    std::vector<Block> blocks = {
        Block{.duration = 1.0},
        Block{.duration = 2.0},  // stage 1 total: 3.0
        Block{.duration = 3.0},
        Block{.duration = 4.0},
        Block{.duration = 5.0}   // stage 2 total: 12.0
    };

    Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 2};
    PhaseLP phase_lp(phase, options, stages, blocks);

    // Total duration should be sum of all stage durations (3.0 + 12.0)
    CHECK(phase_lp.duration() == doctest::Approx(15.0));
}

TEST_CASE("PhaseLP stage access")
{
    OptionsLP options;
    std::vector<Stage> stages = {
        Stage{.uid = "s1", .first_block = 0, .count_block = 1, .active = true},
        Stage{.uid = "s2", .first_block = 1, .count_block = 1, .active = true},
        Stage{.uid = "s3", .first_block = 2, .count_block = 1, .active = false}
    };
    
    std::vector<Block> blocks = {
        Block{.duration = 1.0},
        Block{.duration = 2.0},
        Block{.duration = 3.0}
    };

    SUBCASE("Full phase")
    {
        Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 3};
        PhaseLP phase_lp(phase, options, stages, blocks);

        CHECK(phase_lp.stages().size() == 2); // Only active stages
        CHECK(phase_lp.first_stage() == 0);
        CHECK(phase_lp.count_stage() == 3);
    }

    SUBCASE("Partial phase")
    {
        Phase phase{.uid = "p1", .first_stage = 1, .count_stage = 2};
        PhaseLP phase_lp(phase, options, stages, blocks);

        CHECK(phase_lp.stages().size() == 1); // Only one active stage in range
        CHECK(phase_lp.first_stage() == 1);
        CHECK(phase_lp.count_stage() == 2);
    }
}

TEST_CASE("PhaseLP active status")
{
    OptionsLP options;
    std::vector<Stage> stages = {
        Stage{.uid = "s1", .first_block = 0, .count_block = 1, .active = true}
    };
    std::vector<Block> blocks = {Block{.duration = 1.0}};

    SUBCASE("Active phase")
    {
        Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 1, .active = true};
        PhaseLP phase_lp(phase, options, stages, blocks);
        CHECK(phase_lp.is_active());
    }

    SUBCASE("Inactive phase")
    {
        Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 1, .active = false};
        PhaseLP phase_lp(phase, options, stages, blocks);
        CHECK_FALSE(phase_lp.is_active());
    }

    SUBCASE("Default active status")
    {
        Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 1};
        PhaseLP phase_lp(phase, options, stages, blocks);
        CHECK(phase_lp.is_active()); // Default should be active
    }
}

TEST_CASE("PhaseLP construction from simulation")
{
    OptionsLP options;
    Simulation simulation;
    simulation.stage_array = {
        Stage{.uid = "s1", .first_block = 0, .count_block = 1, .active = true}
    };
    simulation.block_array = {Block{.duration = 1.0}};

    Phase phase{.uid = "p1", .first_stage = 0, .count_stage = 1};
    PhaseLP phase_lp(phase, options, simulation);

    CHECK(phase_lp.stages().size() == 1);
    CHECK(phase_lp.duration() == doctest::Approx(1.0));
}
