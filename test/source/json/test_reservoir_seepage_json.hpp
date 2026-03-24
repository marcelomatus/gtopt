#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reservoir_seepage.hpp>

TEST_CASE("ReservoirSeepage daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20,
    "slope":1.5,
    "constant":100.0
  })";

  gtopt::ReservoirSeepage filt =
      daw::json::from_json<gtopt::ReservoirSeepage>(json_data);

  REQUIRE(filt.uid == 5);
  REQUIRE(filt.name == "FILTER_A");
  REQUIRE(std::get<Uid>(filt.waterway) == Uid(10));
  REQUIRE(std::get<Uid>(filt.reservoir) == 20);
  REQUIRE(std::get<Real>(filt.slope.value_or(TRealFieldSched {0.0}))
          == doctest::Approx(1.5));
  REQUIRE(std::get<Real>(filt.constant.value_or(TRealFieldSched {0.0}))
          == doctest::Approx(100.0));
}

TEST_CASE("ReservoirSeepage daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  })";

  gtopt::ReservoirSeepage filt =
      daw::json::from_json<gtopt::ReservoirSeepage>(json_data);

  REQUIRE(filt.uid == 5);
  REQUIRE(filt.name == "FILTER_A");
  REQUIRE(std::get<Uid>(filt.waterway) == 10);
  REQUIRE(std::get<Uid>(filt.reservoir) == 20);
  // slope and constant are nullopt when not present in JSON
  CHECK_FALSE(filt.slope.has_value());
  CHECK_FALSE(filt.constant.has_value());
}

TEST_CASE("ReservoirSeepage array json test")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  },{
    "uid":15,
    "name":"FILTER_B",
    "waterway":30,
    "reservoir":40,
    "slope":2.0,
    "constant":200.0
  }])";

  std::vector<gtopt::ReservoirSeepage> filts =
      daw::json::from_json_array<gtopt::ReservoirSeepage>(json_data);

  REQUIRE(filts[0].uid == 5);
  REQUIRE(filts[0].name == "FILTER_A");
  REQUIRE(std::get<Uid>(filts[0].waterway) == 10);
  REQUIRE(std::get<Uid>(filts[0].reservoir) == 20);

  REQUIRE(filts[1].uid == 15);
  REQUIRE(filts[1].name == "FILTER_B");
  REQUIRE(std::get<Uid>(filts[1].waterway) == 30);
  REQUIRE(std::get<Uid>(filts[1].reservoir) == 40);
  REQUIRE(std::get<Real>(filts[1].slope.value_or(TRealFieldSched {0.0}))
          == doctest::Approx(2.0));
  REQUIRE(std::get<Real>(filts[1].constant.value_or(TRealFieldSched {0.0}))
          == doctest::Approx(200.0));
}

TEST_CASE("ReservoirSeepage with active property serialization")
{
  SUBCASE("With boolean active")
  {
    ReservoirSeepage filt;
    filt.uid = 1;
    filt.name = "test_filt";
    filt.active = True;

    auto json = daw::json::to_json(filt);
    const ReservoirSeepage roundtrip =
        daw::json::from_json<ReservoirSeepage>(json);

    CHECK(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value_or(False)) == True);
  }

  SUBCASE("With schedule active")
  {
    ReservoirSeepage filt;
    filt.uid = 1;
    filt.name = "test_filt";
    filt.active = std::vector<IntBool> {True, False, True, False};

    auto json = daw::json::to_json(filt);
    ReservoirSeepage roundtrip = daw::json::from_json<ReservoirSeepage>(json);

    CHECK(roundtrip.active.has_value());
    if (roundtrip.active.has_value()) {
      const auto& active =
          std::get<std::vector<IntBool>>(roundtrip.active.value());
      CHECK(active.size() == 4);
      CHECK(active[0] == True);
      CHECK(active[1] == False);
      CHECK(active[2] == True);
      CHECK(active[3] == False);
    }
  }
}

TEST_CASE("ReservoirSeepage with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  })";

  const ReservoirSeepage filt =
      daw::json::from_json<ReservoirSeepage>(json_data);

  CHECK(filt.uid == 5);
  CHECK(filt.name == "FILTER_A");
  CHECK(filt.active.has_value() == false);
  // slope/constant absent in JSON → nullopt
  CHECK_FALSE(filt.slope.has_value());
  CHECK_FALSE(filt.constant.has_value());
  CHECK(filt.segments.empty());  // null defaults to empty
}

