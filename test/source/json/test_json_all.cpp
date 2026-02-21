/**
 * @file      test_json_all.cpp
 * @brief     Combined JSON serialization/deserialization tests
 * @date      2026-02-21
 * @copyright BSD-3-Clause
 *
 * Combined JSON unit tests for all gtopt types, merged into a single
 * compilation unit to reduce build time.
 */

#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/json/json_demand_profile.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_filtration.hpp>
#include <gtopt/json/json_flow.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_generator_profile.hpp>
#include <gtopt/json/json_junction.hpp>
#include <gtopt/json/json_line.hpp>
#include <gtopt/json/json_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json/json_reserve_provision.hpp>
#include <gtopt/json/json_reserve_zone.hpp>
#include <gtopt/json/json_reservoir.hpp>
#include <gtopt/json/json_simulation.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/json/json_turbine.hpp>
#include <gtopt/json/json_waterway.hpp>
#include <gtopt/object.hpp>
#include <gtopt/reservoir.hpp>

using namespace gtopt;

// === test_json_basic_types.cpp ===

namespace
{
struct Foo
{
  double f1 {};
};

using variant_t = std::variant<double, std::vector<double>, gtopt::FileSched>;
struct MyClass
{
  double f1;
  std::vector<double> f2;
  gtopt::FileSched f3;
  variant_t f4;
  std::optional<variant_t> f5;
};

}  // namespace

namespace daw::json
{

template<>
struct json_data_contract<Foo>
{
  using type = json_member_list<json_number<"f1", double>>;

  static auto to_json_data(Foo const& mc)
  {
    return std::forward_as_tuple(mc.f1);
  }
};

template<>
struct json_data_contract<MyClass>
{
  using type = json_member_list<
      json_number<"f1", double>,
      json_array<"f2", double>,
      json_string<"f3", gtopt::FileSched>,
      json_variant<"f4", variant_t, jvtl_RealFieldSched>,
      json_variant_null<"f5", std::optional<variant_t>, jvtl_RealFieldSched>>;

  static auto to_json_data(MyClass const& mc)
  {
    return std::forward_as_tuple(mc.f1, mc.f2, mc.f3, mc.f4, mc.f5);
  }
};

}  // namespace daw::json

TEST_CASE("daw json gtopt basic types 1")
{
  const double f1 = 3.5;
  const std::vector<double> f2 = {1, 2, 3, 4};
  const gtopt::FileSched f3 = "f1";
  const variant_t f4 = f1;
  const std::optional<variant_t> f5;

  const MyClass mc0 = {f1, f2, f3, f4, f5};  // NOLINT

  const auto json_data_4 = daw::json::to_json(mc0);

  const std::string_view json_data = R"({
     "f1": 3.5,
     "f2": [1,2,3,4],
     "f3": "f1",
     "f4": 3.5
    })";

  //

  {
    const MyClass mc = daw::json::from_json<MyClass>(json_data_4);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
  {
    auto mc = daw::json::from_json<MyClass>(json_data);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
}

TEST_CASE("daw json gtopt basic types 2")
{
  const double f1 = 3.5;
  const std::vector<double> f2 = {1, 2, 3, 4};
  const gtopt::FileSched f3 = "f1";
  const variant_t f4 = f1;
  const std::optional<variant_t> f5 = f3;

  const MyClass mc0 = {f1, f2, f3, f4, f5};  // NOLINT

  const auto json_data_4 = daw::json::to_json(mc0);

  const std::string_view json_data = R"({
     "f1": 3.5,
     "f2": [1,2,3,4],
     "f3": "f1",
     "f4": 3.5,
     "f5": "f1"
    })";

  //

  {
    const auto mc = daw::json::from_json<MyClass>(json_data_4);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
  {
    const auto mc = daw::json::from_json<MyClass>(json_data);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
}

// === test_json_bus.cpp ===

TEST_CASE("Bus daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  const gtopt::Bus bus_a = daw::json::from_json<gtopt::Bus>(json_data);

  REQUIRE(bus_a.uid == 5);
  REQUIRE(bus_a.name == "CRUCERO");
}

TEST_CASE("Bus daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  const gtopt::Bus bus_a = daw::json::from_json<gtopt::Bus>(json_data);

  REQUIRE(bus_a.uid == 5);
  REQUIRE(bus_a.name == "CRUCERO");
}

TEST_CASE("Bus daw json test 3")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO",
    },{
    "uid":10,
    "name":"PTOMONTT",
    }])";

  std::vector<gtopt::Bus> bus_a =
      daw::json::from_json_array<gtopt::Bus>(json_data);

  REQUIRE(bus_a[0].uid == 5);
  REQUIRE(bus_a[0].name == "CRUCERO");

  REQUIRE(bus_a[1].uid == 10);
  REQUIRE(bus_a[1].name == "PTOMONTT");
}

TEST_CASE("Bus daw json test 4")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO",
    "active": [0,0,1,1]
    },{
    "uid":10,
    "name":"PTOMONTT",
    }])";

  std::vector<gtopt::Bus> bus_a =
      daw::json::from_json_array<gtopt::Bus>(json_data);

  REQUIRE(bus_a[0].uid == 5);
  REQUIRE(bus_a[0].name == "CRUCERO");

  REQUIRE(bus_a[1].uid == 10);
  REQUIRE(bus_a[1].name == "PTOMONTT");

  using gtopt::False;
  using gtopt::True;

  const std::vector<gtopt::IntBool> empty;
  const std::vector<gtopt::IntBool> active = {False, False, True, True};
  REQUIRE(std::get<std::vector<gtopt::IntBool>>(bus_a[0].active.value_or(empty))
          == active);
}

TEST_CASE("Bus with active property serialization")
{

  SUBCASE("With boolean active")
  {
    Bus bus(1, "test_bus");
    bus.active = True;

    auto json = daw::json::to_json(bus);
    Bus roundtrip = daw::json::from_json<Bus>(json);

    REQUIRE(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value()) == True);  // NOLINT
  }

  SUBCASE("With schedule active")
  {
    Bus bus(1, "test_bus");
    bus.active = std::vector<IntBool> {True, False, True, False};

    auto json = daw::json::to_json(bus);
    Bus roundtrip = daw::json::from_json<Bus>(json);

    REQUIRE(roundtrip.active.has_value());
    const auto& active =
        std::get<std::vector<IntBool>>(roundtrip.active.value());  // NOLINT
    REQUIRE(active.size() == 4);
    CHECK(active[0] == True);
    CHECK(active[1] == False);
    CHECK(active[2] == True);
    CHECK(active[3] == False);
  }
}

TEST_CASE("Bus with empty optional fields")
{

  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    "voltage":null,
    "reference_theta":null,
    "use_kirchhoff":null
    })";

  const Bus bus = daw::json::from_json<Bus>(json_data);

  CHECK(bus.uid == 5);
  CHECK(bus.name == "CRUCERO");
  CHECK(bus.voltage.has_value() == false);
  CHECK(bus.reference_theta.has_value() == false);
  CHECK(bus.use_kirchhoff.has_value() == false);
}

// === test_json_demand.cpp ===

TEST_CASE("Demand daw json test")
{
  using Uid = gtopt::Uid;

  const std::string_view json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300
    })";

  gtopt::Demand dem = daw::json::from_json<gtopt::Demand>(json_data);

  REQUIRE(dem.uid == 5);
  REQUIRE(dem.name == "GUACOLDA");
  REQUIRE(std::get<Uid>(dem.bus) == 10);
  if (dem.capacity) {
    REQUIRE(std::get<double>(dem.capacity.value()) == doctest::Approx(300));
  }
}

TEST_CASE("Demand daw json test 2")
{
  const std::string_view json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300
    })";

  gtopt::Demand dem = daw::json::from_json<gtopt::Demand>(json_data);

  REQUIRE(dem.uid == 5);
  REQUIRE(dem.name == "GUACOLDA");

  REQUIRE(std::get<gtopt::Uid>(dem.bus) == 10);
  if (dem.capacity) {
    REQUIRE(std::get<double>(dem.capacity.value()) == doctest::Approx(300));
  }
}

TEST_CASE("Demand daw json test 3")
{
  using Name = gtopt::Name;

  const std::string_view json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":"GOLDA",
    "capacity":300
    })";

  gtopt::Demand dem = daw::json::from_json<gtopt::Demand>(json_data);

  REQUIRE(dem.uid == 5);
  REQUIRE(dem.name == "GUACOLDA");
  REQUIRE(std::get<Name>(dem.bus) == "GOLDA");
  if (dem.capacity) {
    REQUIRE(std::get<double>(dem.capacity.value()) == doctest::Approx(300));
  }
}

