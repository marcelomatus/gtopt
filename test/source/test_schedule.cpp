#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("schedule test vector")
{
  using namespace gtopt;
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

  const PlanningOptionsLP options;
  SimulationLP simulation {sim, options};

  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  SUBCASE("tfield")
  {
    std::vector<double> vec = {1, 2};
    const TRealFieldSched tfield {vec};

    const TRealSched tsched {ic, "class", id, tfield};

    REQUIRE(tsched.at(make_uid<Stage>(1)) == 1);
    REQUIRE(tsched.at(make_uid<Stage>(2)) == 2);
  }

  SUBCASE("stbfield")
  {
    std::vector<std::vector<std::vector<double>>> vec = {
        {{1}, {2, 3}},
        {{4}, {5, 6}},
    };
    const STBRealFieldSched stbfield {vec};

    const STBRealSched stbsched {ic, "class", id, stbfield};

    REQUIRE(stbsched.at(
                make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1))
            == 1);
    REQUIRE(stbsched.at(
                make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(2))
            == 2);
    REQUIRE(stbsched.at(
                make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3))
            == 3);
    REQUIRE(stbsched.at(
                make_uid<Scenario>(2), make_uid<Stage>(1), make_uid<Block>(1))
            == 4);
    REQUIRE(stbsched.at(
                make_uid<Scenario>(2), make_uid<Stage>(2), make_uid<Block>(2))
            == 5);
    REQUIRE(stbsched.at(
                make_uid<Scenario>(2), make_uid<Stage>(2), make_uid<Block>(3))
            == 6);
  }

  SUBCASE("tbfield")
  {
    std::vector<std::vector<double>> vec = {{{1}, {2, 3}}};
    const TBRealFieldSched tbfield {vec};

    const TBRealSched tbsched {ic, "class", id, tbfield};

    REQUIRE(tbsched.at(make_uid<Stage>(1), make_uid<Block>(1)) == 1);
    REQUIRE(tbsched.at(make_uid<Stage>(2), make_uid<Block>(2)) == 2);
    REQUIRE(tbsched.at(make_uid<Stage>(2), make_uid<Block>(3)) == 3);
  }

  SUBCASE("stfield")
  {
    std::vector<std::vector<double>> vec = {{1, 2}, {3, 4}};
    const STRealFieldSched stfield {vec};

    const STRealSched stsched {ic, "class", id, stfield};

    REQUIRE(stsched.at(make_uid<Scenario>(1), make_uid<Stage>(1)) == 1);
    REQUIRE(stsched.at(make_uid<Scenario>(1), make_uid<Stage>(2)) == 2);
    REQUIRE(stsched.at(make_uid<Scenario>(2), make_uid<Stage>(1)) == 3);
    REQUIRE(stsched.at(make_uid<Scenario>(2), make_uid<Stage>(2)) == 4);
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

  REQUIRE(sched.at(make_uid<Stage>(1), make_uid<Block>(1)) == -3);
}

TEST_CASE("schedule test ")
{
  const gtopt::Schedule<int, StageUid, BlockUid> sched {-3};

  REQUIRE(sched.at(make_uid<Stage>(1), make_uid<Block>(1)) == -3);
}

TEST_CASE("schedule test 2")
{
  using Vector = std::vector<int>;
  const Vector vec = {1, 2, 3, 4};

  const gtopt::FieldSched<int, Vector> fs = {3};

  const gtopt::Schedule<int, StageUid> sched {fs};
  REQUIRE(sched.at(make_uid<Stage>(2)) == 3);
}

TEST_CASE("schedule test 3")
{
  using Vector = std::vector<std::vector<int>>;
  const Vector vec2 = {{1, 2}, {3, 4}};

  const gtopt::FieldSched<int, Vector> fs = {4};
  const gtopt::Schedule<int, ScenarioUid, StageUid> sched(fs);

  REQUIRE(sched.at(make_uid<Scenario>(0), make_uid<Stage>(0)) == 4);
  REQUIRE(sched.at(make_uid<Scenario>(1), make_uid<Stage>(1)) == 4);
}