TEST_CASE("ReservoirSeepage with piecewise segments JSON")
{
  std::string_view json_data = R"({
    "uid":10,
    "name":"FILTER_PWL",
    "waterway":1,
    "reservoir":2,
    "slope":0.001,
    "constant":1.0,
    "segments":[
      {"volume":0.0, "slope":0.0003, "constant":0.5},
      {"volume":500.0, "slope":0.0001, "constant":0.65}
    ]
  })";

  const ReservoirSeepage filt =
      daw::json::from_json<ReservoirSeepage>(json_data);

  CHECK(filt.uid == 10);
  CHECK(filt.name == "FILTER_PWL");
  CHECK(std::get<Real>(filt.slope.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.001));
  CHECK(std::get<Real>(filt.constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(1.0));
  REQUIRE(filt.segments.size() == 2);
  CHECK(filt.segments[0].volume == doctest::Approx(0.0));
  CHECK(filt.segments[0].slope == doctest::Approx(0.0003));
  CHECK(filt.segments[0].constant == doctest::Approx(0.5));
  CHECK(filt.segments[1].volume == doctest::Approx(500.0));
  CHECK(filt.segments[1].slope == doctest::Approx(0.0001));
  CHECK(filt.segments[1].constant == doctest::Approx(0.65));
}

TEST_CASE("ReservoirSeepage with segments roundtrip")
{
  ReservoirSeepage filt;
  filt.uid = 42;
  filt.name = "roundtrip_test";
  filt.waterway = Uid {1};
  filt.reservoir = Uid {2};
  filt.slope = 0.01;
  filt.constant = 5.0;
  filt.segments = {
      {.volume = 0.0, .slope = 0.002, .constant = 1.0},
      {.volume = 1000.0, .slope = 0.001, .constant = 3.0},
  };

  auto json = daw::json::to_json(filt);
  const ReservoirSeepage roundtrip =
      daw::json::from_json<ReservoirSeepage>(json);

  CHECK(roundtrip.uid == 42);
  CHECK(roundtrip.name == "roundtrip_test");
  CHECK(std::get<Real>(roundtrip.slope.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.01));
  CHECK(std::get<Real>(roundtrip.constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(5.0));
  REQUIRE(roundtrip.segments.size() == 2);
  CHECK(roundtrip.segments[0].volume == doctest::Approx(0.0));
  CHECK(roundtrip.segments[0].slope == doctest::Approx(0.002));
  CHECK(roundtrip.segments[1].volume == doctest::Approx(1000.0));
  CHECK(roundtrip.segments[1].constant == doctest::Approx(3.0));
}

TEST_CASE("ReservoirSeepage with per-stage slope/constant array JSON")
{
  // Verify that slope and constant can be per-stage arrays in JSON
  // (analogous to how pmax in Generator accepts stage arrays)
  std::string_view json_data = R"({
    "uid":7,
    "name":"FILTER_SCHED",
    "waterway":1,
    "reservoir":2,
    "slope":[0.001, 0.002, 0.003],
    "constant":[1.0, 1.5, 2.0]
  })";

  const ReservoirSeepage filt =
      daw::json::from_json<ReservoirSeepage>(json_data);

  CHECK(filt.uid == 7);
  REQUIRE(filt.slope.has_value());
  REQUIRE(filt.constant.has_value());

  const auto& slope_vec =
      std::get<std::vector<Real>>(filt.slope.value());  // NOLINT
  REQUIRE(slope_vec.size() == 3);
  CHECK(slope_vec[0] == doctest::Approx(0.001));
  CHECK(slope_vec[1] == doctest::Approx(0.002));
  CHECK(slope_vec[2] == doctest::Approx(0.003));

  const auto& const_vec =
      std::get<std::vector<Real>>(filt.constant.value());  // NOLINT
  REQUIRE(const_vec.size() == 3);
  CHECK(const_vec[0] == doctest::Approx(1.0));
  CHECK(const_vec[1] == doctest::Approx(1.5));
  CHECK(const_vec[2] == doctest::Approx(2.0));
}

TEST_CASE("ReservoirSeepage with filename slope/constant JSON")
{
  // Verify that slope and constant can be filename strings in JSON
  // (referencing Parquet files from plpmanfi.dat)
  std::string_view json_data = R"({
    "uid":8,
    "name":"FILTER_FILE",
    "waterway":1,
    "reservoir":2,
    "slope":"slope",
    "constant":"constant"
  })";

  const ReservoirSeepage filt =
      daw::json::from_json<ReservoirSeepage>(json_data);

  CHECK(filt.uid == 8);
  REQUIRE(filt.slope.has_value());
  REQUIRE(filt.constant.has_value());
  CHECK(std::get<FileSched>(filt.slope.value()) == "slope");  // NOLINT
  CHECK(std::get<FileSched>(filt.constant.value()) == "constant");  // NOLINT
}