TEST_CASE("Demand daw json test 4")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO",
    "active": [0,0,1,1],
    "bus":"GOLDA"
    },{
    "uid":10,
    "name":"PTOMONTT",
    "bus":"GOLDA",
    }])";

  std::vector<gtopt::Demand> demand_a =
      daw::json::from_json_array<gtopt::Demand>(json_data);

  REQUIRE(demand_a[0].uid == 5);
  REQUIRE(demand_a[0].name == "CRUCERO");

  REQUIRE(demand_a[1].uid == 10);
  REQUIRE(demand_a[1].name == "PTOMONTT");

  using gtopt::False;
  using gtopt::True;

  const std::vector<gtopt::IntBool> empty;
  const std::vector<gtopt::IntBool> active = {False, False, True, True};

  REQUIRE(
      std::get<std::vector<gtopt::IntBool>>(demand_a[0].active.value_or(empty))
      == active);
}

// === test_json_demand_profile.cpp ===


TEST_CASE("DemandProfile daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"PROFILE_A",
    "demand":10,
    "profile":0.85
  })";

  DemandProfile dp = daw::json::from_json<DemandProfile>(json_data);

  CHECK(dp.uid == 1);
  CHECK(dp.name == "PROFILE_A");
  CHECK_FALSE(dp.active.has_value());
  CHECK(std::get<Uid>(dp.demand) == 10);
  CHECK(std::get<double>(dp.profile) == doctest::Approx(0.85));
}

TEST_CASE("DemandProfile daw json test - with cost")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"PROFILE_B",
    "active":1,
    "demand":"DEMAND_REF",
    "profile":1.0,
    "scost":500.0
  })";

  DemandProfile dp = daw::json::from_json<DemandProfile>(json_data);

  CHECK(dp.uid == 2);
  CHECK(dp.name == "PROFILE_B");
  REQUIRE(dp.active.has_value());
  CHECK(std::get<IntBool>(dp.active.value()) == True);  // NOLINT
  CHECK(std::get<Name>(dp.demand) == "DEMAND_REF");
  REQUIRE(dp.scost.has_value());
  CHECK(std::get<double>(dp.scost.value())  // NOLINT
        == doctest::Approx(500.0));
}

TEST_CASE("DemandProfile array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"DP_A",
    "demand":10,
    "profile":0.5
  },{
    "uid":2,
    "name":"DP_B",
    "demand":20,
    "profile":0.9
  }])";

  std::vector<DemandProfile> profiles =
      daw::json::from_json_array<DemandProfile>(json_data);

  REQUIRE(profiles.size() == 2);
  CHECK(profiles[0].uid == 1);
  CHECK(profiles[0].name == "DP_A");
  CHECK(profiles[1].uid == 2);
  CHECK(profiles[1].name == "DP_B");
}

TEST_CASE("DemandProfile round-trip serialization")
{
  DemandProfile dp;
  dp.uid = 5;
  dp.name = "RT_PROFILE";
  dp.demand = Uid {42};
  dp.profile = 0.75;
  dp.scost = 100.0;

  auto json = daw::json::to_json(dp);
  DemandProfile roundtrip = daw::json::from_json<DemandProfile>(json);

  CHECK(roundtrip.uid == dp.uid);
  CHECK(roundtrip.name == dp.name);
  CHECK(std::get<Uid>(roundtrip.demand) == 42);
  CHECK(std::get<double>(roundtrip.profile) == doctest::Approx(0.75));
  REQUIRE(roundtrip.scost.has_value());
  CHECK(std::get<double>(roundtrip.scost.value())  // NOLINT
        == doctest::Approx(100.0));
}

// === test_json_filtration.cpp ===


TEST_CASE("Filtration daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20,
    "slope":1.5,
    "constant":100.0
  })";

  gtopt::Filtration filt = daw::json::from_json<gtopt::Filtration>(json_data);

  REQUIRE(filt.uid == 5);
  REQUIRE(filt.name == "FILTER_A");
  REQUIRE(std::get<Uid>(filt.waterway) == Uid(10));
  REQUIRE(std::get<Uid>(filt.reservoir) == 20);
  REQUIRE(filt.slope == 1.5);
  REQUIRE(filt.constant == 100.0);
}

TEST_CASE("Filtration daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  })";

  gtopt::Filtration filt = daw::json::from_json<gtopt::Filtration>(json_data);

  REQUIRE(filt.uid == 5);
  REQUIRE(filt.name == "FILTER_A");
  REQUIRE(std::get<Uid>(filt.waterway) == 10);
  REQUIRE(std::get<Uid>(filt.reservoir) == 20);
  REQUIRE(filt.slope == 0.0);  // default value
  REQUIRE(filt.constant == 0.0);  // default value
}

TEST_CASE("Filtration array json test")
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

  std::vector<gtopt::Filtration> filts =
      daw::json::from_json_array<gtopt::Filtration>(json_data);

  REQUIRE(filts[0].uid == 5);
  REQUIRE(filts[0].name == "FILTER_A");
  REQUIRE(std::get<Uid>(filts[0].waterway) == 10);
  REQUIRE(std::get<Uid>(filts[0].reservoir) == 20);

  REQUIRE(filts[1].uid == 15);
  REQUIRE(filts[1].name == "FILTER_B");
  REQUIRE(std::get<Uid>(filts[1].waterway) == 30);
  REQUIRE(std::get<Uid>(filts[1].reservoir) == 40);
  REQUIRE(filts[1].slope == 2.0);
  REQUIRE(filts[1].constant == 200.0);
}

TEST_CASE("Filtration with active property serialization")
{

  SUBCASE("With boolean active")
  {
    Filtration filt;
    filt.uid = 1;
    filt.name = "test_filt";
    filt.active = True;

    auto json = daw::json::to_json(filt);
    const Filtration roundtrip = daw::json::from_json<Filtration>(json);

    CHECK(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value_or(False)) == True);
  }

  SUBCASE("With schedule active")
  {
    Filtration filt;
    filt.uid = 1;
    filt.name = "test_filt";
    filt.active = std::vector<IntBool> {True, False, True, False};

    auto json = daw::json::to_json(filt);
    Filtration roundtrip = daw::json::from_json<Filtration>(json);

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

TEST_CASE("Filtration with empty optional fields")
{

  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  })";

  const Filtration filt = daw::json::from_json<Filtration>(json_data);

  CHECK(filt.uid == 5);
  CHECK(filt.name == "FILTER_A");
  CHECK(filt.active.has_value() == false);
  CHECK(filt.slope == 0.0);  // null defaults to 0.0
  CHECK(filt.constant == 0.0);  // null defaults to 0.0
}

// === test_json_flow.cpp ===


TEST_CASE("Flow JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "inflow",
    "direction": 1,
    "junction": 10,
    "discharge": 25.5
  })";

  const Flow flow = daw::json::from_json<Flow>(json_data);

  CHECK(flow.uid == 1);
  CHECK(flow.name == "inflow");
  REQUIRE(flow.direction.has_value());
  CHECK(flow.direction.value_or(0) == 1);
  CHECK(std::get<Uid>(flow.junction) == 10);
  CHECK(std::get<Real>(flow.discharge) == doctest::Approx(25.5));
}

TEST_CASE("Flow JSON default values")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "name": "outflow",
    "junction": 5,
    "discharge": 10.0
  })";

  const Flow flow = daw::json::from_json<Flow>(json_data);

  CHECK(flow.uid == 2);
  CHECK(flow.name == "outflow");
  // direction defaults to 1 (input) when not specified
}

TEST_CASE("Flow JSON round-trip serialization")
{
  Flow original;
  original.uid = 7;
  original.name = "nat_inflow";
  original.direction = -1;
  original.junction = Uid {3};
  original.discharge = 42.0;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Flow roundtrip = daw::json::from_json<Flow>(json);  // NOLINT
  CHECK(roundtrip.uid == 7);
  CHECK(roundtrip.name == "nat_inflow");
  REQUIRE(roundtrip.direction.has_value());
  CHECK(roundtrip.direction.value_or(0) == -1);
  CHECK(std::get<Uid>(roundtrip.junction) == 3);
  CHECK(std::get<Real>(roundtrip.discharge) == doctest::Approx(42.0));
}

TEST_CASE("Flow JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "name": "f1", "junction": 1, "discharge": 10.0},
    {"uid": 2, "name": "f2", "direction": -1, "junction": 2, "discharge": 5.0}
  ])";

  const auto flows = daw::json::from_json_array<Flow>(json_data);

  REQUIRE(flows.size() == 2);
  CHECK(flows[0].uid == 1);
  CHECK(flows[1].uid == 2);
  REQUIRE(flows[1].direction.has_value());
  CHECK(flows[1].direction.value_or(0) == -1);
}

TEST_CASE("Flow is_input method")
{
  Flow flow_in;
  flow_in.direction = 1;
  CHECK(flow_in.is_input());

  Flow flow_out;
  flow_out.direction = -1;
  CHECK_FALSE(flow_out.is_input());

  Flow flow_default;
  flow_default.direction = std::nullopt;
  CHECK(flow_default.is_input());  // defaults to input (1)
}

// === test_json_generator.cpp ===

