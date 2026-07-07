// SPDX-License-Identifier: BSD-3-Clause
//
// Per-(stage, block) promotion tests for the T→TB sweep.
//
//   * PR-A: Generator.gcost / Demand.fcost / Battery.discharge_cost /
//     charge_cost
//   * PR-B: Generator.heat_rate / lossfactor / emission_rate,
//           Demand.lossfactor, Line.lossfactor
//   * PR-C: ReserveZone.urcost/drcost,
//           ReserveProvision.urcost/drcost/{ur,dr}_capacity_factor/
//           {ur,dr}_provision_factor,
//           InertiaZone.cost,
//           InertiaProvision.cost/provision_factor,
//           FlowRight.fcost/uvalue
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
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/inertia_provision.hpp>
#include <gtopt/inertia_zone.hpp>
#include <gtopt/json/json_battery.hpp>
#include <gtopt/json/json_converter.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_flow_right.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_inertia_provision.hpp>
#include <gtopt/json/json_inertia_zone.hpp>
#include <gtopt/json/json_reserve_provision.hpp>
#include <gtopt/json/json_reserve_zone.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>

using namespace gtopt;

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

TEST_CASE("Battery.discharge_cost — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_discharge": 60,
    "pmax_charge": 60,
    "input_efficiency": 0.95,
    "output_efficiency": 0.95,
    "discharge_cost": [[2.5, 5.0, 7.5]]
  })";

  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.discharge_cost.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*bat.discharge_cost);
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

// ─── PR-B: per-block heat_rate / lossfactor / emission_rate ──────

TEST_CASE("Generator.heat_rate — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmin": 0,
    "pmax": 100,
    "heat_rate": [[9.5, 10.0, 11.5]]
  })";
  const auto gen = daw::json::from_json<Generator>(json_data);
  REQUIRE(gen.heat_rate.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*gen.heat_rate);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][2] == doctest::Approx(11.5));
}

TEST_CASE("Generator.lossfactor — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmin": 0,
    "pmax": 100,
    "lossfactor": [[0.01, 0.02]]
  })";
  const auto gen = daw::json::from_json<Generator>(json_data);
  REQUIRE(gen.lossfactor.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*gen.lossfactor);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 2);
  CHECK(v[0][0] == doctest::Approx(0.01));
  CHECK(v[0][1] == doctest::Approx(0.02));
}

TEST_CASE(
    "Generator.emission_rate — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmin": 0,
    "pmax": 100,
    "emission_rate": [[0.3, 0.4, 0.5]]
  })";
  const auto gen = daw::json::from_json<Generator>(json_data);
  REQUIRE(gen.emission_rate.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*gen.emission_rate);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][1] == doctest::Approx(0.4));
}

TEST_CASE("Demand.lossfactor — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "d1",
    "bus": 1,
    "lmax": [[50.0]],
    "lossfactor": [[0.05, 0.08]]
  })";
  const auto dem = daw::json::from_json<Demand>(json_data);
  REQUIRE(dem.lossfactor.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*dem.lossfactor);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 2);
  CHECK(v[0][0] == doctest::Approx(0.05));
  CHECK(v[0][1] == doctest::Approx(0.08));
}

TEST_CASE("Generator.heat_rate — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmin": 0,
    "pmax": 100,
    "heat_rate": 9.5
  })";
  const auto gen = daw::json::from_json<Generator>(json_data);
  REQUIRE(gen.heat_rate.has_value());
  CHECK(std::get<Real>(*gen.heat_rate) == doctest::Approx(9.5));
}

// ─── PR-C: per-block reserve / inertia / flow_right cost block ─────

TEST_CASE("ReserveZone.urcost/drcost — 2-D JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "z1",
    "urcost": [[900.0, 1000.0]],
    "drcost": [[700.0, 800.0]]
  })";
  const auto rz = daw::json::from_json<ReserveZone>(json_data);
  REQUIRE(rz.urcost.has_value());
  REQUIRE(rz.drcost.has_value());
  const auto& vu = std::get<std::vector<std::vector<Real>>>(*rz.urcost);
  const auto& vd = std::get<std::vector<std::vector<Real>>>(*rz.drcost);
  REQUIRE(vu.size() == 1);
  REQUIRE(vu[0].size() == 2);
  CHECK(vu[0][0] == doctest::Approx(900.0));
  CHECK(vu[0][1] == doctest::Approx(1000.0));
  CHECK(vd[0][0] == doctest::Approx(700.0));
  CHECK(vd[0][1] == doctest::Approx(800.0));
}

