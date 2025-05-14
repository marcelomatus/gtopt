#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/block_lp.hpp>

namespace gtopt {

TEST_SUITE("FlatHelper Tests") {
    TEST_CASE("Active Elements Accessors") {
        std::vector<ScenarioIndex> active_scenarios = {ScenarioIndex{0}};
        std::vector<StageIndex> active_stages = {StageIndex{0}};
        std::vector<std::vector<BlockIndex>> active_stage_blocks = {{BlockIndex{0}, BlockIndex{1}}};
        std::vector<BlockIndex> active_blocks = {BlockIndex{0}, BlockIndex{1}};

        FlatHelper helper(active_scenarios, active_stages, active_stage_blocks, active_blocks);

        SUBCASE("Scenario accessors") {
            CHECK(helper.active_scenarios().size() == 1);
            CHECK(helper.active_scenario_count() == 1);
            CHECK(helper.is_first_scenario(ScenarioIndex{0}));
        }

        SUBCASE("Stage accessors") {
            CHECK(helper.active_stages().size() == 1);
            CHECK(helper.active_stage_count() == 1);
            CHECK(helper.is_first_stage(StageIndex{0}));
            CHECK(helper.is_last_stage(StageIndex{0}));
        }

        SUBCASE("Block accessors") {
            CHECK(helper.active_blocks().size() == 2);
            CHECK(helper.active_block_count() == 2);
            CHECK(helper.active_stage_blocks().size() == 1);
            CHECK(helper.active_stage_blocks()[0].size() == 2);
        }
    }

    TEST_CASE("Flat Methods") {
        std::vector<ScenarioIndex> active_scenarios = {ScenarioIndex{0}};
        std::vector<StageIndex> active_stages = {StageIndex{0}};
        std::vector<std::vector<BlockIndex>> active_stage_blocks = {{BlockIndex{0}, BlockIndex{1}}};
        std::vector<BlockIndex> active_blocks = {BlockIndex{0}, BlockIndex{1}};

        FlatHelper helper(active_scenarios, active_stages, active_stage_blocks, active_blocks);

        SUBCASE("GSTBIndexHolder") {
            GSTBIndexHolder holder;
            holder[{ScenarioIndex{0}, StageIndex{0}, BlockIndex{0}}] = 10.0;
            holder[{ScenarioIndex{0}, StageIndex{0}, BlockIndex{1}}] = 20.0;

            auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

            REQUIRE(values.size() == 2);
            REQUIRE(valid.size() == 2);
            CHECK(values[0] == doctest::Approx(10.0));
            CHECK(values[1] == doctest::Approx(20.0));
            CHECK(valid[0] == true);
            CHECK(valid[1] == true);
        }

        SUBCASE("STBIndexHolder") {
            STBIndexHolder holder;
            holder[{ScenarioIndex{0}, StageIndex{0}}] = {
                {BlockIndex{0}, 10.0},
                {BlockIndex{1}, 20.0}
            };

            auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

            REQUIRE(values.size() == 2);
            REQUIRE(valid.size() == 2);
            CHECK(values[0] == doctest::Approx(10.0));
            CHECK(values[1] == doctest::Approx(20.0));
            CHECK(valid[0] == true);
            CHECK(valid[1] == true);
        }

        SUBCASE("STIndexHolder") {
            STIndexHolder holder;
            holder[{ScenarioIndex{0}, StageIndex{0}}] = 42.0;

            auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

            REQUIRE(values.size() == 1);
            REQUIRE(valid.size() == 1);
            CHECK(values[0] == doctest::Approx(42.0));
            CHECK(valid[0] == true);
        }

        SUBCASE("TIndexHolder") {
            TIndexHolder holder;
            holder[StageIndex{0}] = 99.0;

            auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

            REQUIRE(values.size() == 1);
            REQUIRE(valid.size() == 1);
            CHECK(values[0] == doctest::Approx(99.0));
            CHECK(valid[0] == true);
        }

        SUBCASE("With Factor") {
            GSTBIndexHolder holder;
            holder[{ScenarioIndex{0}, StageIndex{0}, BlockIndex{0}}] = 10.0;

            block_factor_matrix_t factor(boost::extents[1][1]);
            factor[0][0] = {2.0}; // Factor for block 0

            auto [values, valid] = helper.flat(holder, [](auto v) { return v; }, factor);

            REQUIRE(values.size() == 2);
            CHECK(values[0] == doctest::Approx(20.0)); // 10.0 * 2.0
            CHECK(values[1] == doctest::Approx(0.0));  // No value set
            CHECK(valid[0] == true);
            CHECK(valid[1] == false);
        }
    }

    TEST_CASE("Edge Cases") {
        SUBCASE("Empty Active Elements") {
            FlatHelper helper({}, {}, {}, {});

            CHECK(helper.active_scenario_count() == 0);
            CHECK(helper.active_stage_count() == 0);
            CHECK(helper.active_block_count() == 0);

            GSTBIndexHolder holder;
            auto [values, valid] = helper.flat(holder, [](auto v) { return v; });
            CHECK(values.empty());
            CHECK(valid.empty());
        }

        SUBCASE("Missing Values") {
            std::vector<ScenarioIndex> active_scenarios = {ScenarioIndex{0}, ScenarioIndex{1}};
            std::vector<StageIndex> active_stages = {StageIndex{0}, StageIndex{1}};
            std::vector<std::vector<BlockIndex>> active_stage_blocks = {{BlockIndex{0}}, {BlockIndex{0}}};
            std::vector<BlockIndex> active_blocks = {BlockIndex{0}};

            FlatHelper helper(active_scenarios, active_stages, active_stage_blocks, active_blocks);

            GSTBIndexHolder holder;
            holder[{ScenarioIndex{0}, StageIndex{0}, BlockIndex{0}}] = 10.0;

            auto [values, valid] = helper.flat(holder, [](auto v) { return v; });

            REQUIRE(values.size() == 4); // 2 scenarios * 2 stages
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
}

} // namespace gtopt