TEST_CASE("Generator daw json test 1")
{
  using Uid = gtopt::Uid;

  std::string_view test_001_t_json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300,
    "pmin":0,
    "pmax":275.5
    })";

  gtopt::Generator gen =
      daw::json::from_json<gtopt::Generator>(test_001_t_json_data);

  REQUIRE(gen.uid == 5);
  REQUIRE(gen.name == "GUACOLDA");
  REQUIRE(std::get<Uid>(gen.bus) == 10);
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value_or(-1.0))
          == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));
}

TEST_CASE("Generator daw json test 2")
{
  using Name = gtopt::Name;

  std::string_view test_001_t_json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":"GUACOLDA",
    "capacity":300,
    "pmin":0,
    "pmax":275.5
    })";

  gtopt::Generator gen =
      daw::json::from_json<gtopt::Generator>(test_001_t_json_data);

  REQUIRE(gen.uid == 5);
  REQUIRE(gen.name == "GUACOLDA");
  REQUIRE(std::get<Name>(gen.bus) == "GUACOLDA");
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value_or(-1.0))
          == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));
}

// === test_json_generator_profile.cpp ===


TEST_CASE("GeneratorProfile daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"GPROFILE_A",
    "generator":10,
    "profile":0.95
  })";

  GeneratorProfile gp = daw::json::from_json<GeneratorProfile>(json_data);

  CHECK(gp.uid == 1);
  CHECK(gp.name == "GPROFILE_A");
  CHECK_FALSE(gp.active.has_value());
  CHECK(std::get<Uid>(gp.generator) == 10);
  CHECK(std::get<double>(gp.profile) == doctest::Approx(0.95));
}

TEST_CASE("GeneratorProfile daw json test - with cost and active")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"GPROFILE_B",
    "active":1,
    "generator":"GEN_REF",
    "profile":1.0,
    "scost":250.0
  })";

  GeneratorProfile gp = daw::json::from_json<GeneratorProfile>(json_data);

  CHECK(gp.uid == 2);
  CHECK(gp.name == "GPROFILE_B");
  REQUIRE(gp.active.has_value());
  CHECK(std::get<IntBool>(gp.active.value()) == True);  // NOLINT
  CHECK(std::get<Name>(gp.generator) == "GEN_REF");
  REQUIRE(gp.scost.has_value());
  CHECK(std::get<double>(gp.scost.value())  // NOLINT
        == doctest::Approx(250.0));
}

TEST_CASE("GeneratorProfile array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"GP_A",
    "generator":10,
    "profile":0.5
  },{
    "uid":2,
    "name":"GP_B",
    "generator":20,
    "profile":0.9
  }])";

  std::vector<GeneratorProfile> profiles =
      daw::json::from_json_array<GeneratorProfile>(json_data);

  REQUIRE(profiles.size() == 2);
  CHECK(profiles[0].uid == 1);
  CHECK(profiles[0].name == "GP_A");
  CHECK(profiles[1].uid == 2);
  CHECK(profiles[1].name == "GP_B");
}

TEST_CASE("GeneratorProfile round-trip serialization")
{
  GeneratorProfile gp;
  gp.uid = 7;
  gp.name = "RT_GPROFILE";
  gp.generator = Uid {99};
  gp.profile = 0.80;
  gp.scost = 300.0;

  auto json = daw::json::to_json(gp);
  GeneratorProfile roundtrip = daw::json::from_json<GeneratorProfile>(json);

  CHECK(roundtrip.uid == gp.uid);
  CHECK(roundtrip.name == gp.name);
  CHECK(std::get<Uid>(roundtrip.generator) == 99);
  CHECK(std::get<double>(roundtrip.profile) == doctest::Approx(0.80));
  REQUIRE(roundtrip.scost.has_value());
  CHECK(std::get<double>(roundtrip.scost.value()) ==  // NOLINT
        doctest::Approx(300.0));
}

// === test_json_junction.cpp ===

TEST_CASE("Junction JSON basic parsing")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO"
  })";

  const gtopt::Junction junction =
      daw::json::from_json<gtopt::Junction>(json_data);

  REQUIRE(junction.uid == 5);
  REQUIRE(junction.name == "CRUCERO");
  REQUIRE_FALSE(junction.drain.has_value());
}

TEST_CASE("Junction JSON with drain")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    "drain":true
  })";

  const gtopt::Junction junction =
      daw::json::from_json<gtopt::Junction>(json_data);

  REQUIRE(junction.uid == 5);
  REQUIRE(junction.name == "CRUCERO");
  REQUIRE(junction.drain.has_value());
  REQUIRE(junction.drain.value_or(false) == true);
}

TEST_CASE("Junction JSON array parsing")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO"
  },{
    "uid":10,
    "name":"PTOMONTT",
    "drain":false
  }])";

  std::vector<gtopt::Junction> junctions =
      daw::json::from_json_array<gtopt::Junction>(json_data);

  REQUIRE(junctions.size() == 2);
  REQUIRE(junctions[0].uid == 5);
  REQUIRE(junctions[0].name == "CRUCERO");
  REQUIRE_FALSE(junctions[0].drain.has_value());

  REQUIRE(junctions[1].uid == 10);
  REQUIRE(junctions[1].name == "PTOMONTT");
  REQUIRE(junctions[1].drain.has_value());
  REQUIRE(junctions[1].drain.value_or(true) == false);
}

TEST_CASE("Junction JSON with active schedule")
{

  std::string_view json_data = R"({
    "uid":1,
    "name":"SCHED_JUNC",
    "active":[1,0,1,0]
  })";

  Junction junction = daw::json::from_json<Junction>(json_data);

  REQUIRE(junction.active.has_value());
  const auto& active =
      std::get<std::vector<IntBool>>(junction.active.value());  // NOLINT
  CHECK(active.size() == 4);
  CHECK(active[0] == True);
  CHECK(active[1] == False);
  CHECK(active[2] == True);
  CHECK(active[3] == False);
}

TEST_CASE("Junction JSON roundtrip serialization")
{
  gtopt::Junction original;
  original.uid = 7;
  original.name = "ROUNDTRIP";
  original.drain = true;

  auto json = daw::json::to_json(original);
  gtopt::Junction roundtrip =
      daw::json::from_json<gtopt::Junction>(json);  // NOLINT

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(roundtrip.drain.has_value());
  REQUIRE(roundtrip.drain.value() == true);  // NOLINT
}

// === test_json_line.cpp ===

TEST_CASE("Line JSON basic parsing")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20
  })";

  gtopt::Line line = daw::json::from_json<gtopt::Line>(json_data);

  REQUIRE(line.uid == 5);
  REQUIRE(line.name == "LINE_1");
  REQUIRE(std::get<gtopt::Uid>(line.bus_a) == 10);
  REQUIRE(std::get<gtopt::Uid>(line.bus_b) == 20);
  REQUIRE_FALSE(line.capacity.has_value());
}

TEST_CASE("Line JSON with optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20,
    "voltage":230.0,
    "resistance":0.05,
    "reactance":0.15,
    "capacity":200.0
  })";

  const gtopt::Line line = daw::json::from_json<gtopt::Line>(json_data);

  REQUIRE(line.uid == 5);
  REQUIRE(line.name == "LINE_1");
  REQUIRE(line.voltage.has_value());
  REQUIRE(line.resistance.has_value());
  REQUIRE(line.reactance.has_value());
  REQUIRE(line.capacity.has_value());
}

TEST_CASE("Line JSON array parsing")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20
  },{
    "uid":6,
    "name":"LINE_2",
    "bus_a":20,
    "bus_b":30,
    "capacity":200.0
  }])";

  std::vector<gtopt::Line> lines =
      daw::json::from_json_array<gtopt::Line>(json_data);

  REQUIRE(lines.size() == 2);
  REQUIRE(lines[0].uid == 5);
  REQUIRE(lines[0].name == "LINE_1");
  REQUIRE_FALSE(lines[0].capacity.has_value());

  REQUIRE(lines[1].uid == 6);
  REQUIRE(lines[1].name == "LINE_2");
  REQUIRE(lines[1].capacity.has_value());
}

TEST_CASE("Line JSON with active schedule")
{

  std::string_view json_data = R"({
    "uid":1,
    "name":"LINE_1",
    "bus_a":1,
    "bus_b":2,
    "active":[1,0,1,0]
  })";

  Line line = daw::json::from_json<Line>(json_data);

  CHECK(line.active.has_value());
  if (line.active.has_value()) {
    const auto& active = std::get<std::vector<IntBool>>(line.active.value());
    REQUIRE(active.size() == 4);
    CHECK(active[0] == True);
    CHECK(active[1] == False);
    CHECK(active[2] == True);
    CHECK(active[3] == False);
  }
}

