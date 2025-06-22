#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/line.hpp>
#include <gtopt/stage.hpp>

using namespace gtopt;

TEST_CASE("Line construction and default values")
{
  Line line;

  // Check default values
  CHECK(line.uid == Uid {unknown_uid});
  CHECK(line.name == Name {});
  CHECK_FALSE(line.active.has_value());
  CHECK(line.bus_a == SingleId {});
  CHECK(line.bus_b == SingleId {});
  CHECK_FALSE(line.voltage.has_value());
  CHECK_FALSE(line.resistance.has_value());
  CHECK_FALSE(line.reactance.has_value());
  CHECK_FALSE(line.lossfactor.has_value());
  CHECK_FALSE(line.tmin.has_value());
  CHECK_FALSE(line.tmax.has_value());
  CHECK_FALSE(line.tcost.has_value());
  CHECK_FALSE(line.capacity.has_value());
  CHECK_FALSE(line.expcap.has_value());
  CHECK_FALSE(line.expmod.has_value());
  CHECK_FALSE(line.capmax.has_value());
  CHECK_FALSE(line.annual_capcost.has_value());
  CHECK_FALSE(line.annual_derating.has_value());
}

TEST_CASE("Line attribute assignment")
{
  Line line;

  // Assign basic attributes
  line.uid = 1001;
  line.name = "TestLine";
  line.active = true;
  line.bus_a = Uid {1};
  line.bus_b = Uid {2};

  // Assign electrical parameters
  line.voltage = 132.0;
  line.resistance = 0.01;
  line.reactance = 0.1;
  line.lossfactor = 0.02;
  line.tmin = -100.0;
  line.tmax = 100.0;
  line.tcost = 0.5;

  // Assign capacity parameters
  line.capacity = 200.0;
  line.expcap = 50.0;
  line.expmod = 10.0;
  line.capmax = 300.0;
  line.annual_capcost = 5000.0;
  line.annual_derating = 0.01;

  // Check assigned values
  CHECK(line.uid == 1001);
  CHECK(line.name == "TestLine");
  CHECK(std::get<IntBool>(line.active.value()) == 1);
  CHECK(std::get<Uid>(line.bus_a) == 1);
  CHECK(std::get<Uid>(line.bus_b) == 2);

  // For OptTRealFieldSched types, we need to get the Real variant alternative
  CHECK(std::get_if<Real>(&line.voltage.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.resistance.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.reactance.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.lossfactor.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.tmin.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.tmax.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.tcost.value()) != nullptr);

  // Check actual values using std::get_if
  CHECK(*std::get_if<Real>(&line.voltage.value()) == 132.0);
  CHECK(*std::get_if<Real>(&line.resistance.value()) == 0.01);
  CHECK(*std::get_if<Real>(&line.reactance.value()) == 0.1);
  CHECK(*std::get_if<Real>(&line.lossfactor.value()) == 0.02);
  CHECK(*std::get_if<Real>(&line.tmin.value()) == -100.0);
  CHECK(*std::get_if<Real>(&line.tmax.value()) == 100.0);
  CHECK(*std::get_if<Real>(&line.tcost.value()) == 0.5);

  // Capacity-related checks
  CHECK(*std::get_if<Real>(&line.capacity.value()) == 200.0);
  CHECK(*std::get_if<Real>(&line.expcap.value()) == 50.0);
  CHECK(*std::get_if<Real>(&line.expmod.value()) == 10.0);
  CHECK(*std::get_if<Real>(&line.capmax.value()) == 300.0);
  CHECK(*std::get_if<Real>(&line.annual_capcost.value()) == 5000.0);
  CHECK(*std::get_if<Real>(&line.annual_derating.value()) == 0.01);
}

TEST_CASE("Line time-block schedules")
{
  Line line;
  line.bus_a = Uid {1};
  line.bus_b = Uid {2};
  line.reactance = 0.1;  // Add valid reactance for validation

  // Create simple scalar flow limits
  line.tmin = -100.0;
  line.tmax = 100.0;

  // Verify values were properly assigned
  REQUIRE(line.tmin.has_value());
  REQUIRE(line.tmax.has_value());

  auto* tmin_real_ptr = std::get_if<Real>(&line.tmin.value());
  auto* tmax_real_ptr = std::get_if<Real>(&line.tmax.value());

  REQUIRE(tmin_real_ptr != nullptr);
  REQUIRE(tmax_real_ptr != nullptr);

  CHECK(*tmin_real_ptr == -100.0);
  CHECK(*tmax_real_ptr == 100.0);
}
