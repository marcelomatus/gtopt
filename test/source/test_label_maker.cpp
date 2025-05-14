#include <doctest/doctest.h>

#include <gtopt/block_lp.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

TEST_SUITE("LabelMaker") {

TEST_CASE("basic label generation")
{
    const OptionsLP options;
    std::vector<ScenarioLP> scenarios;
    std::vector<StageLP> stages;

    // Setup test data
    scenarios.emplace_back(Scenario{.uid = Uid{1}, .name = "scenario1"}, std::vector<StageLP>{});
    scenarios.emplace_back(Scenario{.uid = Uid{2}, .name = "scenario2"}, std::vector<StageLP>{});

    stages.emplace_back(Stage{.uid = Uid{1}, .name = "stage1"}, std::vector<BlockLP>{});
    stages.emplace_back(Stage{.uid = Uid{2}, .name = "stage2"}, std::vector<BlockLP>{});

    const Block block1{.uid = Uid{1}, .name = "block1"};
    const Block block2{.uid = Uid{2}, .name = "block2"};
    const BlockLP block1_lp{block1};
    const BlockLP block2_lp{block2};

    SECTION("Simple label generation")
    {
        LabelMaker maker(options, scenarios, stages);
        
        OptionsLP non_const_options = options;
        non_const_options.use_lp_names_ = true; // Access private member for test
        REQUIRE(maker.label("var") == "var");
        REQUIRE(maker.label("prefix", "var") == "prefix_var");
        REQUIRE(maker.label("a", "b", "c") == "a_b_c");
    }

    SECTION("Disabled label generation")
    {
        LabelMaker maker(options, scenarios, stages);
        
        OptionsLP non_const_options = options;
        non_const_options.use_lp_names_ = false; // Access private member for test
        REQUIRE(maker.label("var").empty());
        REQUIRE(maker.label("prefix", "var").empty());
    }

    SECTION("Time-based labels")
    {
        LabelMaker maker(options, scenarios, stages);
        options.use_lp_names(true);

        REQUIRE(maker.t_label(StageIndex{0}, "var", "x", "y") == "var_x_y_stage1");
        REQUIRE(maker.t_label(StageIndex{1}, "a", "b", "c") == "a_b_c_stage2");
    }

    SECTION("Scenario-time labels")
    {
        LabelMaker maker(options, scenarios, stages);
        options.use_lp_names(true);

        REQUIRE(maker.st_label(ScenarioIndex{0}, StageIndex{0}, "var", "x", "y") == "var_x_y_scenario1_stage1");
        REQUIRE(maker.st_label(ScenarioIndex{1}, StageIndex{1}, "a", "b", "c") == "a_b_c_scenario2_stage2");
    }

    SECTION("Scenario-time-block labels with objects")
    {
        LabelMaker maker(options, scenarios, stages);
        options.use_lp_names(true);

        REQUIRE(maker.stb_label(scenarios[0], stages[0], block1_lp, "var") == "var_scenario1_stage1_block1");
        REQUIRE(maker.stb_label(scenarios[1], stages[1], block2_lp, "a", "b") == "a_b_scenario2_stage2_block2");
    }

    SECTION("Scenario-time-block labels with indices")
    {
        LabelMaker maker(options, scenarios, stages);
        options.use_lp_names(true);

        REQUIRE(maker.stb_label(ScenarioIndex{0}, StageIndex{0}, block1_lp, "var") == "var_scenario1_stage1_block1");
        REQUIRE(maker.stb_label(ScenarioIndex{1}, StageIndex{1}, block2, "a", "b") == "a_b_scenario2_stage2_block2");
    }
} // TEST_SUITE

TEST_CASE("edge cases")
{
    OptionsLP options;
    std::vector<ScenarioLP> scenarios;
    std::vector<StageLP> stages;

    LabelMaker maker(options, scenarios, stages);

    SECTION("Empty inputs")
    {
        options.use_lp_names(true);
        REQUIRE(maker.label() == "");
        REQUIRE(maker.t_label(StageIndex{0}) == "_stage1");
    }

    SECTION("Out of bounds indices")
    {
        options.use_lp_names(true);
        // Should handle gracefully (though in real usage indices should be valid)
        REQUIRE_NOTHROW(maker.t_label(StageIndex{99}, "var"));
        REQUIRE_NOTHROW(maker.st_label(ScenarioIndex{99}, StageIndex{99}, "var"));
    }
}