TEST_CASE("ReserveZone.urcost — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "z1",
    "urcost": 950.0
  })";
  const auto rz = daw::json::from_json<ReserveZone>(json_data);
  REQUIRE(rz.urcost.has_value());
  CHECK(std::get<Real>(*rz.urcost) == doctest::Approx(950.0));
}

TEST_CASE("ReserveProvision.{ur,dr}cost/provision_factor — 2-D JSON")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "p1",
    "generator": 1,
    "reserve_zones": [1],
    "urcost": [[5.0, 8.0]],
    "drcost": [[3.0, 4.0]],
    "ur_capacity_factor": [[1.0, 0.9]],
    "dr_capacity_factor": [[1.0, 0.8]],
    "ur_provision_factor": [[1.0, 0.95]],
    "dr_provision_factor": [[1.0, 0.9]]
  })";
  const auto rp = daw::json::from_json<ReserveProvision>(json_data);
  REQUIRE(rp.urcost.has_value());
  REQUIRE(rp.drcost.has_value());
  REQUIRE(rp.ur_capacity_factor.has_value());
  REQUIRE(rp.ur_provision_factor.has_value());
  const auto& vu = std::get<std::vector<std::vector<Real>>>(*rp.urcost);
  REQUIRE(vu[0].size() == 2);
  CHECK(vu[0][0] == doctest::Approx(5.0));
  CHECK(vu[0][1] == doctest::Approx(8.0));
  const auto& vupf =
      std::get<std::vector<std::vector<Real>>>(*rp.ur_provision_factor);
  CHECK(vupf[0][1] == doctest::Approx(0.95));
}

TEST_CASE("InertiaZone.cost — 2-D JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "iz1",
    "cost": [[50.0, 75.0]]
  })";
  const auto iz = daw::json::from_json<InertiaZone>(json_data);
  REQUIRE(iz.cost.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*iz.cost);
  REQUIRE(v[0].size() == 2);
  CHECK(v[0][0] == doctest::Approx(50.0));
  CHECK(v[0][1] == doctest::Approx(75.0));
}

TEST_CASE("InertiaProvision.cost/provision_factor — 2-D JSON")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "ip1",
    "generator": 1,
    "inertia_zones": [1],
    "provision_factor": [[20.0, 22.0]],
    "cost": [[1.5, 2.5]]
  })";
  const auto ip = daw::json::from_json<InertiaProvision>(json_data);
  REQUIRE(ip.cost.has_value());
  REQUIRE(ip.provision_factor.has_value());
  const auto& vc = std::get<std::vector<std::vector<Real>>>(*ip.cost);
  const auto& vpf =
      std::get<std::vector<std::vector<Real>>>(*ip.provision_factor);
  CHECK(vc[0][1] == doctest::Approx(2.5));
  CHECK(vpf[0][0] == doctest::Approx(20.0));
}

TEST_CASE("FlowRight.fcost/uvalue — 2-D JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "fr1",
    "fcost": [[5000.0, 5500.0]],
    "uvalue": [[10.0, 12.5]]
  })";
  const auto fr = daw::json::from_json<FlowRight>(json_data);
  REQUIRE(fr.fcost.has_value());
  REQUIRE(fr.uvalue.has_value());
  const auto& vf = std::get<std::vector<std::vector<Real>>>(*fr.fcost);
  const auto& vu = std::get<std::vector<std::vector<Real>>>(*fr.uvalue);
  REQUIRE(vf[0].size() == 2);
  CHECK(vf[0][0] == doctest::Approx(5000.0));
  CHECK(vf[0][1] == doctest::Approx(5500.0));
  CHECK(vu[0][1] == doctest::Approx(12.5));
}

TEST_CASE("FlowRight.fcost — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "fr1",
    "fcost": 3000.0
  })";
  const auto fr = daw::json::from_json<FlowRight>(json_data);
  REQUIRE(fr.fcost.has_value());
  CHECK(std::get<Real>(*fr.fcost) == doctest::Approx(3000.0));
}

// ─── PR-E: Battery efficiencies (input / output) → TB ──────────────

TEST_CASE(
    "Battery.input_efficiency — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_charge": 60,
    "pmax_discharge": 60,
    "input_efficiency": [[0.95, 0.93, 0.91]],
    "output_efficiency": [[0.90, 0.92, 0.94]]
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.input_efficiency.has_value());
  REQUIRE(bat.output_efficiency.has_value());
  const auto& vi =
      std::get<std::vector<std::vector<Real>>>(*bat.input_efficiency);
  const auto& vo =
      std::get<std::vector<std::vector<Real>>>(*bat.output_efficiency);
  REQUIRE(vi[0].size() == 3);
  CHECK(vi[0][0] == doctest::Approx(0.95));
  CHECK(vi[0][2] == doctest::Approx(0.91));
  CHECK(vo[0][0] == doctest::Approx(0.90));
  CHECK(vo[0][2] == doctest::Approx(0.94));
}