TEST_CASE("Line JSON roundtrip serialization")
{
  gtopt::Line original;
  original.uid = 7;
  original.name = "ROUNDTRIP";
  original.bus_a = gtopt::SingleId(static_cast<gtopt::Uid>(1));
  original.bus_b = gtopt::SingleId(static_cast<gtopt::Uid>(2));
  original.capacity = 100.0;

  auto json = daw::json::to_json(original);
  gtopt::Line roundtrip = daw::json::from_json<gtopt::Line>(json);

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(std::get<gtopt::Uid>(roundtrip.bus_a) == 1);
  REQUIRE(std::get<gtopt::Uid>(roundtrip.bus_b) == 2);
  CHECK(roundtrip.capacity.has_value());
  CHECK(std::get<double>(roundtrip.capacity.value_or(-1.0)) == 100.0);
}

TEST_CASE("Line with empty optional fields")
{

  std::string_view json_data = R"({
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20,
    "voltage":null,
    "resistance":null,
    "reactance":null,
    "capacity":null
  })";

  const Line line = daw::json::from_json<Line>(json_data);

  CHECK(line.uid == 5);
  CHECK(line.name == "LINE_1");
  CHECK(line.voltage.has_value() == false);
  CHECK(line.resistance.has_value() == false);
  CHECK(line.reactance.has_value() == false);
  CHECK(line.capacity.has_value() == false);
}

// === test_json_optimization.cpp ===

TEST_CASE("JSON Planning - Serialize empty")
{

  // Create empty planning
  const Planning opt {};

  // Serialize to JSON
  const auto json_data = daw::json::to_json(opt);

  // Verify JSON structure
  CHECK(json_data.find("\"options\":") != std::string::npos);
  CHECK(json_data.find("\"simulation\":") != std::string::npos);
  CHECK(json_data.find("\"system\":") != std::string::npos);
}

TEST_CASE("JSON Planning - Round trip serialization")
{

  // Create planning with components
  const Options options {};
  const Simulation simulation {};

  // Create minimal system
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> gen_array = {
      {.uid = Uid {1}, .name = "g1", .bus = Uid {1}},
  };
  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .generator_array = gen_array,
  };

  Planning original {
      .options = options,
      .simulation = simulation,
      .system = system,
  };

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Deserialize back to an object
  const auto deserialized = daw::json::from_json<Planning>(json_data);

  // Verify the deserialized object matches the original
  CHECK(deserialized.system.name == original.system.name);

  REQUIRE(deserialized.system.bus_array.size()
          == original.system.bus_array.size());
  CHECK(deserialized.system.bus_array[0].name
        == original.system.bus_array[0].name);

  REQUIRE(deserialized.system.generator_array.size()
          == original.system.generator_array.size());
  CHECK(deserialized.system.generator_array[0].name
        == original.system.generator_array[0].name);
}

TEST_CASE("JSON Planning - Partial filled objects")
{

  // Create planning with only some components filled
  const Planning opt1 {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {},  // NOLINT
  };

  // Serialize and deserialize
  const auto json_data1 = daw::json::to_json(opt1);
  const auto deserialized1 = daw::json::from_json<Planning>(json_data1);

  // Check that partial filling works correctly
  CHECK(deserialized1.system.name.empty());

  // Another test with different components filled
  const Planning opt2 {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {.name = "TestSystem"},
  };

  // Serialize and deserialize
  const auto json_data2 = daw::json::to_json(opt2);
  const auto deserialized2 = daw::json::from_json<Planning>(json_data2);

  // Check that partial filling works correctly
  CHECK(deserialized2.system.name == "TestSystem");
}

// === test_json_options.cpp ===

TEST_CASE("json_options - Deserialization of Options from JSON")
{

  // JSON string representing Options
  const std::string json_string = R"({
    "input_directory": "input_dir",
    "input_format": "json",
    "demand_fail_cost": 1000.0,
    "reserve_fail_cost": 500.0,
    "use_line_losses": true,
    "use_kirchhoff": false,
    "use_single_bus": true,
    "kirchhoff_threshold": 0.01,
    "scale_objective": 100.0,
    "scale_theta": 10.0,
    "output_directory": "output_dir",
    "output_format": "csv",
    "compression_format": "gzip",
    "use_lp_names": true,
    "use_uid_fname": false,
    "annual_discount_rate": 0.05
  })";

  // Deserialize from JSON
  const auto options = daw::json::from_json<Options>(json_string);

  // Check all fields are correctly deserialized
  REQUIRE(options.input_directory.has_value());
  if (options.input_directory) {
    CHECK(*options.input_directory == "input_dir");
  }

  REQUIRE(options.input_format.has_value());
  if (options.input_format) {
    CHECK(*options.input_format == "json");
  }

  REQUIRE(options.demand_fail_cost.has_value());
  if (options.demand_fail_cost) {
    CHECK(*options.demand_fail_cost == doctest::Approx(1000.0));
  }

  REQUIRE(options.reserve_fail_cost.has_value());
  if (options.reserve_fail_cost) {
    CHECK(options.reserve_fail_cost.value() == doctest::Approx(500.0));
  }

  REQUIRE(options.use_line_losses.has_value());
  if (options.use_line_losses) {
    CHECK(*options.use_line_losses == true);
  }

  REQUIRE(options.use_kirchhoff.has_value());
  if (options.use_kirchhoff) {
    CHECK(*options.use_kirchhoff == false);
  }

  REQUIRE(options.use_single_bus.has_value());
  if (options.use_single_bus) {
    CHECK(*options.use_single_bus == true);
  }

  REQUIRE(options.kirchhoff_threshold.has_value());
  if (options.kirchhoff_threshold) {
    CHECK(*options.kirchhoff_threshold == doctest::Approx(0.01));
  }

  REQUIRE(options.scale_objective.has_value());
  if (options.scale_objective) {
    CHECK(*options.scale_objective == doctest::Approx(100.0));
  }

  REQUIRE(options.scale_theta.has_value());
  if (options.scale_theta) {
    CHECK(*options.scale_theta == doctest::Approx(10.0));
  }

  REQUIRE(options.output_directory.has_value());
  if (options.output_directory) {
    CHECK(*options.output_directory == "output_dir");
  }

  REQUIRE(options.output_format.has_value());
  if (options.output_format) {
    CHECK(*options.output_format == "csv");
  }

  REQUIRE(options.compression_format.has_value());
  if (options.compression_format) {
    CHECK(*options.compression_format == "gzip");
  }

  REQUIRE(options.use_lp_names.has_value());
  if (options.use_lp_names) {
    CHECK(*options.use_lp_names == true);
  }

  REQUIRE(options.use_uid_fname.has_value());
  if (options.use_uid_fname) {
    CHECK(*options.use_uid_fname == false);
  }

  REQUIRE(options.annual_discount_rate.has_value());
  if (options.annual_discount_rate) {
    CHECK(*options.annual_discount_rate == doctest::Approx(0.05));
  }
}

TEST_CASE(
    "json_options - Deserialization with missing fields (should use nulls)")
{

  // JSON string with only some fields
  const std::string json_string = R"({
    "input_directory": "input_dir",
    "use_kirchhoff": true,
    "output_directory": "output_dir"
  })";

  // Deserialize from JSON
  const auto options = daw::json::from_json<Options>(json_string);

  // Check populated fields
  REQUIRE(options.input_directory.has_value());
  if (options.input_directory) {
    CHECK(*options.input_directory == "input_dir");
  }

  REQUIRE(options.use_kirchhoff.has_value());
  if (options.use_kirchhoff) {
    CHECK(*options.use_kirchhoff == true);
  }

  REQUIRE(options.output_directory.has_value());
  if (options.output_directory) {
    CHECK(*options.output_directory == "output_dir");
  }

  // Check unpopulated fields
  CHECK_FALSE(options.input_format.has_value());
  CHECK_FALSE(options.demand_fail_cost.has_value());
  CHECK_FALSE(options.reserve_fail_cost.has_value());
  CHECK_FALSE(options.use_line_losses.has_value());
  CHECK_FALSE(options.use_single_bus.has_value());
  CHECK_FALSE(options.kirchhoff_threshold.has_value());
  CHECK_FALSE(options.scale_objective.has_value());
  CHECK_FALSE(options.scale_theta.has_value());
  CHECK_FALSE(options.output_format.has_value());
  CHECK_FALSE(options.compression_format.has_value());
  CHECK_FALSE(options.use_lp_names.has_value());
  CHECK_FALSE(options.use_uid_fname.has_value());
  CHECK_FALSE(options.annual_discount_rate.has_value());
}

