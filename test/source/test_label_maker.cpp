#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

TEST_CASE("basic label generation")
{
  Options options;
  std::vector<ScenarioLP> scenarios;
  std::vector<StageLP> stages;

  // Setup test data
  scenarios.emplace_back(Scenario {.uid = Uid {1}, .name = "scenario1"});
  scenarios.emplace_back(Scenario {.uid = Uid {2}, .name = "scenario2"});

  stages.emplace_back(Stage {.uid = Uid {1}, .name = "stage1"});
  stages.emplace_back(Stage {.uid = Uid {2}, .name = "stage2"});

  Block block1 {.uid = Uid {1}, .name = "block1"};
  Block block2 {.uid = Uid {2}, .name = "block2"};
  BlockLP block1_lp {block1};
  BlockLP block2_lp {block2};

  {
    options.use_lp_names = true;  // Access private member for test
    OptionsLP non_const_options {options};
    LabelMaker maker(non_const_options);

    REQUIRE(maker.lp_label("var") == "var");
    REQUIRE(maker.lp_label("prefix", "var") == "prefix_var");
    REQUIRE(maker.lp_label("a", "b", "c") == "a_b_c");
  }

  {
    Options options;
    options.use_lp_names = false;
    OptionsLP non_const_options {options};
    LabelMaker maker(non_const_options);

    REQUIRE(maker.lp_label("var").empty());
    REQUIRE(maker.lp_label("prefix", "var").empty());
  }

  {
    Options options;
    options.use_lp_names = true;
    OptionsLP non_const_options {options};
    LabelMaker maker(non_const_options);

    auto& st0 = stages[0];
    StageLP& st1 = stages[1];
    REQUIRE(maker.lp_label(st0, "var", "x", "y") == "var_x_y_1");
    REQUIRE(maker.lp_label(st1, "a", "b", "c") == "a_b_c_2");
  }

  {
    Options options;
    options.use_lp_names = true;
    OptionsLP non_const_options {options};
    LabelMaker maker(non_const_options);

    REQUIRE(maker.lp_label(scenarios[0], stages[0], "var", "x", "y")
            == "var_x_y_1_1");
    REQUIRE(maker.lp_label(scenarios[1], stages[1], "a", "b", "c")
            == "a_b_c_2_2");
  }

  {
    Options options;
    options.use_lp_names = true;
    OptionsLP non_const_options {options};
    LabelMaker maker(non_const_options);

    REQUIRE(maker.lp_label(scenarios[0], stages[0], block1_lp, "var", "a", "b")
            == "var_a_b_1_1_1");
    REQUIRE(maker.lp_label(scenarios[1], stages[1], block2_lp, "a", "b", "c")
            == "a_b_c_2_2_2");
  }

  {
    Options options;
    options.use_lp_names = true;
    OptionsLP non_const_options {options};
    LabelMaker maker(non_const_options);

    REQUIRE(maker.lp_label(scenarios[0], stages[0], block1_lp, "var", "a", "b")
            == "var_a_b_1_1_1");
    REQUIRE(maker.lp_label(scenarios[1], stages[1], block2_lp, "a", "b", "c")
            == "a_b_c_2_2_2");
  }
}

TEST_CASE("edge cases")
{
  Options opt;
  opt.use_lp_names = true;
  OptionsLP options {opt};
  std::vector<ScenarioLP> scenarios;
  std::vector<StageLP> stages;

  // Setup test data
  scenarios.emplace_back(Scenario {.uid = Uid {1}, .name = "scenario1"});
  scenarios.emplace_back(Scenario {.uid = Uid {2}, .name = "scenario2"});

  stages.emplace_back(Stage {.uid = Uid {1}, .name = "stage1"});
  stages.emplace_back(Stage {.uid = Uid {2}, .name = "stage2"});

  LabelMaker maker(options);

  {
    REQUIRE(maker.lp_label() == "");
    REQUIRE(maker.lp_label(stages[0], "a", "b", "c") == "a_b_c_1");
  }
}
