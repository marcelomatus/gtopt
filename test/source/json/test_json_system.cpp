/**
 * @file      test_json_system.cpp
 * @brief     Header of
 * @date      Sun Mar 30 17:45:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_system.hpp>

TEST_CASE("System daw json test 1")
{
  const std::string_view json_data = R"({
    "name":"SEN",
    "block_array":[{"uid": 1, "duration": 2}],
    "stage_array":[],
    "scenery_array":[],
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

  REQUIRE(sys.block_array.size() == 1);
  REQUIRE(sys.block_array[0].uid == 1);
  REQUIRE(sys.block_array[0].duration == 2);

  REQUIRE(sys.stage_array.size() == 0);
  REQUIRE(sys.scenery_array.size() == 0);

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
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value()) == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));

  REQUIRE(sys.demand_array.size() == 1);
  REQUIRE(sys.demand_array[0].uid == 10);
  REQUIRE(std::get<gtopt::Uid>(sys.demand_array[0].bus) == 5);
  REQUIRE(sys.demand_array[0].name == "PTOMONTT");
  REQUIRE(std::get<double>(sys.demand_array[0].capacity.value())
          == doctest::Approx(100));

  REQUIRE(sys.line_array.size() == 1);
  REQUIRE(sys.line_array[0].uid == 1);
  REQUIRE(sys.line_array[0].name == "GUACOLDA-PTOMONTT");
  REQUIRE(std::get<gtopt::Uid>(sys.line_array[0].bus_a) == 10);
  REQUIRE(std::get<gtopt::Name>(sys.line_array[0].bus_b) == "PTOMONTT");
  REQUIRE(std::get<double>(sys.line_array[0].capacity.value())
          == doctest::Approx(300));
  REQUIRE(std::get<double>(sys.line_array[0].voltage.value_or(0.0))
          == doctest::Approx(220));
}

#ifdef NONE
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

  const gtopt::System system {.name = "system",
                              .block_array = {},
                              .stage_array = {},
                              .scenery_array = {},
                              .bus_array = bus_array,
                              .demand_array = demand_array,
                              .generator_array = generator_array,
                              .line_array = line_array,
                              .generator_profiles = {},
                              .demand_profiles = {},
                              .batteries = {},
                              .converters = {},
                              .junctions = {},
                              .waterways = {},
                              .inflows = {},
                              .outflows = {},
                              .reservoirs = {},
                              .filtrations = {},
                              .turbines = {},
                              .reserve_zones = {},
                              .reserve_provisions = {},
                              .emission_zones = {},
                              .generator_emissions = {},
                              .demand_emissions = {}};

  REQUIRE(system.bus_array.size() == size);
  REQUIRE(system.generator_array.size() == size);
  auto json_data = daw::json::to_json(system);

  std::cout << "The json data size is " << json_data.size() << std::endl;

  gtopt::System sys;
  REQUIRE(!sys.bus_array.empty() == false);

  // BENCHMARK("read system")
  {
    sys = daw::json::from_json<gtopt::System>(json_data);
    // return sys.name;
  };

  REQUIRE(sys.bus_array.size() == size);
  REQUIRE(sys.generator_array.size() == size);

  SECTION("Check all elements")
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

#endif