TEST_CASE("json_options - Round-trip serialization and deserialization")
{

  // Create original Options
  Options original {
      .input_directory = "input_dir",
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = true,
      .scale_objective = 100.0,
      .output_directory = "output_dir",
      .use_lp_names = false,
  };

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Deserialize back to Options
  const auto deserialized = daw::json::from_json<Options>(json_data);

  // Check all fields match
  CHECK(deserialized.input_directory == original.input_directory);
  CHECK(deserialized.demand_fail_cost == original.demand_fail_cost);
  CHECK(deserialized.use_kirchhoff == original.use_kirchhoff);
  CHECK(deserialized.scale_objective == original.scale_objective);
  CHECK(deserialized.output_directory == original.output_directory);
  CHECK(deserialized.use_lp_names == original.use_lp_names);

  // Check that unpopulated fields remain empty
  CHECK_FALSE(deserialized.input_format.has_value());
  CHECK_FALSE(deserialized.reserve_fail_cost.has_value());
  CHECK_FALSE(deserialized.use_line_losses.has_value());
  CHECK_FALSE(deserialized.use_single_bus.has_value());
  CHECK_FALSE(deserialized.kirchhoff_threshold.has_value());
  CHECK_FALSE(deserialized.scale_theta.has_value());
  CHECK_FALSE(deserialized.output_format.has_value());
  CHECK_FALSE(deserialized.compression_format.has_value());
  CHECK_FALSE(deserialized.use_uid_fname.has_value());
  CHECK_FALSE(deserialized.annual_discount_rate.has_value());
}

// === test_json_planning.cpp ===


TEST_CASE("Planning daw json test 1 - basic parsing")
{
  const std::string_view json_data = R"({
    "options": {
      "input_directory": "data",
      "use_kirchhoff": true
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 2}],
      "stage_array": [],
      "scenario_array": []
    },
    "system": {
      "name": "TEST",
      "bus_array": [{"uid": 5, "name": "BUS1"}],
      "generator_array": []
    }
  })";

  gtopt::Planning plan = daw::json::from_json<gtopt::Planning>(json_data);

  if (plan.options.input_directory) {
    CHECK(plan.options.input_directory.value() == "data");
  }
  if (plan.options.use_kirchhoff) {
    CHECK(plan.options.use_kirchhoff.value() == true);
  }

  REQUIRE(plan.simulation.block_array.size() == 1);
  REQUIRE(plan.simulation.block_array[0].uid == 1);
  REQUIRE(plan.simulation.block_array[0].duration == 2);
  REQUIRE(plan.simulation.stage_array.empty());
  REQUIRE(plan.simulation.scenario_array.empty());

  REQUIRE(plan.system.name == "TEST");
  REQUIRE(plan.system.bus_array.size() == 1);
  REQUIRE(plan.system.bus_array[0].uid == 5);
  REQUIRE(plan.system.bus_array[0].name == "BUS1");
  REQUIRE(plan.system.generator_array.empty());
}

TEST_CASE("Planning daw json test 2 - large scale")
{
  const size_t size = 1000;
  std::vector<gtopt::Bus> bus_array(size);
  std::vector<gtopt::Generator> generator_array(size);

  {
    gtopt::Uid uid = 0;
    for (size_t i = 0; i < size; ++i) {
      const gtopt::SingleId bus {uid};
      bus_array[i] = {.uid = uid, .name = "bus"};
      generator_array[i] = {
          .uid = uid,
          .name = "gen",
          .bus = bus,
          .pmin = 0.0,
          .pmax = 300.0,
          .gcost = 50.0,
          .capacity = 300.0,
      };
      ++uid;
    }
  }

  const Planning planning {
      .options = Options {.input_directory = "large_test"},
      .simulation = Simulation(),
      .system =
          System {
              .name = "large_system",
              .bus_array = bus_array,
              .generator_array = generator_array,
          },
  };

  auto json_data = daw::json::to_json(planning);
  gtopt::Planning parsed_plan =
      daw::json::from_json<gtopt::Planning>(json_data);

  if (parsed_plan.options.input_directory) {
    REQUIRE(parsed_plan.options.input_directory.value() == "large_test");
  }
  REQUIRE(parsed_plan.system.name == "large_system");
  REQUIRE(parsed_plan.system.bus_array.size() == size);
  REQUIRE(parsed_plan.system.generator_array.size() == size);

  // Verify bus and generator data
  gtopt::Uid uid = 0;
  for (size_t i = 0; i < size; ++i) {
    gtopt::SingleId bus {uid};
    REQUIRE(parsed_plan.system.bus_array[i].uid == uid);
    REQUIRE(parsed_plan.system.generator_array[i].uid == uid);
    REQUIRE(parsed_plan.system.generator_array[i].bus == bus);
    if (parsed_plan.system.generator_array[i].pmax) {
      REQUIRE(std::get<double>(
                  parsed_plan.system.generator_array[i].pmax.value_or(-1.0))
              == doctest::Approx(300.0));
    }
    ++uid;
  }
}

// === test_json_reserve_provision.cpp ===


TEST_CASE("ReserveProvision daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"RPROV_A",
    "generator":10,
    "reserve_zones":"ZONE_1,ZONE_2",
    "urmax":50.0,
    "drmax":30.0
  })";

  ReserveProvision rp =
      daw::json::from_json<ReserveProvision>(json_data);  // NOLINT

  CHECK(rp.uid == 1);
  CHECK(rp.name == "RPROV_A");
  CHECK_FALSE(rp.active.has_value());
  CHECK(std::get<Uid>(rp.generator) == 10);
  CHECK(rp.reserve_zones == "ZONE_1,ZONE_2");

  REQUIRE(rp.urmax.has_value());
  CHECK(std::get<double>(rp.urmax.value())  // NOLINT
        == doctest::Approx(50.0));
  REQUIRE(rp.drmax.has_value());
  CHECK(std::get<double>(rp.drmax.value())  // NOLINT
        == doctest::Approx(30.0));
}

TEST_CASE("ReserveProvision daw json test - with factors and costs")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"RPROV_B",
    "active":1,
    "generator":"GEN_COAL",
    "reserve_zones":"ZONE_A",
    "urmax":100.0,
    "drmax":80.0,
    "ur_capacity_factor":0.9,
    "dr_capacity_factor":0.8,
    "ur_provision_factor":0.95,
    "dr_provision_factor":0.85,
    "urcost":1000.0,
    "drcost":800.0
  })";

  ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);

  CHECK(rp.uid == 2);
  CHECK(rp.name == "RPROV_B");
  REQUIRE(rp.active.has_value());
  CHECK(std::get<IntBool>(rp.active.value()) == True);  // NOLINT
  CHECK(std::get<Name>(rp.generator) == "GEN_COAL");
  CHECK(rp.reserve_zones == "ZONE_A");

  REQUIRE(rp.ur_capacity_factor.has_value());
  CHECK(std::get<double>(rp.ur_capacity_factor.value())  // NOLINT
        == doctest::Approx(0.9));
  REQUIRE(rp.dr_capacity_factor.has_value());
  CHECK(std::get<double>(rp.dr_capacity_factor.value())  // NOLINT
        == doctest::Approx(0.8));
  REQUIRE(rp.ur_provision_factor.has_value());
  CHECK(std::get<double>(rp.ur_provision_factor.value())  // NOLINT
        == doctest::Approx(0.95));
  REQUIRE(rp.dr_provision_factor.has_value());
  CHECK(std::get<double>(rp.dr_provision_factor.value())  // NOLINT
        == doctest::Approx(0.85));

  REQUIRE(rp.urcost.has_value());
  CHECK(std::get<double>(rp.urcost.value())  // NOLINT
        == doctest::Approx(1000.0));
  REQUIRE(rp.drcost.has_value());
  CHECK(std::get<double>(rp.drcost.value())  // NOLINT
        == doctest::Approx(800.0));
}

TEST_CASE("ReserveProvision daw json test - minimal fields")
{
  std::string_view json_data = R"({
    "uid":3,
    "name":"RPROV_MINIMAL",
    "generator":5,
    "reserve_zones":"Z1"
  })";

  const ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);

  CHECK(rp.uid == 3);
  CHECK(rp.name == "RPROV_MINIMAL");
  CHECK_FALSE(rp.active.has_value());
  CHECK_FALSE(rp.urmax.has_value());
  CHECK_FALSE(rp.drmax.has_value());
  CHECK_FALSE(rp.ur_capacity_factor.has_value());
  CHECK_FALSE(rp.dr_capacity_factor.has_value());
  CHECK_FALSE(rp.ur_provision_factor.has_value());
  CHECK_FALSE(rp.dr_provision_factor.has_value());
  CHECK_FALSE(rp.urcost.has_value());
  CHECK_FALSE(rp.drcost.has_value());
}

TEST_CASE("ReserveProvision array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"RPROV_A",
    "generator":10,
    "reserve_zones":"Z1"
  },{
    "uid":2,
    "name":"RPROV_B",
    "generator":20,
    "reserve_zones":"Z1,Z2"
  }])";

  std::vector<ReserveProvision> provisions =
      daw::json::from_json_array<ReserveProvision>(json_data);

  REQUIRE(provisions.size() == 2);
  CHECK(provisions[0].uid == 1);
  CHECK(provisions[0].name == "RPROV_A");
  CHECK(provisions[1].uid == 2);
  CHECK(provisions[1].name == "RPROV_B");
  CHECK(provisions[1].reserve_zones == "Z1,Z2");
}

