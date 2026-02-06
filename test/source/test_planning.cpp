/**
 * @file      test_planning.cpp
 * @brief     Unit tests for the Planning class
 * @date      Fri May  2 20:35:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains the unit tests for the Planning class.
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning.hpp>

TEST_CASE("Planning - Default construction")
{
  using namespace gtopt;

  const Planning opt {};

  // Default planning should have empty components
  CHECK(opt.system.name.empty());
}

TEST_CASE("Planning - Construction with properties")
{
  using namespace gtopt;

  // Create basic components
  const Options options {};
  const Simulation simulation {};

  // Create minimal system
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const System system {.name = "TestSystem", .bus_array = bus_array};

  // Create planning with components
  Planning opt {.options = options, .simulation = simulation, .system = system};

  // Verify the planning properties
  CHECK(opt.system.name == "TestSystem");
  REQUIRE(opt.system.bus_array.size() == 1);
  CHECK(opt.system.bus_array[0].name == "b1");
}

TEST_CASE("Planning - Merge operation")
{
  using namespace gtopt;

  // Create first planning
  Planning opt1 {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {.name = "Opt1System"},
  };

  // Create second planning with different components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  Planning opt2 {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {.name = "Opt2System", .bus_array = bus_array},
  };

  // Merge second into first
  opt1.merge(std::move(opt2));

  // Verify the merge result (should contain components from both)
  // Exact behavior depends on how merge is implemented in child components
  // Here we're assuming last-wins behavior based on the merge() implementation
  CHECK(opt1.system.name == "Opt2System");
  REQUIRE(opt1.system.bus_array.size() == 1);
  CHECK(opt1.system.bus_array[0].name == "b1");
}

TEST_CASE("Planning - JSON serialization/deserialization")
{
  using namespace gtopt;

  // Create planning with components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Planning original {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {.name = "JsonSystem", .bus_array = bus_array},
  };

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Should be able to deserialize back to an object
  const auto deserialized = daw::json::from_json<Planning>(json_data);

  // Verify the deserialized object
  CHECK(deserialized.system.name == "JsonSystem");
  REQUIRE(deserialized.system.bus_array.size() == 1);
  CHECK(deserialized.system.bus_array[0].name == "b1");
}
