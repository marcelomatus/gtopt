// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/system.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Battery new fields default to nullopt")  // NOLINT
{
  const Battery battery;

  CHECK_FALSE(battery.bus.has_value());
  CHECK_FALSE(battery.pmax_charge.has_value());
  CHECK_FALSE(battery.pmax_discharge.has_value());
  CHECK_FALSE(battery.gcost.has_value());
}

TEST_CASE("Battery unified field assignment")  // NOLINT
{
  Battery battery;

  battery.uid = 1;
  battery.name = "bess1";
  battery.bus = Uid {3};
  battery.pmax_charge = 60.0;
  battery.pmax_discharge = 60.0;
  battery.gcost = 5.0;

  REQUIRE(battery.bus.has_value());
  CHECK(std::get<Uid>(*battery.bus) == Uid {3});

  REQUIRE(battery.pmax_charge.has_value());
  CHECK(std::get<Real>(*battery.pmax_charge) == 60.0);

  REQUIRE(battery.pmax_discharge.has_value());
  CHECK(std::get<Real>(*battery.pmax_discharge) == 60.0);

  REQUIRE(battery.gcost.has_value());
  CHECK(std::get<Real>(*battery.gcost) == 5.0);
}

TEST_CASE("System::expand_batteries with unified definition")  // NOLINT
{
  System system;
  system.name = "ExpandTest";

  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // A single generator already exists (for non-battery use)
  system.generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 20.0,
       .capacity = 200.0,},
  };

  // A single demand already exists
  system.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  // Unified battery definition: bus is set → triggers expansion
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 200.0,
          .pmax_charge = 60.0,
          .pmax_discharge = 60.0,
          .gcost = 0.0,
          .capacity = 200.0,
      },
  };

  REQUIRE(system.generator_array.size() == 1);
  REQUIRE(system.demand_array.size() == 1);
  REQUIRE(system.converter_array.empty());

  // Expand
  system.expand_batteries();

  // Generator was appended
  CHECK(system.generator_array.size() == 2);
  const auto& gen = system.generator_array.back();
  CHECK(gen.name == "bat1_gen");
  CHECK(gen.uid == Uid {2});
  CHECK(std::get<Uid>(gen.bus) == Uid {1});
  REQUIRE(gen.capacity.has_value());
  CHECK(std::get<Real>(gen.capacity.value_or(RealFieldSched {0.0})) == 60.0);
  REQUIRE(gen.gcost.has_value());
  CHECK(std::get<Real>(gen.gcost.value_or(RealFieldSched {-1.0})) == 0.0);

  // Demand was appended
  CHECK(system.demand_array.size() == 2);
  const auto& dem = system.demand_array.back();
  CHECK(dem.name == "bat1_dem");
  CHECK(dem.uid == Uid {2});
  CHECK(std::get<Uid>(dem.bus) == Uid {1});
  REQUIRE(dem.capacity.has_value());
  CHECK(std::get<Real>(dem.capacity.value_or(RealFieldSched {0.0})) == 60.0);

  // Converter was created
  CHECK(system.converter_array.size() == 1);
  const auto& conv = system.converter_array.back();
  CHECK(conv.name == "bat1_conv");
  CHECK(conv.uid == Uid {1});
  CHECK(std::get<Name>(conv.battery) == "bat1");
  CHECK(std::get<Name>(conv.generator) == "bat1_gen");
  CHECK(std::get<Name>(conv.demand) == "bat1_dem");

  // Battery bus was cleared (idempotent)
  CHECK_FALSE(system.battery_array[0].bus.has_value());
}

TEST_CASE("expand_batteries skips batteries without bus")  // NOLINT
{
  System system;
  system.name = "SkipTest";

  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // Traditional battery (no bus field)
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_traditional",
          .input_efficiency = 0.9,
          .output_efficiency = 0.9,
          .emin = 0.0,
          .emax = 50.0,
          .capacity = 50.0,
      },
  };

  system.expand_batteries();

  // Nothing was auto-generated
  CHECK(system.generator_array.empty());
  CHECK(system.demand_array.empty());
  CHECK(system.converter_array.empty());
}

TEST_CASE("expand_batteries is idempotent")  // NOLINT
{
  System system;
  system.name = "IdempotentTest";

  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};

  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .pmax_charge = 50.0,
          .pmax_discharge = 50.0,
          .capacity = 100.0,
      },
  };

  system.expand_batteries();
  CHECK(system.generator_array.size() == 1);
  CHECK(system.demand_array.size() == 1);
  CHECK(system.converter_array.size() == 1);

  // Second call should be a no-op (bus was cleared)
  system.expand_batteries();
  CHECK(system.generator_array.size() == 1);
  CHECK(system.demand_array.size() == 1);
  CHECK(system.converter_array.size() == 1);
}

TEST_CASE("expand_batteries multiple batteries")  // NOLINT
{
  System system;
  system.name = "MultiBatteryTest";

  system.bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .pmax_charge = 60.0,
          .pmax_discharge = 60.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "bat2",
          .bus = Uid {2},
          .pmax_charge = 30.0,
          .pmax_discharge = 40.0,
          .capacity = 100.0,
      },
  };

  system.expand_batteries();

  CHECK(system.generator_array.size() == 2);
  CHECK(system.demand_array.size() == 2);
  CHECK(system.converter_array.size() == 2);

  // Verify UIDs are sequential
  CHECK(system.generator_array[0].uid == Uid {1});
  CHECK(system.generator_array[1].uid == Uid {2});
  CHECK(system.demand_array[0].uid == Uid {1});
  CHECK(system.demand_array[1].uid == Uid {2});
  CHECK(system.converter_array[0].uid == Uid {1});
  CHECK(system.converter_array[1].uid == Uid {2});

  // Verify names
  CHECK(system.generator_array[0].name == "bat1_gen");
  CHECK(system.generator_array[1].name == "bat2_gen");
  CHECK(system.demand_array[0].name == "bat1_dem");
  CHECK(system.demand_array[1].name == "bat2_dem");

  // Verify different power ratings
  REQUIRE(system.generator_array[1].capacity.has_value());
  CHECK(std::get<Real>(
            system.generator_array[1].capacity.value_or(RealFieldSched {0.0}))
        == 40.0);
  REQUIRE(system.demand_array[1].capacity.has_value());
  CHECK(std::get<Real>(
            system.demand_array[1].capacity.value_or(RealFieldSched {0.0}))
        == 30.0);
}
