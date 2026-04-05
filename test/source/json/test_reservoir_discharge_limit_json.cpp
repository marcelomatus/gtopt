#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reservoir_discharge_limit.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ReservoirDischargeLimitSegment daw json round-trip")
{
  std::string_view json_data = R"({
    "volume": 757000.0,
    "slope": 1.3985e-4,
    "intercept": 57.454
  })";

  const ReservoirDischargeLimitSegment seg =
      daw::json::from_json<ReservoirDischargeLimitSegment>(json_data);

  CHECK(seg.volume == doctest::Approx(757000.0));
  CHECK(seg.slope == doctest::Approx(1.3985e-4));
  CHECK(seg.intercept == doctest::Approx(57.454));
}

TEST_CASE("ReservoirDischargeLimit daw json test - basic")
{
  std::string_view json_data = R"({
    "uid": 1,
    "name": "ddl_ralco",
    "waterway": 10,
    "reservoir": 20,
    "segments": [
      { "volume": 0.0, "slope": 6.9868e-5, "intercept": 15.787 },
      { "volume": 757000.0, "slope": 1.3985e-4, "intercept": 57.454 }
    ]
  })";

  const ReservoirDischargeLimit ddl =
      daw::json::from_json<ReservoirDischargeLimit>(json_data);

  CHECK(ddl.uid == 1);
  CHECK(ddl.name == "ddl_ralco");
  CHECK(std::get<Uid>(ddl.waterway) == Uid {10});
  CHECK(std::get<Uid>(ddl.reservoir) == Uid {20});
  REQUIRE(ddl.segments.size() == 2);
  CHECK(ddl.segments[0].volume == doctest::Approx(0.0));
  CHECK(ddl.segments[0].slope == doctest::Approx(6.9868e-5));
  CHECK(ddl.segments[0].intercept == doctest::Approx(15.787));
  CHECK(ddl.segments[1].volume == doctest::Approx(757000.0));
  CHECK(ddl.segments[1].slope == doctest::Approx(1.3985e-4));
  CHECK(ddl.segments[1].intercept == doctest::Approx(57.454));
}

TEST_CASE("ReservoirDischargeLimit daw json test - no segments")
{
  std::string_view json_data = R"({
    "uid": 2,
    "name": "ddl_empty",
    "waterway": 5,
    "reservoir": 6
  })";

  const ReservoirDischargeLimit ddl =
      daw::json::from_json<ReservoirDischargeLimit>(json_data);

  CHECK(ddl.uid == 2);
  CHECK(ddl.name == "ddl_empty");
  CHECK(std::get<Uid>(ddl.waterway) == Uid {5});
  CHECK(std::get<Uid>(ddl.reservoir) == Uid {6});
  CHECK(ddl.segments.empty());
}

TEST_CASE("ReservoirDischargeLimit daw json test - string IDs")
{
  std::string_view json_data = R"({
    "uid": 3,
    "name": "ddl_named",
    "waterway": "ww_ralco",
    "reservoir": "rsv_ralco",
    "segments": [
      { "volume": 0.0, "slope": 1e-4, "intercept": 10.0 }
    ]
  })";

  const ReservoirDischargeLimit ddl =
      daw::json::from_json<ReservoirDischargeLimit>(json_data);

  CHECK(ddl.uid == 3);
  CHECK(std::get<Name>(ddl.waterway) == "ww_ralco");
  CHECK(std::get<Name>(ddl.reservoir) == "rsv_ralco");
  REQUIRE(ddl.segments.size() == 1);
}

TEST_CASE("ReservoirDischargeLimit daw json array test")
{
  std::string_view json_data = R"([
    {
      "uid": 1,
      "name": "ddl_a",
      "waterway": 10,
      "reservoir": 20,
      "segments": [
        { "volume": 0.0, "slope": 1e-4, "intercept": 10.0 }
      ]
    },
    {
      "uid": 2,
      "name": "ddl_b",
      "waterway": 11,
      "reservoir": 21,
      "segments": [
        { "volume": 0.0, "slope": 2e-4, "intercept": 20.0 },
        { "volume": 500.0, "slope": 3e-4, "intercept": 30.0 }
      ]
    }
  ])";

  const std::vector<ReservoirDischargeLimit> ddls =
      daw::json::from_json_array<ReservoirDischargeLimit>(json_data);

  REQUIRE(ddls.size() == 2);
  CHECK(ddls[0].uid == 1);
  CHECK(ddls[0].segments.size() == 1);
  CHECK(ddls[1].uid == 2);
  CHECK(ddls[1].segments.size() == 2);
  CHECK(ddls[1].segments[1].volume == doctest::Approx(500.0));
}

TEST_CASE("ReservoirDischargeLimit to_json and from_json round-trip")
{
  const ReservoirDischargeLimit orig {
      .uid = 42,
      .name = "ddl_roundtrip",
      .waterway = Uid {101},
      .reservoir = Uid {201},
      .segments =
          {
              {.volume = 0.0, .slope = 6.9868e-5, .intercept = 15.787},
              {.volume = 757000.0, .slope = 1.3985e-4, .intercept = 57.454},
          },
  };

  const auto json = daw::json::to_json(orig);
  const ReservoirDischargeLimit roundtrip =
      daw::json::from_json<ReservoirDischargeLimit>(json);

  CHECK(roundtrip.uid == 42);
  CHECK(roundtrip.name == "ddl_roundtrip");
  CHECK(std::get<Uid>(roundtrip.waterway) == Uid {101});
  CHECK(std::get<Uid>(roundtrip.reservoir) == Uid {201});
  REQUIRE(roundtrip.segments.size() == 2);
  CHECK(roundtrip.segments[0].slope == doctest::Approx(6.9868e-5));
  CHECK(roundtrip.segments[0].intercept == doctest::Approx(15.787));
  CHECK(roundtrip.segments[1].volume == doctest::Approx(757000.0));
  CHECK(roundtrip.segments[1].intercept == doctest::Approx(57.454));
}

TEST_CASE("ReservoirDischargeLimit with active property")
{
  SUBCASE("With boolean active")
  {
    ReservoirDischargeLimit ddl;
    ddl.uid = 1;
    ddl.name = "test_ddl";
    ddl.active = True;

    auto json = daw::json::to_json(ddl);
    const ReservoirDischargeLimit roundtrip =
        daw::json::from_json<ReservoirDischargeLimit>(json);

    CHECK(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value_or(False)) == True);
  }

  SUBCASE("Without active (null)")
  {
    std::string_view json_data = R"({
      "uid": 1,
      "name": "ddl_noactive",
      "waterway": 1,
      "reservoir": 2
    })";

    const ReservoirDischargeLimit ddl =
        daw::json::from_json<ReservoirDischargeLimit>(json_data);

    CHECK_FALSE(ddl.active.has_value());
  }
}