TEST_CASE("ReserveProvision round-trip serialization")
{
  ReserveProvision rp;
  rp.uid = 10;
  rp.name = "RT_RPROV";
  rp.active = True;
  rp.generator = Uid {42};
  rp.reserve_zones = "ZONE_X,ZONE_Y";
  rp.urmax = 200.0;
  rp.drmax = 150.0;
  rp.urcost = 5000.0;
  rp.drcost = 3000.0;

  auto json = daw::json::to_json(rp);
  ReserveProvision roundtrip = daw::json::from_json<ReserveProvision>(json);

  CHECK(roundtrip.uid == rp.uid);
  CHECK(roundtrip.name == rp.name);
  CHECK(std::get<Uid>(roundtrip.generator) == 42);
  CHECK(roundtrip.reserve_zones == "ZONE_X,ZONE_Y");
  REQUIRE(roundtrip.urmax.has_value());
  CHECK(std::get<double>(roundtrip.urmax.value())  // NOLINT
        == doctest::Approx(200.0));
  REQUIRE(roundtrip.drmax.has_value());
  CHECK(std::get<double>(roundtrip.drmax.value_or(0.0))
        == doctest::Approx(150.0));
  REQUIRE(roundtrip.urcost.has_value());
  CHECK(std::get<double>(roundtrip.urcost.value_or(0.0))
        == doctest::Approx(5000.0));
  REQUIRE(roundtrip.drcost.has_value());
  CHECK(std::get<double>(roundtrip.drcost.value_or(0.0))
        == doctest::Approx(3000.0));
}

// === test_json_reserve_zone.cpp ===


TEST_CASE("ReserveZone daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"ZONE_A",
    "urreq":100.0,
    "drreq":50.0
  })";

  ReserveZone rz = daw::json::from_json<ReserveZone>(json_data);

  CHECK(rz.uid == 1);
  CHECK(rz.name == "ZONE_A");
  CHECK_FALSE(rz.active.has_value());

  REQUIRE(rz.urreq.has_value());
  CHECK(std::get<double>(rz.urreq.value()) ==  // NOLINT
        doctest::Approx(100.0));

  REQUIRE(rz.drreq.has_value());
  CHECK(std::get<double>(rz.drreq.value()) ==  // NOLINT
        doctest::Approx(50.0));
}

TEST_CASE("ReserveZone daw json test - with costs")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"ZONE_B",
    "active":1,
    "urreq":200.0,
    "drreq":100.0,
    "urcost":5000.0,
    "drcost":3000.0
  })";

  ReserveZone rz = daw::json::from_json<ReserveZone>(json_data);

  CHECK(rz.uid == 2);
  CHECK(rz.name == "ZONE_B");
  REQUIRE(rz.active.has_value());
  CHECK(std::get<IntBool>(rz.active.value()) == True);  // NOLINT

  REQUIRE(rz.urcost.has_value());
  CHECK(std::get<double>(rz.urcost.value())  // NOLINT
        == doctest::Approx(5000.0));

  REQUIRE(rz.drcost.has_value());
  CHECK(std::get<double>(rz.drcost.value())  // NOLINT
        == doctest::Approx(3000.0));
}

TEST_CASE("ReserveZone daw json test - minimal fields")
{
  std::string_view json_data = R"({
    "uid":3,
    "name":"ZONE_MINIMAL"
  })";

  const ReserveZone rz = daw::json::from_json<ReserveZone>(json_data);

  CHECK(rz.uid == 3);
  CHECK(rz.name == "ZONE_MINIMAL");
  CHECK_FALSE(rz.active.has_value());
  CHECK_FALSE(rz.urreq.has_value());
  CHECK_FALSE(rz.drreq.has_value());
  CHECK_FALSE(rz.urcost.has_value());
  CHECK_FALSE(rz.drcost.has_value());
}

TEST_CASE("ReserveZone array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"ZONE_A",
    "urreq":100.0
  },{
    "uid":2,
    "name":"ZONE_B",
    "drreq":50.0
  }])";

  std::vector<ReserveZone> zones =
      daw::json::from_json_array<ReserveZone>(json_data);

  REQUIRE(zones.size() == 2);
  CHECK(zones[0].uid == 1);
  CHECK(zones[0].name == "ZONE_A");
  CHECK(zones[1].uid == 2);
  CHECK(zones[1].name == "ZONE_B");
}

TEST_CASE("ReserveZone round-trip serialization")
{
  ReserveZone rz;
  rz.uid = 10;
  rz.name = "RT_ZONE";
  rz.active = True;
  rz.urreq = 150.0;
  rz.drreq = 75.0;
  rz.urcost = 4000.0;
  rz.drcost = 2000.0;

  auto json = daw::json::to_json(rz);
  ReserveZone roundtrip = daw::json::from_json<ReserveZone>(json);

  CHECK(roundtrip.uid == rz.uid);
  CHECK(roundtrip.name == rz.name);
  REQUIRE(roundtrip.urreq.has_value());
  CHECK(std::get<double>(roundtrip.urreq.value())  // NOLINT
        == doctest::Approx(150.0));
  REQUIRE(roundtrip.drreq.has_value());
  CHECK(std::get<double>(roundtrip.drreq.value())  // NOLINT
        == doctest::Approx(75.0));
}

// === test_json_reservoir.cpp ===


TEST_CASE("Reservoir basic fields deserialization")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "active":1,
    "junction":456,
    "capacity":null,
    "annual_loss":null,
    "vmin":null,
    "vmax":null,
    "vcost":null,
    "vini":null,
    "vfin":null
  })";

  Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 123);
  CHECK(res.name == "TestReservoir");
  CHECK(res.active.has_value());
  CHECK(std::get<IntBool>(res.active.value_or(False)));
  CHECK(std::get<Uid>(res.junction) == 456);
  CHECK_FALSE(res.capacity.has_value());
  CHECK_FALSE(res.annual_loss.has_value());
  CHECK_FALSE(res.vmin.has_value());
  CHECK_FALSE(res.vmax.has_value());
  CHECK_FALSE(res.vcost.has_value());
  CHECK_FALSE(res.vini.has_value());
  CHECK_FALSE(res.vfin.has_value());
}

TEST_CASE("Reservoir optional fields deserialization")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "junction":12,
    "capacity":1.0,
    "annual_loss":0.05,
    "vmin":100.0,
    "vmax":1000.0,
    "vcost":5.0,
    "vini":500.0,
    "vfin":600.0
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);
  CHECK(res.capacity.has_value());
  CHECK(std::get<double>(res.capacity.value_or(-1.0)) == 1.0);
  CHECK(std::get<double>(res.annual_loss.value_or(-1.0)) == 0.05);
  CHECK(std::get<double>(res.vmin.value_or(-1.0)) == 100.0);
  CHECK(std::get<double>(res.vmax.value_or(-1.0)) == 1000.0);
  CHECK(std::get<double>(res.vcost.value_or(-1.0)) == 5.0);
  CHECK(res.vini.value_or(-1.0) == 500.0);
  CHECK(res.vfin.value_or(-1.0) == 600.0);
}

TEST_CASE("Reservoir array deserialization")
{
  std::string_view json_data = R"([{
    "uid":123,
    "name":"ReservoirA",
    "junction":456
  },{
    "uid":124,
    "name":"ReservoirB",
    "junction":457,
    "capacity":1.0,
    "vmax":1000.0
  }])";

  std::vector<Reservoir> reservoirs =
      daw::json::from_json_array<Reservoir>(json_data);

  CHECK(reservoirs.size() == 2);
  CHECK(reservoirs[0].uid == 123);
  CHECK(reservoirs[0].name == "ReservoirA");
  CHECK(std::get<Uid>(reservoirs[0].junction) == 456);

  CHECK(reservoirs[1].uid == 124);
  CHECK(reservoirs[1].name == "ReservoirB");
  CHECK(std::get<Uid>(reservoirs[1].junction) == 457);
  CHECK(std::get<double>(reservoirs[1].capacity.value_or(-1.0)) == 1.0);
  CHECK(std::get<double>(reservoirs[1].vmax.value_or(-1.0)) == 1000.0);
}

