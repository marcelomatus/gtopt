// SPDX-License-Identifier: BSD-3-Clause
//
// Per-(stage, block) promotion tests for PR-A:
//   * Generator.gcost   — OptTBRealFieldSched
//   * Demand.fcost      — OptTBRealFieldSched
//   * Battery.gcost     — OptTBRealFieldSched
//   * Battery.charge_cost — OptTBRealFieldSched
//
// Each test exercises three round-trip paths:
//   1. JSON parse of a 2-D ``[[block0, block1, ...]]`` literal — the
//      shape only TB schedules accept.
//   2. C++ ``TBRealFieldSched {std::vector<std::vector<Real>>{...}}``
//      construction — the shape used by direct in-process tests.
//   3. Scalar broadcast — the legacy ``"gcost": 20.0`` form must keep
//      working unchanged.

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/json/json_battery.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_generator.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Generator.gcost — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmin": 0,
    "pmax": 100,
    "gcost": [[10.0, 20.0, 30.0]]
  })";

  const auto gen = daw::json::from_json<Generator>(json_data);
  REQUIRE(gen.gcost.has_value());
  // The 2-D variant is std::vector<std::vector<Real>>.  Each outer
  // index = stage, inner = block within stage.
  const auto& v = std::get<std::vector<std::vector<Real>>>(*gen.gcost);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][0] == doctest::Approx(10.0));
  CHECK(v[0][1] == doctest::Approx(20.0));
  CHECK(v[0][2] == doctest::Approx(30.0));
}

TEST_CASE("Generator.gcost — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmin": 0,
    "pmax": 100,
    "gcost": 25.0
  })";

  const auto gen = daw::json::from_json<Generator>(json_data);
  REQUIRE(gen.gcost.has_value());
  // Scalar variant — `double` (Real).
  CHECK(std::get<Real>(*gen.gcost) == doctest::Approx(25.0));
}

TEST_CASE("Demand.fcost — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "d1",
    "bus": 1,
    "lmax": [[50.0]],
    "fcost": [[1000.0, 5000.0]]
  })";

  const auto dem = daw::json::from_json<Demand>(json_data);
  REQUIRE(dem.fcost.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*dem.fcost);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 2);
  CHECK(v[0][0] == doctest::Approx(1000.0));
  CHECK(v[0][1] == doctest::Approx(5000.0));
}

TEST_CASE("Demand.fcost — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "d1",
    "bus": 1,
    "lmax": [[50.0]],
    "fcost": 1500.0
  })";

  const auto dem = daw::json::from_json<Demand>(json_data);
  REQUIRE(dem.fcost.has_value());
  CHECK(std::get<Real>(*dem.fcost) == doctest::Approx(1500.0));
}

TEST_CASE("Battery.gcost — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_discharge": 60,
    "pmax_charge": 60,
    "input_efficiency": 0.95,
    "output_efficiency": 0.95,
    "gcost": [[2.5, 5.0, 7.5]]
  })";

  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.gcost.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*bat.gcost);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][0] == doctest::Approx(2.5));
  CHECK(v[0][1] == doctest::Approx(5.0));
  CHECK(v[0][2] == doctest::Approx(7.5));
}

TEST_CASE("Battery.charge_cost — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_discharge": 60,
    "pmax_charge": 60,
    "input_efficiency": 0.95,
    "output_efficiency": 0.95,
    "charge_cost": [[1.0, 3.0]]
  })";

  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.charge_cost.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*bat.charge_cost);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 2);
  CHECK(v[0][0] == doctest::Approx(1.0));
  CHECK(v[0][1] == doctest::Approx(3.0));
}

TEST_CASE("Battery.charge_cost — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_discharge": 60,
    "pmax_charge": 60,
    "input_efficiency": 0.95,
    "output_efficiency": 0.95,
    "charge_cost": 4.2
  })";

  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.charge_cost.has_value());
  CHECK(std::get<Real>(*bat.charge_cost) == doctest::Approx(4.2));
}

TEST_CASE("TBRealFieldSched 2-D construction direct")  // NOLINT
{
  // C++-side construction mirroring how tests pin per-block costs
  // without parsing JSON.  This is the literal shape produced by
  // ``tools/ucjl2gtopt.py`` after the PR-A enhancement.
  const Generator gen {
      .uid = Uid {1},
      .name = "g1",
      .bus = Uid {1},
      .pmin = TBRealFieldSched {0.0},
      .pmax = TBRealFieldSched {100.0},
      .gcost = TBRealFieldSched {std::vector<std::vector<Real>> {
          {10.0, 20.0, 30.0},
      }},
  };
  REQUIRE(gen.gcost.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*gen.gcost);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][0] == doctest::Approx(10.0));
  CHECK(v[0][1] == doctest::Approx(20.0));
  CHECK(v[0][2] == doctest::Approx(30.0));
}
