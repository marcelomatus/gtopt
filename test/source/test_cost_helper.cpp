#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

TEST_CASE("Construction and basic properties")
{
  const OptionsLP options;
  const std::vector<ScenarioLP> scenarios;
  const std::vector<StageLP> stages;

  const CostHelper helper(options, scenarios, stages);

  SUBCASE("Empty construction works")
  {
    CHECK_NOTHROW(CostHelper(options, scenarios, stages));
  }
}

TEST_CASE("block_cost calculation")
{
  Options opt;
  opt.scale_objective = 1.0;
  const OptionsLP options {opt};

  Scenario scenario;
  scenario.probability_factor = 0.5;
  std::vector<ScenarioLP> scenarios {ScenarioLP {scenario}};
  Stage stage;
  stage.discount_factor = 0.9;
  std::vector<StageLP> stages {StageLP {stage}};

  Block pblock;
  pblock.duration = 1.0;
  const BlockLP block {pblock};
  const CostHelper helper(options, scenarios, stages);

  SUBCASE("Basic cost calculation")
  {
    const double cost = 100.0;
    const double expected = 100.0 * 0.9 * 0.5;
    CHECK(helper.block_ecost(scenarios[0], stages[0], block, cost)
          == doctest::Approx(expected));
  }

  SUBCASE("Zero cost")
  {
    CHECK(helper.block_ecost(scenarios[0], stages[0], block, 0.0)
          == doctest::Approx(0.0));
  }
}

TEST_CASE("stage_cost calculation")
{
  Options opt;
  opt.scale_objective = 1.0;

  const OptionsLP options {opt};

  Scenario scenario;
  scenario.probability_factor = 1.0;
  std::vector<ScenarioLP> scenarios {ScenarioLP {scenario}};

  std::vector<Block> pblocks(2);
  pblocks[0].duration = 1.0;
  pblocks[1].duration = 3.0;
  std::vector<BlockLP> block_lps;
  block_lps.emplace_back(pblocks[0]);
  block_lps.emplace_back(pblocks[1]);

  std::vector<Stage> stages(2);
  stages[0].first_block = 0;
  stages[0].count_block = 1;
  stages[0].discount_factor = 0.9;

  stages[1].discount_factor = 0.8;
  stages[1].first_block = 1;
  stages[1].count_block = 1;

  std::vector<StageLP> stage_lps;
  stage_lps.emplace_back(stages[0], pblocks);
  stage_lps.emplace_back(stages[1], pblocks);

  const CostHelper helper(options, scenarios, stage_lps);

  SUBCASE("First stage")
  {
    const double cost = 100.0;
    CHECK(helper.stage_ecost(stage_lps[0], cost)
          == doctest::Approx(100.0 * 0.9));
  }

  SUBCASE("Second stage")
  {
    const double cost = 100.0;
    const double prob_factor = 0.5;
    CHECK(helper.stage_ecost(stage_lps[1], cost, prob_factor)
          == doctest::Approx(100.0 * 0.8 * 0.5 * 3.0));
  }
}

TEST_CASE("scenario_stage_cost calculation")
{
  Options opt;
  opt.scale_objective = 1.0;
  const OptionsLP options {opt};

  std::vector<Scenario> scenarios(2);
  scenarios[0].probability_factor = 0.6;
  scenarios[1].probability_factor = 0.4;
  std::vector<ScenarioLP> scenario_lps;
  scenario_lps.emplace_back(scenarios[0]);
  scenario_lps.emplace_back(scenarios[1]);

  std::vector<Block> pblocks(2);
  pblocks[0].duration = 1.0;
  pblocks[1].duration = 1.0;
  std::vector<BlockLP> block_lps;
  block_lps.emplace_back(pblocks[0]);
  block_lps.emplace_back(pblocks[1]);

  std::vector<Stage> stages(2);
  stages[0].first_block = 0;
  stages[0].count_block = 1;
  stages[0].discount_factor = 0.9;

  stages[1].discount_factor = 0.8;
  stages[1].first_block = 1;
  stages[1].count_block = 1;

  std::vector<StageLP> stage_lps;
  stage_lps.emplace_back(stages[0], pblocks);
  stage_lps.emplace_back(stages[1], pblocks);

  const CostHelper helper(options, scenario_lps, stage_lps);

  SUBCASE("First scenario, first stage")
  {
    const double cost = 100.0;
    CHECK(helper.scenario_stage_ecost(scenario_lps[0], stage_lps[0], cost)
          == doctest::Approx(100.0 * 0.9 * 0.6));
  }

  SUBCASE("Second scenario, second stage")
  {
    const double cost = 100.0;
    CHECK(helper.scenario_stage_ecost(scenario_lps[1], stage_lps[1], cost)
          == doctest::Approx(100.0 * 0.8 * 0.4));
  }
}

TEST_CASE("Factor matrix generation")
{
  Options opt;
  opt.scale_objective = 1.0;
  const OptionsLP options {opt};

  std::vector<Scenario> scenarios(2);
  scenarios[0].probability_factor = 0.6;
  scenarios[1].probability_factor = 0.4;
  std::vector<ScenarioLP> scenario_lps;
  scenario_lps.emplace_back(scenarios[0]);
  scenario_lps.emplace_back(scenarios[1]);

  std::vector<Block> pblocks(2);
  pblocks[0].duration = 1.0;
  pblocks[1].duration = 1.0;
  std::vector<BlockLP> block_lps;
  block_lps.emplace_back(pblocks[0]);
  block_lps.emplace_back(pblocks[1]);

  std::vector<Stage> stages(2);
  stages[0].first_block = 0;
  stages[0].count_block = 1;
  stages[0].discount_factor = 0.9;

  stages[1].discount_factor = 0.8;
  stages[1].first_block = 1;
  stages[1].count_block = 1;

  std::vector<StageLP> stage_lps;
  stage_lps.emplace_back(stages[0], pblocks);
  stage_lps.emplace_back(stages[1], pblocks);

  CostHelper helper(options, scenario_lps, stage_lps);

  SUBCASE("block_icost_factors")
  {
    auto factors = helper.block_icost_factors();
    REQUIRE(factors.size() == 2);  // scenarios
    REQUIRE(factors[0].size() == 2);  // stages
    // Just verify the structure - exact values depend on BlockLP
  }

  SUBCASE("stage_icost_factors")
  {
    auto factors = helper.stage_icost_factors();
    REQUIRE(factors.size() == 2);  // stages
    CHECK(factors[0] == doctest::Approx(1.0 / 0.9));
    CHECK(factors[1] == doctest::Approx(1.0 / 0.8));
  }

  SUBCASE("scenario_stage_icost_factors")
  {
    auto factors = helper.scenario_stage_icost_factors();
    REQUIRE(factors.size() == 2);  // scenarios
    REQUIRE(factors[0].size() == 2);  // stages
    CHECK(factors[0][0] == doctest::Approx(1.0 / (0.6 * 0.9)));
    CHECK(factors[0][1] == doctest::Approx(1.0 / (0.6 * 0.8)));
    CHECK(factors[1][0] == doctest::Approx(1.0 / (0.4 * 0.9)));
    CHECK(factors[1][1] == doctest::Approx(1.0 / (0.4 * 0.8)));
  }
}
