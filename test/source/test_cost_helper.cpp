#include <doctest/doctest.h>
#include <gtopt/cost_helper.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/block_lp.hpp>

using namespace gtopt;

TEST_SUITE("CostHelper") {
    TEST_CASE("Construction and basic properties") {
        const OptionsLP options;
        const std::vector<ScenarioLP> scenarios;
        const std::vector<StageLP> stages;
        
        const CostHelper helper(options, scenarios, stages);
        
        SUBCASE("Empty construction works") {
            CHECK_NOTHROW(CostHelper(options, scenarios, stages));
        }
        
        SUBCASE("Basic functionality") {
            CHECK(helper.stage_cost(StageIndex{0}, 100.0) > 0);
        }
    }

    TEST_CASE("stage_factors calculation") {
        SUBCASE("Single stage") {
            std::vector<StageLP> stages(1);
            stages[0].discount_factor = 0.9;
            
            auto factors = detail::stage_factors(stages);
            REQUIRE(factors.size() == 1);
            CHECK(factors[0] == doctest::Approx(0.9));
        }

        SUBCASE("Multiple stages") {
            std::vector<StageLP> stages(3);
            stages[0].discount_factor = 0.9;
            stages[1].discount_factor = 0.8;
            stages[2].discount_factor = 0.7;
            
            auto factors = detail::stage_factors(stages);
            REQUIRE(factors.size() == 3);
            CHECK(factors[0] == doctest::Approx(0.9));
            CHECK(factors[1] == doctest::Approx(0.72)); // 0.9 * 0.8
            CHECK(factors[2] == doctest::Approx(0.504)); // 0.9 * 0.8 * 0.7
        }
    }

    TEST_CASE("block_cost calculation") {
        const OptionsLP options;
        std::vector<ScenarioLP> scenarios(1);
        std::vector<StageLP> stages(1);
        stages[0].discount_factor = 0.9;
        scenarios[0].probability_factor = 0.5;
        
        const BlockLP block;
        const CostHelper helper(options, scenarios, stages);
        
        SUBCASE("Basic cost calculation") {
            const double cost = 100.0;
            const double expected = 100.0 * 0.9 * 0.5;
            CHECK(helper.block_cost(ScenarioIndex{0}, StageIndex{0}, block, cost) 
                == doctest::Approx(expected));
        }

        SUBCASE("Zero cost") {
            CHECK(helper.block_cost(ScenarioIndex{0}, StageIndex{0}, block, 0.0) 
                == doctest::Approx(0.0));
        }
    }

    TEST_CASE("stage_cost calculation") {
        const OptionsLP options;
        const std::vector<ScenarioLP> scenarios;
        std::vector<StageLP> stages(2);
        stages[0].discount_factor = 0.9;
        stages[1].discount_factor = 0.8;
        
        const CostHelper helper(options, scenarios, stages);
        
        SUBCASE("First stage") {
            const double cost = 100.0;
            CHECK(helper.stage_cost(StageIndex{0}, cost) 
                == doctest::Approx(100.0 * 0.9));
        }

        SUBCASE("Second stage") {
            const double cost = 100.0;
            CHECK(helper.stage_cost(StageIndex{1}, cost) 
                == doctest::Approx(100.0 * 0.9 * 0.8));
        }
    }

    TEST_CASE("scenario_stage_cost calculation") {
        const OptionsLP options;
        std::vector<ScenarioLP> scenarios(2);
        scenarios[0].probability_factor = 0.6;
        scenarios[1].probability_factor = 0.4;
        std::vector<StageLP> stages(2);
        stages[0].discount_factor = 0.9;
        stages[1].discount_factor = 0.8;
        
        const CostHelper helper(options, scenarios, stages);
        
        SUBCASE("First scenario, first stage") {
            const double cost = 100.0;
            CHECK(helper.scenario_stage_cost(ScenarioIndex{0}, StageIndex{0}, cost) 
                == doctest::Approx(100.0 * 0.9 * 0.6));
        }

        SUBCASE("Second scenario, second stage") {
            const double cost = 100.0;
            CHECK(helper.scenario_stage_cost(ScenarioIndex{1}, StageIndex{1}, cost) 
                == doctest::Approx(100.0 * 0.9 * 0.8 * 0.4));
        }
    }

    TEST_CASE("Factor matrix generation") {
        const OptionsLP options;
        std::vector<ScenarioLP> scenarios(2);
        scenarios[0].probability_factor = 0.6;
        scenarios[1].probability_factor = 0.4;
        std::vector<StageLP> stages(2);
        stages[0].discount_factor = 0.9;
        stages[1].discount_factor = 0.8;
        
        CostHelper helper(options, scenarios, stages);
        
        SUBCASE("block_cost_factors") {
            auto factors = helper.block_cost_factors();
            REQUIRE(factors.size() == 2); // scenarios
            REQUIRE(factors[0].size() == 2); // stages
            // Just verify the structure - exact values depend on BlockLP
        }

        SUBCASE("stage_cost_factors") {
            auto factors = helper.stage_cost_factors();
            REQUIRE(factors.size() == 2); // stages
            CHECK(factors[0] == doctest::Approx(0.9));
            CHECK(factors[1] == doctest::Approx(0.9 * 0.8));
        }

        SUBCASE("scenario_stage_cost_factors") {
            auto factors = helper.scenario_stage_cost_factors();
            REQUIRE(factors.size() == 2); // scenarios
            REQUIRE(factors[0].size() == 2); // stages
            CHECK(factors[0][0] == doctest::Approx(0.9 * 0.6));
            CHECK(factors[1][1] == doctest::Approx(0.9 * 0.8 * 0.4));
        }
    }
}
