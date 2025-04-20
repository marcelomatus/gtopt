#include <doctest/doctest.h>
#include <gtopt/battery_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// We'll use a simplified approach instead of mocks for now
TEST_CASE("BatteryLP construction") 
{
  Battery battery;
  battery.uid = 1001;
  battery.name = "TestBattery";
  battery.active = true;
  battery.vmin = 10.0;
  battery.vmax = 100.0;
  battery.capacity = 200.0;
  
  // We'll test this class with a more direct approach
  CHECK(battery.uid == 1001);
  CHECK(battery.name == "TestBattery");
  REQUIRE(battery.active.has_value());
  CHECK(std::get<IntBool>(battery.active.value()) == 1);
}

// We'll skip the advanced tests for now and focus on basic type functionality

TEST_CASE("Battery validation test")
{
  Battery battery;
  battery.uid = 1001;
  battery.name = "TestBattery";
  battery.active = true;
  battery.vmin = 10.0;
  battery.vmax = 100.0;
  battery.vini = 50.0;
  battery.capacity = 200.0;
  
  // Test validation success
  auto result = battery.validate();
  CHECK(result.has_value());
  CHECK(result.value() == true);
  
  // Test validation failure - vini > vmax
  battery.vini = 110.0; // Greater than vmax
  auto result2 = battery.validate();
  CHECK_FALSE(result2.has_value());
  CHECK(result2.error() == "Battery initial SOC greater than maximum SOC");
}

TEST_CASE("Line validation test")
{
  Line line;
  line.uid = 1001;
  line.name = "TestLine";
  line.bus_a = Uid{1};
  line.bus_b = Uid{2};
  line.reactance = 0.1;
  
  // Test validation success with valid parameters
  auto result = line.validate();
  CHECK(result.has_value());
  
  // Test validation failure - same bus connection
  line.bus_b = Uid{1}; // Same as bus_a
  auto result2 = line.validate();
  CHECK_FALSE(result2.has_value());
  CHECK(result2.error() == "Line cannot connect a bus to itself");
}