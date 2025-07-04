#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

TEST_CASE("LabelMaker basic functionality")
{
  Options options;
  options.use_lp_names = true;
  const OptionsLP options_lp(options);
  const LabelMaker maker(options_lp);

  SUBCASE("Simple labels")
  {
    CHECK(maker.lp_label("var") == "var");
    CHECK(maker.lp_label("prefix", "var") == "prefix_var");
    CHECK(maker.lp_label("a", "b", "c") == "a_b_c");
  }

  SUBCASE("Empty labels when disabled")
  {
    Options disabled_options;
    disabled_options.use_lp_names = false;
    const OptionsLP disabled_options_lp(disabled_options);
    const LabelMaker disabled_maker(disabled_options_lp);

    CHECK(disabled_maker.lp_label("var").empty());
    CHECK(disabled_maker.lp_label("prefix", "var").empty());
  }
}

TEST_CASE("LabelMaker with StageLP")
{
  Options options;
  options.use_lp_names = true;
  const OptionsLP options_lp(options);
  const LabelMaker maker(options_lp);

  const Stage stage1 {.uid = Uid {1}, .name = "stage1"};
  const Stage stage2 {.uid = Uid {2}, .name = "stage2"};
  const StageLP stage1_lp(stage1);
  const StageLP stage2_lp(stage2);

  SUBCASE("With single stage")
  {
    CHECK(maker.lp_label(stage1_lp, "var", "x", "y") == "var_x_y_1");
    CHECK(maker.lp_label(stage2_lp, "a", "b", "c") == "a_b_c_2");
  }
}

TEST_CASE("LabelMaker with ScenarioLP and StageLP")
{
  Options options;
  options.use_lp_names = true;
  const OptionsLP options_lp(options);
  const LabelMaker maker(options_lp);

  const Scenario scenario1 {.uid = Uid {1}, .name = "scenario1"};
  const Scenario scenario2 {.uid = Uid {2}, .name = "scenario2"};
  const ScenarioLP scenario1_lp(scenario1);
  const ScenarioLP scenario2_lp(scenario2);

  const Stage stage1 {.uid = Uid {1}, .name = "stage1"};
  const Stage stage2 {.uid = Uid {2}, .name = "stage2"};
  const StageLP stage1_lp(stage1);
  const StageLP stage2_lp(stage2);

  SUBCASE("With scenario and stage")
  {
    CHECK(maker.lp_label(scenario1_lp, stage1_lp, "var", "x", "y")
          == "var_x_y_1_1");
    CHECK(maker.lp_label(scenario2_lp, stage2_lp, "a", "b", "c")
          == "a_b_c_2_2");
  }
}

TEST_CASE("LabelMaker with BlockLP")
{
  Options options;
  options.use_lp_names = true;
  const OptionsLP options_lp(options);
  const LabelMaker maker(options_lp);

  const Scenario scenario1 {.uid = Uid {1}, .name = "scenario1"};
  const ScenarioLP scenario1_lp(scenario1);
  const Stage stage1 {.uid = Uid {1}, .name = "stage1"};
  const StageLP stage1_lp(stage1);
  const Block block1 {.uid = Uid {1}, .name = "block1"};
  const BlockLP block1_lp(block1);

  SUBCASE("With block")
  {
    CHECK(maker.lp_label(scenario1_lp, stage1_lp, block1_lp, "var", "a", "b")
          == "var_a_b_1_1_1");
  }
}

TEST_CASE("LabelMaker edge cases")
{
  Options options;
  options.use_lp_names = true;
  const OptionsLP options_lp(options);
  const LabelMaker maker(options_lp);

  SUBCASE("Empty label")
  {
    CHECK(maker.lp_label() == "");
  }

  SUBCASE("Minimum required arguments")
  {
    const Stage stage {.uid = Uid {1}, .name = "stage"};
    const StageLP stage_lp(stage);
    CHECK(maker.lp_label(stage_lp, "a", "b", "c") == "a_b_c_1");
  }
}
