// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/system.hpp>

TEST_CASE("Battery new fields default to nullopt")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Battery battery;

  CHECK_FALSE(battery.bus.has_value());
  CHECK_FALSE(battery.pmax_charge.has_value());
  CHECK_FALSE(battery.pmax_discharge.has_value());
  CHECK_FALSE(battery.gcost.has_value());
}

TEST_CASE("Battery unified field assignment")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.name = "ExpandTest";

  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // A single generator already exists (for non-battery use)
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 20.0,
          .capacity = 200.0,
      },
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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

TEST_CASE(
    "expand_batteries with source_generator creates internal bus")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.name = "CoupledBatteryTest";

  // External bus
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // Source generator (solar plant) — no bus set initially
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "solar1",
          .gcost = 0.0,
          .capacity = 60.0,
      },
  };

  // Generation-coupled battery: bus + source_generator both set
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .source_generator = Name {"solar1"},
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

  system.expand_batteries();

  // Internal bus was created
  REQUIRE(system.bus_array.size() == 2);
  const auto& int_bus = system.bus_array.back();
  CHECK(int_bus.name == "bat1_int_bus");
  const Uid int_bus_uid = int_bus.uid;

  // Discharge generator connects to external bus
  REQUIRE(system.generator_array.size() == 2);
  const auto& disc_gen = system.generator_array.back();
  CHECK(disc_gen.name == "bat1_gen");
  REQUIRE(std::holds_alternative<Uid>(disc_gen.bus));
  CHECK(std::get<Uid>(disc_gen.bus) == Uid {1});

  // Source generator bus was set to internal bus
  const auto& src_gen = system.generator_array.front();
  CHECK(src_gen.name == "solar1");
  REQUIRE(std::holds_alternative<Uid>(src_gen.bus));
  CHECK(std::get<Uid>(src_gen.bus) == int_bus_uid);

  // Charge demand connects to internal bus
  REQUIRE(system.demand_array.size() == 1);
  const auto& chg_dem = system.demand_array.back();
  CHECK(chg_dem.name == "bat1_dem");
  REQUIRE(std::holds_alternative<Uid>(chg_dem.bus));
  CHECK(std::get<Uid>(chg_dem.bus) == int_bus_uid);

  // Converter was created
  REQUIRE(system.converter_array.size() == 1);
  CHECK(system.converter_array.back().name == "bat1_conv");

  // Battery fields were cleared (idempotent)
  CHECK_FALSE(system.battery_array[0].bus.has_value());
  CHECK_FALSE(system.battery_array[0].source_generator.has_value());
}

TEST_CASE(
    "expand_batteries source_generator with existing bus is overridden")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.name = "OverrideBusTest";

  system.bus_array = {
      {.uid = Uid {1}, .name = "ext_bus"},
      {.uid = Uid {2}, .name = "other_bus"},
  };

  // Source generator with a pre-existing bus (should be overridden)
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "solar1",
          .bus = Uid {2},  // will be overridden
          .gcost = 0.0,
          .capacity = 50.0,
      },
  };

  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .source_generator = Name {"solar1"},
          .pmax_charge = 50.0,
          .pmax_discharge = 50.0,
          .capacity = 100.0,
      },
  };

  system.expand_batteries();

  // An internal bus was created (uid=3 since 1 and 2 already existed)
  REQUIRE(system.bus_array.size() == 3);
  const auto& int_bus = system.bus_array.back();
  CHECK(int_bus.name == "bat1_int_bus");
  const Uid int_bus_uid = int_bus.uid;

  // Source generator bus was overridden to internal bus
  const auto& src_gen = system.generator_array.front();
  CHECK(src_gen.name == "solar1");
  REQUIRE(std::holds_alternative<Uid>(src_gen.bus));
  CHECK(std::get<Uid>(src_gen.bus) == int_bus_uid);

  // Charge demand is on internal bus
  REQUIRE(system.demand_array.size() == 1);
  REQUIRE(std::holds_alternative<Uid>(system.demand_array.back().bus));
  CHECK(std::get<Uid>(system.demand_array.back().bus) == int_bus_uid);

  // Discharge generator is on external bus
  REQUIRE(std::holds_alternative<Uid>(system.generator_array.back().bus));
  CHECK(std::get<Uid>(system.generator_array.back().bus) == Uid {1});
}

TEST_CASE("expand_batteries source_generator not found logs warning")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.name = "MissingSourceGenTest";

  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // Battery references a non-existent source generator
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .source_generator = Name {"nonexistent"},
          .pmax_charge = 50.0,
          .pmax_discharge = 50.0,
          .capacity = 100.0,
      },
  };

  // Should not throw — just log a warning
  REQUIRE_NOTHROW(system.expand_batteries());

  // Internal bus is still created
  CHECK(system.bus_array.size() == 2);
  // Discharge generator + charge demand are still created
  CHECK(system.generator_array.size() == 1);
  CHECK(system.demand_array.size() == 1);
  CHECK(system.converter_array.size() == 1);
}