// ─── Per-(stage, block) schedule input shape tests ─────────────────────
//
// Pins the conversion semantics for ``OptTBRealSched`` introduced when
// Battery/Reservoir/VolumeRight/LngTerminal upgraded ``emin``/``emax``
// from per-stage to per-(stage, block) on 2026-05-18.  The wrapper
// accepts three JSON / C++ shapes (the FieldSched variant alternatives):
//
//   1. **Scalar** — broadcasts to every (stage, block).
//   2. **2-D nested array** ``[[block, …], …]`` — per-(stage, block).
//   3. **FileSched path** — file-backed schedule (covered by other tests).
//
// These tests pin shapes (1) and (2) at the OptSchedule level, using the
// no-arrow-array ctor that takes a bare FSched (no InputContext needed).

TEST_CASE(
    "FieldSched2 / OptTBRealSched — scalar broadcast across (stage, block)")
{
  using namespace gtopt;

  // FieldSched2<Real> = variant<Real, vector<vector<Real>>, FileSched>.
  // Storing a scalar puts the value in the ``Real`` alternative.  The
  // OptSchedule ``at(stage, block)`` resolver returns the scalar for
  // every (stage, block) pair — native broadcast (no arrow array
  // lookup required).
  FieldSched2<Real> scalar_field = 42.5;
  OptSchedule<Real, StageUid, BlockUid> sched {scalar_field};

  // Any (stage_uid, block_uid) returns the same scalar.
  CHECK(sched.at(make_uid<Stage>(1), make_uid<Block>(1)).value_or(-1.0)
        == doctest::Approx(42.5));
  CHECK(sched.at(make_uid<Stage>(5), make_uid<Block>(99)).value_or(-1.0)
        == doctest::Approx(42.5));
  CHECK(sched.at(make_uid<Stage>(0), make_uid<Block>(0)).value_or(-1.0)
        == doctest::Approx(42.5));
}

TEST_CASE("FieldSched2 / OptTBRealSched — empty optional yields no value")
{
  using namespace gtopt;

  const OptSchedule<Real, StageUid, BlockUid> sched_empty;
  CHECK_FALSE(sched_empty.has_value());
  CHECK_FALSE(
      sched_empty.at(make_uid<Stage>(1), make_uid<Block>(1)).has_value());
}

TEST_CASE("FieldSched2 — 2-D ``[[block, …], …]`` shape construction")
{
  using namespace gtopt;

  // 2 stages × 3 blocks: stage 0 = {10, 20, 30}, stage 1 = {40, 50, 60}.
  // Constructing the FieldSched2 stores the alternative correctly.
  FieldSched2<Real> nested_field = std::vector<std::vector<Real>> {
      {10.0, 20.0, 30.0},
      {40.0, 50.0, 60.0},
  };

  REQUIRE(std::holds_alternative<std::vector<std::vector<Real>>>(nested_field));
  const auto& mat = std::get<std::vector<std::vector<Real>>>(nested_field);
  REQUIRE(mat.size() == 2);
  REQUIRE(mat[0].size() == 3);
  CHECK(mat[0][0] == doctest::Approx(10.0));
  CHECK(mat[1][2] == doctest::Approx(60.0));
}

TEST_CASE(
    "FieldSched2 — per-stage list encoded as Mx1 nested array (PLP shape)")
{
  using namespace gtopt;

  // PLP-style per-stage data: each stage has a single per-stage value.
  // Encoded as a M×1 nested vector — one stage row, each row has a
  // 1-block inner vector.  The 2-D variant accepts this shape cleanly.
  FieldSched2<Real> per_stage_field = std::vector<std::vector<Real>> {
      {
          100.0,
      },
      {
          200.0,
      },
      {
          300.0,
      },
  };

  REQUIRE(
      std::holds_alternative<std::vector<std::vector<Real>>>(per_stage_field));
  const auto& mat = std::get<std::vector<std::vector<Real>>>(per_stage_field);
  REQUIRE(mat.size() == 3);
  for (const auto& row : mat) {
    REQUIRE(row.size() == 1);
  }
  CHECK(mat[0][0] == doctest::Approx(100.0));
  CHECK(mat[1][0] == doctest::Approx(200.0));
  CHECK(mat[2][0] == doctest::Approx(300.0));
}