TEST_CASE(
    "Battery.input_efficiency — scalar still broadcasts (legacy)")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_charge": 60,
    "pmax_discharge": 60,
    "input_efficiency": 0.95,
    "output_efficiency": 0.93
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.input_efficiency.has_value());
  CHECK(std::get<Real>(*bat.input_efficiency) == doctest::Approx(0.95));
  CHECK(std::get<Real>(*bat.output_efficiency) == doctest::Approx(0.93));
}

// --- TB promotion of pmax_* + new pmin_* + commitment JSON parse ----------

TEST_CASE("Battery.pmax_charge — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_charge": [[10.0, 10.5, 11.0]],
    "pmax_discharge": [[8.0, 8.5, 9.0]]
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.pmax_charge.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*bat.pmax_charge);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][0] == doctest::Approx(10.0));
  CHECK(v[0][2] == doctest::Approx(11.0));
  REQUIRE(bat.pmax_discharge.has_value());
  const auto& w = std::get<std::vector<std::vector<Real>>>(*bat.pmax_discharge);
  CHECK(w[0][1] == doctest::Approx(8.5));
}

TEST_CASE(
    "Battery.pmin_charge / pmin_discharge — scalar JSON parses")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_charge": 60,
    "pmax_discharge": 60,
    "pmin_charge": 5,
    "pmin_discharge": 2
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.pmin_charge.has_value());
  CHECK(std::get<Real>(*bat.pmin_charge) == doctest::Approx(5.0));
  REQUIRE(bat.pmin_discharge.has_value());
  CHECK(std::get<Real>(*bat.pmin_discharge) == doctest::Approx(2.0));
}

TEST_CASE("Battery.pmin_charge — 2-D per-block JSON parses as TB")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "pmax_charge": 60,
    "pmax_discharge": 60,
    "pmin_charge": [[5.0, 5.1, 5.2, 5.3]]
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.pmin_charge.has_value());
  const auto& v = std::get<std::vector<std::vector<Real>>>(*bat.pmin_charge);
  REQUIRE(v[0].size() == 4);
  CHECK(v[0][0] == doctest::Approx(5.0));
  CHECK(v[0][3] == doctest::Approx(5.3));
}

TEST_CASE("Battery.commitment — JSON parses as OptBool")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1,
    "commitment": true
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  REQUIRE(bat.commitment.has_value());
  CHECK(*bat.commitment == true);
}

TEST_CASE("Battery.commitment — JSON omission leaves nullopt")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "b1",
    "bus": 1
  })";
  const auto bat = daw::json::from_json<Battery>(json_data);
  CHECK_FALSE(bat.commitment.has_value());
}

TEST_CASE("Converter.commitment — JSON parses as OptBool")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1,
    "name": "c1",
    "battery": "bat1",
    "generator": "gen1",
    "demand": "dem1",
    "commitment": true
  })";
  const auto conv = daw::json::from_json<Converter>(json_data);
  REQUIRE(conv.commitment.has_value());
  CHECK(*conv.commitment == true);
}

TEST_CASE("Demand.lmin — JSON scalar + 2-D TB shapes parse")  // NOLINT
{
  // Scalar broadcast
  {
    constexpr std::string_view json_data = R"({
      "uid": 1, "name": "d1", "bus": 1,
      "lmax": 100, "lmin": 30
    })";
    const auto dem = daw::json::from_json<Demand>(json_data);
    REQUIRE(dem.lmin.has_value());
    CHECK(std::get<Real>(*dem.lmin) == doctest::Approx(30.0));
  }
  // Per-block 2-D
  {
    constexpr std::string_view json_data = R"({
      "uid": 1, "name": "d1", "bus": 1,
      "lmax": [[100, 100, 100]],
      "lmin": [[20, 25, 30]]
    })";
    const auto dem = daw::json::from_json<Demand>(json_data);
    REQUIRE(dem.lmin.has_value());
    const auto& v = std::get<std::vector<std::vector<Real>>>(*dem.lmin);
    REQUIRE(v[0].size() == 3);
    CHECK(v[0][0] == doctest::Approx(20.0));
    CHECK(v[0][2] == doctest::Approx(30.0));
  }
}

TEST_CASE("Demand.lmin — JSON omission leaves nullopt")  // NOLINT
{
  constexpr std::string_view json_data = R"({
    "uid": 1, "name": "d1", "bus": 1, "lmax": 100
  })";
  const auto dem = daw::json::from_json<Demand>(json_data);
  CHECK_FALSE(dem.lmin.has_value());
}