TEST_CASE("Reservoir roundtrip serialization")
{
  Reservoir original {
      .uid = Uid {123},
      .name = "TestReservoir",
      .active = true,
      .junction = SingleId {Uid {456}},
      .capacity = OptTRealFieldSched {1.0},
      .annual_loss = OptTRealFieldSched {0.05},
      .vmin = OptTRealFieldSched {100.0},
      .vmax = OptTRealFieldSched {1000.0},
      .vcost = OptTRealFieldSched {5.0},
      .vini = OptReal {500.0},
      .vfin = OptReal {600.0},
  };

  auto json = daw::json::to_json(original);

  // Verify JSON contains expected fields
  CHECK(json.find("\"uid\":123") != std::string::npos);
  CHECK(json.find("\"name\":\"TestReservoir\"") != std::string::npos);
  CHECK(json.find("\"active\":1") != std::string::npos);
  CHECK(json.find("\"capacity\":1") != std::string::npos);

  Reservoir roundtrip = daw::json::from_json<Reservoir>(json);

  CHECK(roundtrip.uid == original.uid);
  CHECK(roundtrip.name == original.name);
  CHECK(roundtrip.active.has_value());
  CHECK(original.active.has_value());
  CHECK(std::get<Uid>(roundtrip.junction) == std::get<Uid>(original.junction));
  CHECK(roundtrip.capacity.has_value());
  CHECK(roundtrip.capacity.value_or(-1.0) == original.capacity.value_or(-2.0));
  CHECK(roundtrip.annual_loss.has_value());
  CHECK(roundtrip.annual_loss.value_or(-1.0)
        == original.annual_loss.value_or(-2.0));
  CHECK(roundtrip.vmin.has_value());
  CHECK(roundtrip.vmin.value_or(-1.0) == original.vmin.value_or(-2.0));
  CHECK(roundtrip.vmax.has_value());
  CHECK(roundtrip.vmax.value_or(-1.0) == original.vmax.value_or(-2.0));
  CHECK(roundtrip.vcost.has_value());
  CHECK(roundtrip.vcost.value_or(-1.0) == original.vcost.value_or(-2.0));
  CHECK(roundtrip.vini.has_value());
  CHECK(roundtrip.vini.value_or(-1.0) == original.vini.value_or(-2.0));
  CHECK(roundtrip.vfin.has_value());
  CHECK(roundtrip.vfin.value_or(-1.0) == original.vfin.value_or(-2.0));
}

TEST_CASE("Reservoir with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "junction":456
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 123);
  CHECK(res.name == "TestReservoir");
  CHECK_FALSE(res.active.has_value());
  CHECK_FALSE(res.capacity.has_value());
  CHECK_FALSE(res.annual_loss.has_value());
  CHECK_FALSE(res.vmin.has_value());
  CHECK_FALSE(res.vmax.has_value());
  CHECK_FALSE(res.vcost.has_value());
  CHECK_FALSE(res.vini.has_value());
  CHECK_FALSE(res.vfin.has_value());
}

// === test_json_simulation.cpp ===

#ifdef NONE

TEST_CASE("Simulation daw json test 1")
{
  const std::string_view json_data = R"({
    "name":"SEN",
    "block_array":[{"uid": 1, "duration": 2}],
    "stage_array":[],
    "scenario_array":[]})";

  gtopt::Simulation sys = daw::json::from_json<gtopt::Simulation>(json_data);

  using gtopt::False;
  using gtopt::True;

  REQUIRE(sys.block_array.size() == 1);
  REQUIRE(sys.block_array[0].uid == 1);
  REQUIRE(sys.block_array[0].duration == 2);

  REQUIRE(sys.stage_array.size() == 0);
  REQUIRE(sys.scenario_array.size() == 0);
}

TEST_CASE("Simulation daw json test 3")
{
  const size_t size = 1000;

  std::vector<gtopt::Bus> bus_array(size);
  std::vector<gtopt::Generator> generator_array(size);
  const std::vector<gtopt::Demand> demand_array;
  const std::vector<gtopt::Line> line_array;

  {
    gtopt::Uid uid = 0;

    for (size_t i = 0; i < size; ++i) {
      const gtopt::SingleId bus {uid};
      bus_array[i] = {.uid = uid, .name = "bus"};
      generator_array[i] = {.uid = uid,
                            .name = "gen",
                            .bus = bus,
                            .pmin = 0.0,
                            .pmax = 300.0,
                            .gcost = 50.0,
                            .capacity = 300.0};
      ++uid;
    }
  }

  const gtopt::Simulation simulation {
      .block_array = {}, .stage_array = {}, .scenario_array = {}};

  auto json_data = daw::json::to_json(simulation);

  gtopt::Simulation sys;

  // BENCHMARK("read simulation")
  {
    sys = daw::json::from_json<gtopt::Simulation>(json_data);
    // return sys.name;
  };
}

#endif

// === test_json_system.cpp ===

TEST_CASE("System daw json test 1")
{
  const std::string_view json_data = R"({
    "name":"SEN",
    "bus_array":[{
       "uid":5,
       "name":"CRUCERO"
      },{
       "uid":10,
       "name":"PTOMONTT"
      }],
    "generator_array":[{
       "uid":5,
       "active":1,
       "name":"GUACOLDA",
       "bus":10,
       "capacity":300,
       "pmin":0,
       "pmax":275.5
     }],
    "line_array":[{
       "uid":1,
       "active":1,
       "name":"GUACOLDA-PTOMONTT",
       "bus_a":10,
       "bus_b":"PTOMONTT",
       "capacity":300,
       "voltage": 220,
       "resistance": 10,
       "reactance": 0.1,
     }],
    "demand_array":[{
       "uid":10,
       "name":"PTOMONTT",
       "bus":5,
       "capacity":100
     }]
})";

  gtopt::System sys = daw::json::from_json<gtopt::System>(json_data);

  using gtopt::False;
  using gtopt::True;

  REQUIRE(sys.name == "SEN");

  REQUIRE(sys.bus_array.size() == 2);
  REQUIRE(sys.bus_array[0].uid == 5);
  REQUIRE(sys.bus_array[0].name == "CRUCERO");

  REQUIRE(sys.bus_array[1].uid == 10);
  REQUIRE(sys.bus_array[1].name == "PTOMONTT");

  REQUIRE(sys.generator_array.size() == 1);
  REQUIRE(sys.generator_array[0].uid == 5);
  REQUIRE(
      std::get<gtopt::IntBool>(sys.generator_array[0].active.value_or(False))
      == True);
  REQUIRE(sys.generator_array[0].name == "GUACOLDA");
  REQUIRE(std::get<gtopt::Uid>(sys.generator_array[0].bus) == 10);

  const auto& gen = sys.generator_array[0];
  if (gen.pmin) {
    REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  }

  if (gen.capacity) {
    REQUIRE(std::get<double>(gen.capacity.value()) == doctest::Approx(300));
  }

  if (gen.pmax) {
    REQUIRE(std::get<double>(gen.pmax.value_or(-1.0))
            == doctest::Approx(275.5));
  }

  REQUIRE(sys.demand_array.size() == 1);
  REQUIRE(sys.demand_array[0].uid == 10);
  REQUIRE(std::get<gtopt::Uid>(sys.demand_array[0].bus) == 5);
  REQUIRE(sys.demand_array[0].name == "PTOMONTT");

  REQUIRE(std::get<double>(sys.demand_array[0].capacity.value_or(-1.0))
          == doctest::Approx(100));

  REQUIRE(sys.line_array.size() == 1);
  REQUIRE(sys.line_array[0].uid == 1);
  REQUIRE(sys.line_array[0].name == "GUACOLDA-PTOMONTT");
  REQUIRE(std::get<gtopt::Uid>(sys.line_array[0].bus_a) == 10);
  REQUIRE(std::get<gtopt::Name>(sys.line_array[0].bus_b) == "PTOMONTT");

  REQUIRE(std::get<double>(sys.line_array[0].capacity.value_or(-1.0))
          == doctest::Approx(300));

  REQUIRE(std::get<double>(sys.line_array[0].voltage.value_or(0.0))
          == doctest::Approx(220));
}

TEST_CASE("System daw json test 3")
{
  const size_t size = 1000;

  std::vector<gtopt::Bus> bus_array(size);
  std::vector<gtopt::Generator> generator_array(size);
  const std::vector<gtopt::Demand> demand_array;
  const std::vector<gtopt::Line> line_array;

  {
    gtopt::Uid uid = 0;

    for (size_t i = 0; i < size; ++i) {
      const gtopt::SingleId bus {uid};
      bus_array[i] = {.uid = uid, .name = "bus"};
      generator_array[i] = {
          .uid = uid,
          .name = "gen",
          .bus = bus,
          .pmin = 0.0,
          .pmax = 300.0,
          .gcost = 50.0,
          .capacity = 300.0,
      };
      ++uid;
    }
  }

  const gtopt::System system {
      .name = "system",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  REQUIRE(system.bus_array.size() == size);
  REQUIRE(system.generator_array.size() == size);
  auto json_data = daw::json::to_json(system);

  std::cout << "The json data size is " << json_data.size() << '\n';

  gtopt::System sys;
  REQUIRE(!sys.bus_array.empty() == false);

  // BENCHMARK("read system")
  {
    sys = daw::json::from_json<gtopt::System>(json_data);
    // return sys.name;
  };

  REQUIRE(sys.bus_array.size() == size);
  REQUIRE(sys.generator_array.size() == size);

  {
    gtopt::Uid uid = 0;
    for (size_t i = 0; i < size; ++i) {
      gtopt::SingleId bus {uid};
      REQUIRE(sys.bus_array[i].uid == uid);
      REQUIRE(sys.generator_array[i].uid == uid);
      REQUIRE(sys.generator_array[i].bus == bus);
      ++uid;
    }
  }
}

// === test_json_turbine.cpp ===

TEST_CASE("Turbine daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "waterway":10,
    "generator":20,
    "capacity":100.0,
    "conversion_rate":50.0,
    "drain":true
  })";

  gtopt::Turbine turbine = daw::json::from_json<gtopt::Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(turbine.capacity.has_value());
  CHECK(std::get<double>(turbine.capacity.value_or(-1.0)) == 100.0);
  CHECK(turbine.conversion_rate.has_value());
  CHECK(std::get<double>(turbine.conversion_rate.value_or(-1.0)) == 50.0);
  CHECK(turbine.drain.has_value());
  CHECK(turbine.drain.value() == true);  // NOLINT
}

