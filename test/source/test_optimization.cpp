/**
 * @file      test_optimization.cpp
 * @brief     Unit tests for the Optimization class
 * @date      Fri May  2 20:35:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains the unit tests for the Optimization class.
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_optimization.hpp>
#include <gtopt/optimization.hpp>

TEST_CASE("Optimization - Default construction")
{
  using namespace gtopt;

  Optimization opt {};

  // Default optimization should have empty components
  CHECK(opt.system.name.empty());
}

TEST_CASE("Optimization - Construction with properties")
{
  using namespace gtopt;

  // Create basic components
  Options options {};
  Simulation simulation {};

  // Create minimal system
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  System system {.name = "TestSystem", .bus_array = bus_array};

  // Create optimization with components
  Optimization opt {
      .options = options, .simulation = simulation, .system = system};

  // Verify the optimization properties
  CHECK(opt.system.name == "TestSystem");
  REQUIRE(opt.system.bus_array.size() == 1);
  CHECK(opt.system.bus_array[0].name == "b1");
}

TEST_CASE("Optimization - Merge operation")
{
  using namespace gtopt;

  // Create first optimization
  Optimization opt1 {
      .options = {}, .simulation = {}, .system = {.name = "Opt1System"}};

  // Create second optimization with different components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  Optimization opt2 {.options = {},
                     .simulation = {},
                     .system = {.name = "Opt2System", .bus_array = bus_array}};

  // Merge second into first
  opt1.merge(opt2);

  // Verify the merge result (should contain components from both)
  // Exact behavior depends on how merge is implemented in child components
  // Here we're assuming last-wins behavior based on the merge() implementation
  CHECK(opt1.system.name == "Opt2System");
  REQUIRE(opt1.system.bus_array.size() == 1);
  CHECK(opt1.system.bus_array[0].name == "b1");
}

TEST_CASE("Optimization - JSON serialization/deserialization")
{
  using namespace gtopt;

  // Create optimization with components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  Optimization original {
      .options = {},
      .simulation = {},
      .system = {.name = "JsonSystem", .bus_array = bus_array}};

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Should be able to deserialize back to an object
  const auto deserialized = daw::json::from_json<Optimization>(json_data);

  // Verify the deserialized object
  CHECK(deserialized.system.name == "JsonSystem");
  REQUIRE(deserialized.system.bus_array.size() == 1);
  CHECK(deserialized.system.bus_array[0].name == "b1");
}
