#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reservoir_production_factor.hpp>
#include <gtopt/reservoir_production_factor.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ProductionFactorSegment daw json round-trip")
{
  std::string_view json_data = R"({
    "volume": 500.0,
    "slope": 0.0002294,
    "constant": 1.2558
  })";

  const ProductionFactorSegment seg =
      daw::json::from_json<ProductionFactorSegment>(json_data);

  REQUIRE(seg.volume == doctest::Approx(500.0));
  REQUIRE(seg.slope == doctest::Approx(0.0002294));
  REQUIRE(seg.constant == doctest::Approx(1.2558));
}

TEST_CASE("ProductionFactorSegment daw json default values")
{
  std::string_view json_data = R"({
    "volume": 0.0,
    "slope": 0.0,
    "constant": 0.0
  })";

  const ProductionFactorSegment seg =
      daw::json::from_json<ProductionFactorSegment>(json_data);

  REQUIRE(seg.volume == doctest::Approx(0.0));
  REQUIRE(seg.slope == doctest::Approx(0.0));
  REQUIRE(seg.constant == doctest::Approx(0.0));
}

TEST_CASE("ReservoirProductionFactor daw json test 1")
{
  std::string_view json_data = R"({
    "uid": 1,
    "name": "eff_colbun",
    "turbine": 10,
    "reservoir": 20,
    "mean_production_factor": 1.53,
    "segments": [
      { "volume": 0.0, "slope": 0.0002294, "constant": 1.2558 }
    ]
  })";

  const ReservoirProductionFactor re =
      daw::json::from_json<ReservoirProductionFactor>(json_data);

  REQUIRE(re.uid == 1);
  REQUIRE(re.name == "eff_colbun");
  REQUIRE(std::get<Uid>(re.turbine) == Uid {10});
  REQUIRE(std::get<Uid>(re.reservoir) == Uid {20});
  REQUIRE(re.mean_production_factor == doctest::Approx(1.53));
  REQUIRE(re.segments.size() == 1);
  REQUIRE(re.segments[0].volume == doctest::Approx(0.0));
  REQUIRE(re.segments[0].slope == doctest::Approx(0.0002294));
  REQUIRE(re.segments[0].constant == doctest::Approx(1.2558));
}

TEST_CASE("ReservoirProductionFactor daw json test - multiple segments")
{
  std::string_view json_data = R"({
    "uid": 2,
    "name": "eff_eltoro",
    "turbine": 11,
    "reservoir": 21,
    "mean_production_factor": 4.8,
    "segments": [
      { "volume": 0.0,   "slope": 0.0003, "constant": 1.8 },
      { "volume": 500.0, "slope": 0.0001, "constant": 4.8 }
    ]
  })";

  const ReservoirProductionFactor re =
      daw::json::from_json<ReservoirProductionFactor>(json_data);

  REQUIRE(re.uid == 2);
  REQUIRE(re.name == "eff_eltoro");
  REQUIRE(re.mean_production_factor == doctest::Approx(4.8));
  REQUIRE(re.segments.size() == 2);
  REQUIRE(re.segments[1].volume == doctest::Approx(500.0));
  REQUIRE(re.segments[1].constant == doctest::Approx(4.8));
}

TEST_CASE("ReservoirProductionFactor daw json test - empty segments")
{
  std::string_view json_data = R"({
    "uid": 3,
    "name": "eff_simple",
    "turbine": 12,
    "reservoir": 22,
    "mean_production_factor": 2.0,
    "segments": []
  })";

  const ReservoirProductionFactor re =
      daw::json::from_json<ReservoirProductionFactor>(json_data);

  REQUIRE(re.uid == 3);
  REQUIRE(re.mean_production_factor == doctest::Approx(2.0));
  REQUIRE(re.segments.empty());
}

TEST_CASE("ReservoirProductionFactor daw json array test")
{
  std::string_view json_data = R"([
    {
      "uid": 1,
      "name": "eff_a",
      "turbine": 10,
      "reservoir": 20,
      "mean_production_factor": 1.5,
      "segments": [
        { "volume": 0.0, "slope": 0.001, "constant": 1.2 }
      ]
    },
    {
      "uid": 2,
      "name": "eff_b",
      "turbine": 11,
      "reservoir": 21,
      "mean_production_factor": 3.0,
      "segments": [
        { "volume": 0.0,    "slope": 0.002, "constant": 2.5 },
        { "volume": 1000.0, "slope": 0.001, "constant": 3.0 }
      ]
    }
  ])";

  const std::vector<ReservoirProductionFactor> effs =
      daw::json::from_json_array<ReservoirProductionFactor>(json_data);

  REQUIRE(effs.size() == 2);
  CHECK(effs[0].uid == 1);
  CHECK(effs[0].name == "eff_a");
  CHECK(effs[0].segments.size() == 1);
  CHECK(effs[1].uid == 2);
  CHECK(effs[1].segments.size() == 2);
  CHECK(effs[1].segments[1].volume == doctest::Approx(1000.0));
}

TEST_CASE("ReservoirProductionFactor to_json and from_json round-trip")
{
  const ReservoirProductionFactor orig {
      .uid = 7,
      .name = "eff_roundtrip",
      .turbine = Uid {101},
      .reservoir = Uid {201},
      .mean_production_factor = 1.75,
      .segments =
          {
              {.volume = 0.0, .slope = 0.0005, .constant = 1.5},
              {.volume = 800.0, .slope = 0.0002, .constant = 1.75},
          },
  };

  const auto json = daw::json::to_json(orig);
  const ReservoirProductionFactor roundtrip =
      daw::json::from_json<ReservoirProductionFactor>(json);

  CHECK(roundtrip.uid == 7);
  CHECK(roundtrip.name == "eff_roundtrip");
  CHECK(std::get<Uid>(roundtrip.turbine) == Uid {101});
  CHECK(std::get<Uid>(roundtrip.reservoir) == Uid {201});
  CHECK(roundtrip.mean_production_factor == doctest::Approx(1.75));
  REQUIRE(roundtrip.segments.size() == 2);
  CHECK(roundtrip.segments[0].slope == doctest::Approx(0.0005));
  CHECK(roundtrip.segments[1].volume == doctest::Approx(800.0));
}