TEST_CASE("Turbine daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "waterway":10,
    "generator":20
  })";

  const gtopt::Turbine turbine =
      daw::json::from_json<gtopt::Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(!turbine.capacity.has_value());
  CHECK(!turbine.conversion_rate.has_value());
  CHECK(!turbine.drain.has_value());
}

TEST_CASE("Turbine array json test")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"TURBINE_A",
    "waterway":10,
    "generator":20,
    "drain":false
  },{
    "uid":15,
    "name":"TURBINE_B",
    "capacity":200.0,
    "conversion_rate":75.0,
    "waterway":30,
    "generator":40,
    "drain":true
  }])";

  std::vector<gtopt::Turbine> turbines =
      daw::json::from_json_array<gtopt::Turbine>(json_data);

  CHECK(turbines[0].uid == 5);
  CHECK(turbines[0].name == "TURBINE_A");
  CHECK(!turbines[0].capacity.has_value());
  CHECK(!turbines[0].conversion_rate.has_value());
  CHECK(turbines[0].drain.has_value());
  CHECK(turbines[0].drain.value() == false);  // NOLINT

  CHECK(turbines[1].uid == 15);
  CHECK(turbines[1].name == "TURBINE_B");
  CHECK(turbines[1].capacity.has_value());
  CHECK(std::get<double>(turbines[1].capacity.value_or(-1.0)) == 200.0);
  CHECK(turbines[1].conversion_rate.has_value());
  CHECK(std::get<double>(turbines[1].conversion_rate.value_or(-1.0)) == 75.0);
  CHECK(turbines[1].drain.has_value());
  CHECK(turbines[1].drain.value() == true);  // NOLINT
}

TEST_CASE("Turbine with active property serialization")
{

  SUBCASE("With boolean active")
  {
    Turbine turbine;
    turbine.uid = 1;
    turbine.name = "test_turbine";
    turbine.active = True;
    turbine.drain = false;

    auto json = daw::json::to_json(turbine);
    const Turbine roundtrip = daw::json::from_json<Turbine>(json);

    CHECK(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value_or(False)) == True);
    CHECK(roundtrip.drain.has_value());
    CHECK(roundtrip.drain.value() == false);  // NOLINT
  }

  SUBCASE("With schedule active")
  {
    Turbine turbine;
    turbine.uid = 1;
    turbine.name = "test_turbine";
    turbine.active = std::vector<IntBool> {True, False, True, False};
    turbine.drain = std::nullopt;

    auto json = daw::json::to_json(turbine);
    Turbine roundtrip = daw::json::from_json<Turbine>(json);

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
    CHECK(!roundtrip.drain.has_value());
  }
}

TEST_CASE("Turbine with empty optional fields")
{

  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "active":null,
    "capacity":null,
    "waterway":10,
    "generator":20,
    "conversion_rate":null,
    "drain":null
  })";

  const Turbine turbine = daw::json::from_json<Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(!turbine.active.has_value());
  CHECK(!turbine.capacity.has_value());
  CHECK(!turbine.conversion_rate.has_value());
  CHECK(!turbine.drain.has_value());
}

// === test_json_waterway.cpp ===

TEST_CASE("Waterway JSON basic parsing")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20
  })";

  gtopt::Waterway waterway = daw::json::from_json<gtopt::Waterway>(json_data);

  REQUIRE(waterway.uid == 5);
  REQUIRE(waterway.name == "RIVER_1");
  REQUIRE(std::get<gtopt::Uid>(waterway.junction_a) == 10);
  REQUIRE(std::get<gtopt::Uid>(waterway.junction_b) == 20);
  REQUIRE_FALSE(waterway.capacity.has_value());
}

TEST_CASE("Waterway JSON with optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20,
    "capacity":100.5,
    "lossfactor":0.05,
    "fmin":-50.0,
    "fmax":100.0
  })";

  const gtopt::Waterway waterway =
      daw::json::from_json<gtopt::Waterway>(json_data);

  REQUIRE(waterway.uid == 5);
  REQUIRE(waterway.name == "RIVER_1");
  REQUIRE(waterway.capacity.has_value());
  REQUIRE(waterway.lossfactor.has_value());
  REQUIRE(waterway.fmin.has_value());
  REQUIRE(waterway.fmax.has_value());
}

TEST_CASE("Waterway JSON array parsing")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20
  },{
    "uid":6,
    "name":"RIVER_2",
    "junction_a":20,
    "junction_b":30,
    "capacity":200.0
  }])";

  std::vector<gtopt::Waterway> waterways =
      daw::json::from_json_array<gtopt::Waterway>(json_data);

  REQUIRE(waterways.size() == 2);
  REQUIRE(waterways[0].uid == 5);
  REQUIRE(waterways[0].name == "RIVER_1");
  REQUIRE_FALSE(waterways[0].capacity.has_value());

  REQUIRE(waterways[1].uid == 6);
  REQUIRE(waterways[1].name == "RIVER_2");
  REQUIRE(waterways[1].capacity.has_value());
}

TEST_CASE("Waterway JSON with active schedule")
{

  std::string_view json_data = R"({
    "uid":1,
    "name":"CANAL_1",
    "junction_a":1,
    "junction_b":2,
    "active":[1,0,1,0]
  })";

  Waterway waterway = daw::json::from_json<Waterway>(json_data);

  REQUIRE(waterway.active.has_value());
  const auto& active =
      std::get<std::vector<IntBool>>(waterway.active.value());  // NOLINT
  REQUIRE(active.size() == 4);
  CHECK(active[0] == True);
  CHECK(active[1] == False);
  CHECK(active[2] == True);
  CHECK(active[3] == False);
}

TEST_CASE("Waterway JSON roundtrip serialization")
{
  gtopt::Waterway original;
  original.uid = 7;
  original.name = "ROUNDTRIP";
  original.junction_a = gtopt::SingleId(gtopt::Uid {1});
  original.junction_b = gtopt::SingleId(gtopt::Uid {2});
  original.capacity = 100.0;

  auto json = daw::json::to_json(original);
  gtopt::Waterway roundtrip = daw::json::from_json<gtopt::Waterway>(json);

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(std::get<gtopt::Uid>(roundtrip.junction_a) == 1);
  REQUIRE(std::get<gtopt::Uid>(roundtrip.junction_b) == 2);
  REQUIRE(roundtrip.capacity.has_value());
  REQUIRE(std::get<double>(roundtrip.capacity.value_or(0.0)) == 100.0);
}

TEST_CASE("Waterway with empty optional fields")
{

  std::string_view json_data = R"({
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20,
    "capacity":null,
    "lossfactor":null,
    "fmin":null,
    "fmax":null
  })";

  const Waterway waterway = daw::json::from_json<Waterway>(json_data);

  CHECK(waterway.uid == 5);
  CHECK(waterway.name == "RIVER_1");
  CHECK(waterway.capacity.has_value() == false);
  CHECK(waterway.lossfactor.has_value() == false);
  CHECK(waterway.fmin.has_value() == false);
  CHECK(waterway.fmax.has_value() == false);
}

// === test_planning_json.cpp ===

static constexpr std::string_view planning_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
    "use_lp_names": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1},
      {"uid": 2, "first_block": 1, "count_block": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ]
  },
  "system": {
    "name": "json_test_system",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 50, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 2, "capacity": 80}
    ],
    "line_array": [
      {
        "uid": 1, "name": "l1",
        "bus_a": 1, "bus_b": 2,
        "reactance": 0.1,
        "tmax_ba": 200, "tmax_ab": 200,
        "capacity": 200
      }
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1",
        "input_efficiency": 0.9,
        "output_efficiency": 0.9,
        "vmin": 0, "vmax": 50,
        "capacity": 50
      }
    ],
    "converter_array": [
      {
        "uid": 1, "name": "conv1",
        "battery": 1, "generator": 1, "demand": 1,
        "capacity": 100
      }
    ]
  }
})";

TEST_CASE("Planning JSON round-trip serialization")
{
  auto planning = daw::json::from_json<Planning>(planning_json);

  // Serialize back to JSON
  auto json_output = daw::json::to_json(planning);
  CHECK(!json_output.empty());

  // Parse the output back
  auto planning2 = daw::json::from_json<Planning>(json_output);
  CHECK(planning2.system.name == planning.system.name);
  CHECK(planning2.system.bus_array.size() == planning.system.bus_array.size());
  CHECK(planning2.system.generator_array.size()
        == planning.system.generator_array.size());
}
