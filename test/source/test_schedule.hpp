#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("schedule test vector")
{
  const Id id;
  const System sys;

  const Simulation sim = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
              {.uid = Uid {3}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {1}, .probability_factor = 0.5},
              {.uid = Uid {2}, .probability_factor = 0.5},
          },
  };

  const OptionsLP options;
  SimulationLP simulation {sim, options};

  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  SUBCASE("tfield")
  {
    std::vector<double> vec = {1, 2};
    const TRealFieldSched tfield {vec};

    const TRealSched tsched {ic, "class", id, tfield};

    REQUIRE(tsched.at(StageUid {1}) == 1);
    REQUIRE(tsched.at(StageUid {2}) == 2);
  }

  SUBCASE("stbfield")
  {
    std::vector<std::vector<std::vector<double>>> vec = {
        {{1}, {2, 3}},
        {{4}, {5, 6}},
    };
    const STBRealFieldSched stbfield {vec};

    const STBRealSched stbsched {ic, "class", id, stbfield};

    REQUIRE(stbsched.at(ScenarioUid {1}, StageUid {1}, BlockUid {1}) == 1);
    REQUIRE(stbsched.at(ScenarioUid {1}, StageUid {2}, BlockUid {2}) == 2);
    REQUIRE(stbsched.at(ScenarioUid {1}, StageUid {2}, BlockUid {3}) == 3);
    REQUIRE(stbsched.at(ScenarioUid {2}, StageUid {1}, BlockUid {1}) == 4);
    REQUIRE(stbsched.at(ScenarioUid {2}, StageUid {2}, BlockUid {2}) == 5);
    REQUIRE(stbsched.at(ScenarioUid {2}, StageUid {2}, BlockUid {3}) == 6);
  }

  SUBCASE("tbfield")
  {
    std::vector<std::vector<double>> vec = {{{1}, {2, 3}}};
    const TBRealFieldSched tbfield {vec};

    const TBRealSched tbsched {ic, "class", id, tbfield};

    REQUIRE(tbsched.at(StageUid {1}, BlockUid {1}) == 1);
    REQUIRE(tbsched.at(StageUid {2}, BlockUid {2}) == 2);
    REQUIRE(tbsched.at(StageUid {2}, BlockUid {3}) == 3);
  }

  SUBCASE("stfield")
  {
    std::vector<std::vector<double>> vec = {{1, 2}, {3, 4}};
    const STRealFieldSched stfield {vec};

    const STRealSched stsched {ic, "class", id, stfield};

    REQUIRE(stsched.at(ScenarioUid {1}, StageUid {1}) == 1);
    REQUIRE(stsched.at(ScenarioUid {1}, StageUid {2}) == 2);
    REQUIRE(stsched.at(ScenarioUid {2}, StageUid {1}) == 3);
    REQUIRE(stsched.at(ScenarioUid {2}, StageUid {2}) == 4);
  }
}

TEST_CASE("opt schedule test ")
{
  const gtopt::OptSchedule<int, StageUid, BlockUid> sched0;

  const ArrowChunkedArray array;
  const UidToArrowIdx<StageUid, BlockUid>::UidIdx uididx;

  auto array_tuple = std::make_tuple(array, uididx);

  FieldSched2<Int> field_sched = -3;
  const gtopt::OptSchedule<int, StageUid, BlockUid> sched(field_sched);

  REQUIRE(sched.at(StageUid {1}, BlockUid {1}) == -3);
}

TEST_CASE("schedule test ")
{
  const gtopt::Schedule<int, StageUid, BlockUid> sched {-3};

  REQUIRE(sched.at(StageUid {1}, BlockUid {1}) == -3);
}

TEST_CASE("schedule test 2")
{
  using Vector = std::vector<int>;
  const Vector vec = {1, 2, 3, 4};

  const gtopt::FieldSched<int, Vector> fs = {3};

  const gtopt::Schedule<int, StageUid> sched {fs};
  REQUIRE(sched.at(StageUid {2}) == 3);
}

TEST_CASE("schedule test 3")
{
  using Vector = std::vector<std::vector<int>>;
  const Vector vec2 = {{1, 2}, {3, 4}};

  const gtopt::FieldSched<int, Vector> fs = {4};
  const gtopt::Schedule<int, ScenarioUid, StageUid> sched(fs);

  REQUIRE(sched.at(ScenarioUid {0}, StageUid {0}) == 4);
  REQUIRE(sched.at(ScenarioUid {1}, StageUid {1}) == 4);
}
