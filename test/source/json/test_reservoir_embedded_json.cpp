#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reservoir.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Reservoir with embedded seepage JSON")
{
  std::string_view json_data = R"({
    "uid": 1,
    "name": "rsv_with_seepage",
    "junction": 10,
    "eini": 500.0,
    "emax": 1000.0,
    "seepage": [
      {
        "uid": 1,
        "name": "filt1",
        "waterway": "ww1",
        "reservoir": "rsv_with_seepage",
        "slope": 0.02,
        "constant": 0.5,
        "segments": [
          { "volume": 0.0, "slope": 0.0003, "constant": 0.5 },
          { "volume": 500.0, "slope": 0.0001, "constant": 0.65 }
        ]
      }
    ]
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 1);
  CHECK(res.name == "rsv_with_seepage");
  REQUIRE(res.seepage.size() == 1);
  CHECK(res.seepage[0].uid == 1);
  CHECK(res.seepage[0].name == "filt1");
  CHECK(std::get<Name>(res.seepage[0].waterway) == "ww1");
  CHECK(std::get<Name>(res.seepage[0].reservoir) == "rsv_with_seepage");
  CHECK(std::get<Real>(res.seepage[0].slope.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.02));
  CHECK(std::get<Real>(res.seepage[0].constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.5));
  REQUIRE(res.seepage[0].segments.size() == 2);
  CHECK(res.seepage[0].segments[0].slope == doctest::Approx(0.0003));
  CHECK(res.seepage[0].segments[1].volume == doctest::Approx(500.0));
}

TEST_CASE("Reservoir with embedded discharge_limit JSON")
{
  std::string_view json_data = R"({
    "uid": 2,
    "name": "rsv_with_ddl",
    "junction": 20,
    "discharge_limit": [
      {
        "uid": 1,
        "name": "ddl1",
        "waterway": 5,
        "reservoir": 2,
        "segments": [
          { "volume": 0.0, "slope": 6.9868e-5, "intercept": 15.787 },
          { "volume": 757000.0, "slope": 1.3985e-4, "intercept": 57.454 }
        ]
      }
    ]
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 2);
  CHECK(res.name == "rsv_with_ddl");
  REQUIRE(res.discharge_limit.size() == 1);
  CHECK(res.discharge_limit[0].uid == 1);
  CHECK(res.discharge_limit[0].name == "ddl1");
  CHECK(std::get<Uid>(res.discharge_limit[0].waterway) == Uid {5});
  REQUIRE(res.discharge_limit[0].segments.size() == 2);
  CHECK(res.discharge_limit[0].segments[0].intercept
        == doctest::Approx(15.787));
  CHECK(res.discharge_limit[0].segments[1].volume == doctest::Approx(757000.0));
}

TEST_CASE("Reservoir with embedded production_factor JSON")
{
  std::string_view json_data = R"({
    "uid": 3,
    "name": "rsv_with_pfac",
    "junction": 30,
    "production_factor": [
      {
        "uid": 1,
        "name": "eff1",
        "turbine": 10,
        "reservoir": 3,
        "mean_production_factor": 1.53,
        "segments": [
          { "volume": 0.0, "slope": 0.0002294, "constant": 1.2558 },
          { "volume": 500.0, "slope": 0.0001, "constant": 1.53 }
        ]
      }
    ]
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 3);
  CHECK(res.name == "rsv_with_pfac");
  REQUIRE(res.production_factor.size() == 1);
  CHECK(res.production_factor[0].uid == 1);
  CHECK(res.production_factor[0].name == "eff1");
  CHECK(std::get<Uid>(res.production_factor[0].turbine) == Uid {10});
  CHECK(res.production_factor[0].mean_production_factor
        == doctest::Approx(1.53));
  REQUIRE(res.production_factor[0].segments.size() == 2);
  CHECK(res.production_factor[0].segments[0].slope
        == doctest::Approx(0.0002294));
}

TEST_CASE("Reservoir with all embedded constraints JSON")
{
  std::string_view json_data = R"({
    "uid": 10,
    "name": "rsv_full",
    "junction": 1,
    "eini": 500.0,
    "emax": 1000.0,
    "seepage": [
      {
        "uid": 1,
        "name": "seep1",
        "waterway": 1,
        "reservoir": 10,
        "slope": 0.001,
        "constant": 0.5
      }
    ],
    "discharge_limit": [
      {
        "uid": 1,
        "name": "ddl1",
        "waterway": 1,
        "reservoir": 10,
        "segments": [
          { "volume": 0.0, "slope": 1e-4, "intercept": 10.0 }
        ]
      }
    ],
    "production_factor": [
      {
        "uid": 1,
        "name": "eff1",
        "turbine": 1,
        "reservoir": 10,
        "mean_production_factor": 1.5,
        "segments": [
          { "volume": 0.0, "slope": 0.001, "constant": 1.0 }
        ]
      }
    ]
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 10);
  CHECK(res.seepage.size() == 1);
  CHECK(res.discharge_limit.size() == 1);
  CHECK(res.production_factor.size() == 1);
}

TEST_CASE("Reservoir with absent embedded constraints")
{
  std::string_view json_data = R"({
    "uid": 1,
    "name": "rsv_plain",
    "junction": 1
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.seepage.empty());
  CHECK(res.discharge_limit.empty());
  CHECK(res.production_factor.empty());
}

TEST_CASE("Reservoir embedded constraints roundtrip")
{
  Reservoir original {
      .uid = Uid {100},
      .name = "rsv_roundtrip",
      .junction = SingleId {Uid {1}},
      .emax = 1000.0,
      .eini = 500.0,
      .seepage =
          {
              {
                  .uid = 1,
                  .name = "seep1",
                  .waterway = Uid {5},
                  .reservoir = Uid {100},
                  .slope = 0.001,
                  .constant = 0.5,
              },
          },
      .discharge_limit =
          {
              {
                  .uid = 1,
                  .name = "ddl1",
                  .waterway = Uid {5},
                  .reservoir = Uid {100},
                  .segments =
                      {
                          {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
                      },
              },
          },
      .production_factor =
          {
              {
                  .uid = 1,
                  .name = "eff1",
                  .turbine = Uid {1},
                  .reservoir = Uid {100},
                  .mean_production_factor = 1.5,
                  .segments =
                      {
                          {.volume = 0.0, .slope = 0.001, .constant = 1.0},
                      },
              },
          },
  };

  const auto json = daw::json::to_json(original);
  const Reservoir roundtrip = daw::json::from_json<Reservoir>(json);

  CHECK(roundtrip.uid == 100);
  REQUIRE(roundtrip.seepage.size() == 1);
  CHECK(roundtrip.seepage[0].name == "seep1");
  CHECK(std::get<Uid>(roundtrip.seepage[0].waterway) == Uid {5});

  REQUIRE(roundtrip.discharge_limit.size() == 1);
  CHECK(roundtrip.discharge_limit[0].name == "ddl1");
  CHECK(roundtrip.discharge_limit[0].segments.size() == 1);

  REQUIRE(roundtrip.production_factor.size() == 1);
  CHECK(roundtrip.production_factor[0].name == "eff1");
  CHECK(roundtrip.production_factor[0].mean_production_factor
        == doctest::Approx(1.5));
}
