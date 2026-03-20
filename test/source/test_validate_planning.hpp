// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/validate_planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build a minimal valid Planning with one bus, one block, one stage.
[[nodiscard]] Planning make_minimal_planning()
{
  Planning p;
  p.system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  p.simulation.block_array = {
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
  };
  p.simulation.stage_array = {
      {
          .uid = Uid {1},
          .count_block = 1,
      },
  };
  return p;
}

TEST_CASE("validate_planning - minimal valid planning passes")  // NOLINT
{
  auto p = make_minimal_planning();
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.errors.empty());
  CHECK(result.warnings.empty());
}

TEST_CASE("validate_planning - empty bus_array is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.bus_array.clear();
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE("validate_planning - empty block_array is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.simulation.block_array.clear();
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE("validate_planning - empty stage_array is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.simulation.stage_array.clear();
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE(
    "validate_planning - all empty arrays reports multiple errors")  // NOLINT
{
  Planning p;
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 3);
}

TEST_CASE("validate_planning - block duration <= 0 is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.simulation.block_array[0].duration = 0.0;
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());

  SUBCASE("negative duration")
  {
    auto p2 = make_minimal_planning();
    p2.simulation.block_array[0].duration = -5.0;
    auto r2 = validate_planning(p2);
    CHECK_FALSE(r2.ok());
  }
}

TEST_CASE("validate_planning - stage count_block == 0 is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.simulation.stage_array[0].count_block = 0;
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
}

TEST_CASE(
    "validate_planning - generator negative capacity is a warning")  // NOLINT
{
  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  gen.capacity = RealFieldSched {
      -10.0,
  };
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  // Negative capacity is a warning, not an error
  CHECK(result.ok());
  CHECK(result.warnings.size() >= 1);
}

TEST_CASE(
    "validate_planning - generator with valid capacity has no warnings")  // NOLINT
{
  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  gen.capacity = RealFieldSched {
      100.0,
  };
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.warnings.empty());
}

