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
        const PhaseLP phase;
        CHECK(phase.duration() == 0.0);
        CHECK(phase.index() == unknown_index);
        CHECK(phase.stages().empty());
    }

    SUBCASE("Construction with stages")
    {
        const OptionsLP options;
        const std::vector<Stage> stages = {
            Stage{.uid = 1, .first_block = 0, .count_block = 2, .active = true, .discount_factor = std::nullopt},
            Stage{.uid = 2, .first_block = 2, .count_block = 3, .active = true, .discount_factor = std::nullopt}
        };
        
        std::vector<Block> blocks = {
            Block{.duration = 1.0, .uid = 0},
            Block{.duration = 2.0, .uid = 0},  // stage 1 blocks
            Block{.duration = 3.0, .uid = 0},
            Block{.duration = 4.0, .uid = 0},
            Block{.duration = 5.0, .uid = 0}   // stage 2 blocks
        };

        const Phase phase{
            .uid = 1,
            .first_stage = 0,
            .count_stage = 2,
            .active = true,
            .discount_factor = std::nullopt
        };
        const PhaseLP phase_lp(phase, options, stages, blocks, PhaseIndex{1});

        CHECK(phase_lp.uid() == PhaseUid{1});
        CHECK(phase_lp.index() == 1);
        CHECK(phase_lp.stages().size() == 2);
    }
}

TEST_CASE("PhaseLP duration calculation")
{
    const OptionsLP options;
    std::vector<Stage> stages = {
        Stage{.uid = 1, .first_block = 0, .count_block = 2, .active = true, .discount_factor = std::nullopt},
        Stage{.uid = 2, .first_block = 2, .count_block = 3, .active = true, .discount_factor = std::nullopt}
    };
    
    std::vector<Block> blocks = {
        Block{.duration = 1.0, .uid = 0},
        Block{.duration = 2.0, .uid = 0},  // stage 1 total: 3.0
        Block{.duration = 3.0, .uid = 0},
        Block{.duration = 4.0, .uid = 0},
        Block{.duration = 5.0, .uid = 0}   // stage 2 total: 12.0
    };

    Phase phase{.uid = 1, .first_stage = 0, .count_stage = 2};
    const PhaseLP phase_lp(phase, options, stages, blocks);

    // Total duration should be sum of all stage durations (3.0 + 12.0)
    CHECK(phase_lp.duration() == doctest::Approx(15.0));
}

TEST_CASE("PhaseLP stage access")
{
    const OptionsLP options;
    std::vector<Stage> stages = {
        Stage{.uid = 1, .first_block = 0, .count_block = 1, .active = true, .discount_factor = std::nullopt},
        Stage{.uid = 2, .first_block = 1, .count_block = 1, .active = true, .discount_factor = std::nullopt},
        Stage{.uid = 3, .first_block = 2, .count_block = 1, .active = false, .discount_factor = std::nullopt}
    };
    
    std::vector<Block> blocks = {
        Block{.duration = 1.0, .uid = 0},
        Block{.duration = 2.0, .uid = 0},
        Block{.duration = 3.0, .uid = 0}
    };

    SUBCASE("Full phase")
    {
        Phase phase{.uid = 1, .first_stage = 0, .count_stage = 3};
        const PhaseLP phase_lp(phase, options, stages, blocks);

        CHECK(phase_lp.stages().size() == 2); // Only active stages
        CHECK(phase_lp.first_stage() == 0);
        CHECK(phase_lp.count_stage() == 3);
    }

    SUBCASE("Partial phase")
    {
        Phase phase{.uid = 1, .first_stage = 1, .count_stage = 2};
        const PhaseLP phase_lp(phase, options, stages, blocks);

        CHECK(phase_lp.stages().size() == 1); // Only one active stage in range
        CHECK(phase_lp.first_stage() == 1);
        CHECK(phase_lp.count_stage() == 2);
    }
}

TEST_CASE("PhaseLP active status")
{
    const OptionsLP options;
    std::vector<Stage> stages = {
        Stage{.uid = 1, .first_block = 0, .count_block = 1, .active = true, .discount_factor = std::nullopt}
    };
    std::vector<Block> blocks = {Block{.duration = 1.0}};

    SUBCASE("Active phase")
    {
        Phase phase{.uid = 1, .first_stage = 0, .count_stage = 1, .active = true};
        const PhaseLP phase_lp(phase, options, stages, blocks);
        CHECK(phase_lp.is_active());
    }

    SUBCASE("Inactive phase")
    {
        Phase phase{.uid = 1, .first_stage = 0, .count_stage = 1, .active = false};
        const PhaseLP phase_lp(phase, options, stages, blocks);
        CHECK_FALSE(phase_lp.is_active());
    }

    SUBCASE("Default active status")
    {
        const Phase phase{.uid = 1, .first_stage = 0, .count_stage = 1, .active = true};
        const PhaseLP phase_lp(phase, options, stages, blocks);
        CHECK(phase_lp.is_active()); // Default should be active
    }
}

TEST_CASE("PhaseLP construction from simulation")
{
    const OptionsLP options;
    Simulation simulation;
    simulation.stage_array = {
        Stage{.uid = 1, .first_block = 0, .count_block = 1, .active = true, .discount_factor = std::nullopt}
    };
    simulation.block_array = {Block{.duration = 1.0, .uid = 0}};

    Phase phase{.uid = 1, .first_stage = 0, .count_stage = 1};
    const PhaseLP phase_lp(phase, options, simulation);

    CHECK(phase_lp.stages().size() == 1);
    CHECK(phase_lp.duration() == doctest::Approx(1.0));
}
