/**
 * @file      test_json_optimization.cpp
 * @brief     Unit tests for Optimization JSON serialization
 * @date      Fri May  2 20:35:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains the unit tests for JSON serialization/deserialization
 * of the Optimization class.
 */

#include <string>

#include <doctest/doctest.h>
#include <gtopt/json/json_optimization.hpp>

TEST_CASE("JSON Optimization - Serialize empty")
{
  using namespace gtopt;

  // Create empty optimization
  Optimization opt {};

  // Serialize to JSON
  const auto json_data = daw::json::to_json(opt);

  // Verify JSON structure
  CHECK(json_data.find("\"options\":") != std::string::npos);
  CHECK(json_data.find("\"simulation\":") != std::string::npos);
  CHECK(json_data.find("\"system\":") != std::string::npos);
}

TEST_CASE("JSON Optimization - Round trip serialization")
{
  using namespace gtopt;

  // Create optimization with components
  Options options {};
  Simulation simulation {};

  // Create minimal system
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> gen_array = {
      {.uid = Uid {1}, .name = "g1", .bus = Uid {1}}};
  System system {.name = "TestSystem",
                 .bus_array = bus_array,
                 .generator_array = gen_array};

  Optimization original {
      .options = options, .simulation = simulation, .system = system};

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Deserialize back to an object
  const auto deserialized = daw::json::from_json<Optimization>(json_data);

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

TEST_CASE("JSON Optimization - Partial filled objects")
{
  using namespace gtopt;

  // Create optimization with only some components filled
  Optimization opt1 {
      .options = {},
      .simulation = {},  // Empty simulation
      .system = {}  // Empty system
  };

  // Serialize and deserialize
  const auto json_data1 = daw::json::to_json(opt1);
  const auto deserialized1 = daw::json::from_json<Optimization>(json_data1);

  // Check that partial filling works correctly
  CHECK(deserialized1.system.name.empty());

  // Another test with different components filled
  Optimization opt2 {.options = {},  // Empty options
                     .simulation = {},  // Empty simulation
                     .system = {.name = "TestSystem"}};

  // Serialize and deserialize
  const auto json_data2 = daw::json::to_json(opt2);
  const auto deserialized2 = daw::json::from_json<Optimization>(json_data2);

  // Check that partial filling works correctly
  CHECK(deserialized2.system.name == "TestSystem");
}