TEST_CASE("validate_planning - generator referencing invalid bus")  // NOLINT
{
  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {999};  // Non-existent bus
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE("validate_planning - demand referencing invalid bus")  // NOLINT
{
  auto p = make_minimal_planning();
  Demand dem;
  dem.uid = Uid {1};
  dem.name = "d1";
  dem.bus = Uid {999};  // Non-existent bus
  p.system.demand_array.push_back(dem);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
}

TEST_CASE(
    "validate_planning - line referencing invalid bus_a and bus_b")  // NOLINT
{
  auto p = make_minimal_planning();
  Line line;
  line.uid = Uid {1};
  line.name = "l1";
  line.bus_a = Uid {999};
  line.bus_b = Uid {998};
  p.system.line_array.push_back(line);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  // Both bus_a and bus_b are invalid → 2 errors
  CHECK(result.errors.size() >= 2);
}

TEST_CASE("validate_planning - line with valid bus references")  // NOLINT
{
  auto p = make_minimal_planning();
  Line line;
  line.uid = Uid {1};
  line.name = "l1";
  line.bus_a = Uid {1};  // Valid bus
  line.bus_b = Uid {1};  // Same bus, but valid reference
  p.system.line_array.push_back(line);
  auto result = validate_planning(p);
  CHECK(result.ok());
}

TEST_CASE("validate_planning - generator referenced by name (valid)")  // NOLINT
{
  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Name {"b1"};  // Reference bus by name
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK(result.ok());
}

TEST_CASE(
    "validate_planning - generator referenced by name (invalid)")  // NOLINT
{
  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Name {"nonexistent_bus"};
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
}

TEST_CASE("validate_planning - converter with invalid refs")  // NOLINT
{
  auto p = make_minimal_planning();

  // Add a battery, generator, demand for valid refs
  Battery bat;
  bat.uid = Uid {1};
  bat.name = "bat1";
  bat.bus = Uid {1};
  p.system.battery_array.push_back(bat);

  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  p.system.generator_array.push_back(gen);

  Demand dem;
  dem.uid = Uid {1};
  dem.name = "d1";
  dem.bus = Uid {1};
  p.system.demand_array.push_back(dem);

  SUBCASE("all valid refs")
  {
    Converter conv;
    conv.uid = Uid {1};
    conv.name = "conv1";
    conv.battery = Uid {1};
    conv.generator = Uid {1};
    conv.demand = Uid {1};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("invalid battery ref")
  {
    Converter conv;
    conv.uid = Uid {2};
    conv.name = "conv2";
    conv.battery = Uid {999};
    conv.generator = Uid {1};
    conv.demand = Uid {1};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("invalid generator ref")
  {
    Converter conv;
    conv.uid = Uid {3};
    conv.name = "conv3";
    conv.battery = Uid {1};
    conv.generator = Uid {999};
    conv.demand = Uid {1};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("invalid demand ref")
  {
    Converter conv;
    conv.uid = Uid {4};
    conv.name = "conv4";
    conv.battery = Uid {1};
    conv.generator = Uid {1};
    conv.demand = Uid {999};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }
}

TEST_CASE("validate_planning - hydro element refs")  // NOLINT
{
  auto p = make_minimal_planning();

  // Add junctions
  Junction j1;
  j1.uid = Uid {1};
  j1.name = "j1";
  p.system.junction_array.push_back(j1);

  Junction j2;
  j2.uid = Uid {2};
  j2.name = "j2";
  p.system.junction_array.push_back(j2);

  SUBCASE("waterway with valid junctions")
  {
    Waterway ww;
    ww.uid = Uid {1};
    ww.name = "ww1";
    ww.junction_a = Uid {1};
    ww.junction_b = Uid {2};
    p.system.waterway_array.push_back(ww);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("waterway with invalid junction_a")
  {
    Waterway ww;
    ww.uid = Uid {1};
    ww.name = "ww1";
    ww.junction_a = Uid {999};
    ww.junction_b = Uid {2};
    p.system.waterway_array.push_back(ww);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("flow with valid junction")
  {
    Flow fl;
    fl.uid = Uid {1};
    fl.name = "fl1";
    fl.junction = Uid {1};
    p.system.flow_array.push_back(fl);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("flow with invalid junction")
  {
    Flow fl;
    fl.uid = Uid {1};
    fl.name = "fl1";
    fl.junction = Uid {999};
    p.system.flow_array.push_back(fl);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("reservoir with valid junction")
  {
    Reservoir res;
    res.uid = Uid {1};
    res.name = "res1";
    res.junction = Uid {1};
    p.system.reservoir_array.push_back(res);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("reservoir with invalid junction")
  {
    Reservoir res;
    res.uid = Uid {1};
    res.name = "res1";
    res.junction = Uid {999};
    p.system.reservoir_array.push_back(res);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }
}

TEST_CASE("validate_planning - turbine refs")  // NOLINT
{
  auto p = make_minimal_planning();

  // Add generator and waterway/junction for valid refs
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  p.system.generator_array.push_back(gen);

  Junction j1;
  j1.uid = Uid {1};
  j1.name = "j1";
  p.system.junction_array.push_back(j1);

  Junction j2;
  j2.uid = Uid {2};
  j2.name = "j2";
  p.system.junction_array.push_back(j2);

  Waterway ww;
  ww.uid = Uid {1};
  ww.name = "ww1";
  ww.junction_a = Uid {1};
  ww.junction_b = Uid {2};
  p.system.waterway_array.push_back(ww);

  SUBCASE("turbine with valid refs")
  {
    Turbine turb;
    turb.uid = Uid {1};
    turb.name = "t1";
    turb.waterway = Uid {1};
    turb.generator = Uid {1};
    p.system.turbine_array.push_back(turb);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("turbine with invalid waterway")
  {
    Turbine turb;
    turb.uid = Uid {1};
    turb.name = "t1";
    turb.waterway = Uid {999};
    turb.generator = Uid {1};
    p.system.turbine_array.push_back(turb);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("turbine with invalid generator")
  {
    Turbine turb;
    turb.uid = Uid {1};
    turb.name = "t1";
    turb.waterway = Uid {1};
    turb.generator = Uid {999};
    p.system.turbine_array.push_back(turb);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }
}

TEST_CASE("ValidationResult - ok() semantics")  // NOLINT
{
  ValidationResult r;
  CHECK(r.ok());

  r.warnings.emplace_back("a warning");
  CHECK(r.ok());  // warnings don't affect ok()

  r.errors.emplace_back("an error");
  CHECK_FALSE(r.ok());
}

}  // namespace
