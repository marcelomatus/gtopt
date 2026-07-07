// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("Battery new fields default to nullopt")  // NOLINT
{
  using namespace gtopt;

  const Battery battery;

  CHECK_FALSE(battery.bus.has_value());
  CHECK_FALSE(battery.pmax_charge.has_value());
  CHECK_FALSE(battery.pmax_discharge.has_value());
  CHECK_FALSE(battery.pmin_charge.has_value());
  CHECK_FALSE(battery.pmin_discharge.has_value());
  CHECK_FALSE(battery.discharge_cost.has_value());
  CHECK_FALSE(battery.commitment.has_value());
}

TEST_CASE("Battery unified field assignment")  // NOLINT
{
  using namespace gtopt;

  Battery battery;

  battery.uid = 1;
  battery.name = "bess1";
  battery.bus = Uid {3};
  battery.pmax_charge = 60.0;
  battery.pmax_discharge = 60.0;
  battery.discharge_cost = 5.0;

  REQUIRE(battery.bus.has_value());
  CHECK(std::get<Uid>(*battery.bus) == Uid {3});

  REQUIRE(battery.pmax_charge.has_value());
  CHECK(std::get<Real>(*battery.pmax_charge) == 60.0);

  REQUIRE(battery.pmax_discharge.has_value());
  CHECK(std::get<Real>(*battery.pmax_discharge) == 60.0);

  REQUIRE(battery.discharge_cost.has_value());
  CHECK(std::get<Real>(*battery.discharge_cost) == 5.0);
}

TEST_CASE("System::expand_batteries with unified definition")  // NOLINT
{
  using namespace gtopt;

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
          .discharge_cost = 0.0,
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
  // ``Battery.pmax_discharge`` now maps to ``Generator.pmax`` (TB)
  // instead of ``Generator.capacity`` (T).  ``capacity`` is left unset
  // so the default ``numeric_limits<double>::max()`` sentinel applies.
  REQUIRE(gen.pmax.has_value());
  CHECK(std::get<Real>(gen.pmax.value_or(RealFieldSched2 {0.0})) == 60.0);
  CHECK_FALSE(gen.capacity.has_value());
  REQUIRE(gen.gcost.has_value());
  CHECK(std::get<Real>(gen.gcost.value_or(RealFieldSched2 {-1.0})) == 0.0);

  // Demand was appended
  CHECK(system.demand_array.size() == 2);
  const auto& dem = system.demand_array.back();
  CHECK(dem.name == "bat1_dem");
  CHECK(dem.uid == Uid {2});
  CHECK(std::get<Uid>(dem.bus) == Uid {1});
  // ``Battery.pmax_charge`` now maps to ``Demand.lmax`` (TB) instead
  // of ``Demand.capacity`` (T).
  REQUIRE(dem.lmax.has_value());
  CHECK(std::get<Real>(dem.lmax.value_or(RealFieldSched2 {0.0})) == 60.0);
  CHECK_FALSE(dem.capacity.has_value());

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

TEST_CASE("expand_batteries pins synthetic charge demand fcost=0")  // NOLINT
{
  // The synthetic charge demand created for a battery must carry an explicit
  // fcost = 0, so it stays truly dispatchable in [0, pmax_charge] regardless
  // of the global model_options.demand_fail_cost.  Without this pin, a
  // positive global default (e.g. 1000 $/MWh) would penalize the LP for not
  // charging the battery and force charging at pmax — silently distorting
  // dispatch in any case where the global is raised.
  using namespace gtopt;

  System system;
  system.name = "BatteryFcostPinTest";
  system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
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

  REQUIRE(system.demand_array.size() == 1);
  const auto& dem = system.demand_array.back();
  CHECK(dem.name == "bat1_dem");
  REQUIRE(dem.fcost.has_value());
  CHECK(std::get<Real>(dem.fcost.value_or(RealFieldSched2 {-1.0})) == 0.0);
}

TEST_CASE("expand_batteries pins fcost=0 even with source_generator")  // NOLINT
{
  // Same invariant as above, but for the generation-coupled mode where the
  // charge demand lands on the auto-created internal bus.
  using namespace gtopt;

  System system;
  system.name = "CoupledBatteryFcostPinTest";
  system.bus_array = {
      {
          .uid = Uid {1},
          .name = "ext_bus",
      },
  };
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "solar1",
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

  REQUIRE(system.demand_array.size() == 1);
  const auto& dem = system.demand_array.back();
  REQUIRE(dem.fcost.has_value());
  CHECK(std::get<Real>(dem.fcost.value_or(RealFieldSched2 {-1.0})) == 0.0);
}

TEST_CASE("expand_batteries skips batteries without bus")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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

  // Verify different power ratings (now mapped to operational
  // bounds: ``pmax_discharge`` → ``Generator.pmax``,
  // ``pmax_charge`` → ``Demand.lmax``).
  REQUIRE(system.generator_array[1].pmax.has_value());
  CHECK(std::get<Real>(
            system.generator_array[1].pmax.value_or(RealFieldSched2 {0.0}))
        == 40.0);
  REQUIRE(system.demand_array[1].lmax.has_value());
  CHECK(std::get<Real>(
            system.demand_array[1].lmax.value_or(RealFieldSched2 {0.0}))
        == 30.0);
}

TEST_CASE(
    "expand_batteries with source_generator creates internal bus")  // NOLINT
{
  using namespace gtopt;

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
          .discharge_cost = 0.0,
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
  using namespace gtopt;

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
  using namespace gtopt;

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

TEST_CASE(
    "BatteryLP — capainst primal col_sol binds at capmin lower bound")  // NOLINT
{
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-unchecked-optional-access)

  // Deterministic LP where the battery's own installed-capacity column
  // (`capainst`, owned by BatteryLP via CapacityObjectBase through
  // StorageLP) can be looked up and asserted via `capacity_col_at(stage)`.
  //
  // In gtopt, the `capainst` column lower bound is driven by the Battery
  // `.capacity` field (CapacityObjectBase sets `lowb = stage_capacity`).
  // The LP is therefore forced to *build* at least that much storage
  // whenever an expansion column exists (`expcap > 0` and `expmod > 0`),
  // because the equality row `-capainst + expcap*expmod_col = 0` in the
  // single-stage case means `capainst = expcap * expmod`.  With a small
  // `annual_capcost`, the LP minimizes `expmod_col` down to exactly the
  // value that satisfies `capainst >= capacity`, so `capainst` binds at
  // the lower bound.
  //
  // Setup:
  //   - 1 bus, 1 stage, 1 block (duration = 1 h), 1 scenario.
  //   - Cheap generator at bus1 with ample capacity to serve the demand,
  //     so there is no economic pressure to charge/discharge the battery.
  //   - Demand of 10 MW kept well below generator capacity → no stress
  //     on the battery path.
  //   - Battery with `capacity = 20.0` (sets capainst lowb = 20),
  //     `expcap = 100`, `expmod = 1`, `annual_capcost = 10` → stage
  //     maxexpcap = 100 > 0, so the capainst column is created with
  //     `lowb = 20`, `uppb = 120`.  `emax = 100` is clamped by capmax
  //     via `stage_maxmin_at`.  `eini = 20` keeps SoC bounds feasible.
  //
  // Expected:
  //   - capainst primal ≈ 20.0 (binds at the capacity-driven lower bound;
  //     LP minimizes expmod_col so `capainst = expcap * expmod = 100 *
  //     0.2 = 20`).

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen_main",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d_main",
          .bus = Uid {1},
          .capacity = 10.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 20.0,
          .capacity = 20.0,
          .expcap = 100.0,
          .expmod = 1.0,
          .annual_capcost = 10.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              Block {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              Stage {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              Scenario {
                  .uid = Uid {0},
                  .probability_factor = 1,
              },
          },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "BatteryCapainstLowerBound",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Look up the battery's own capainst column via the LP-class accessor.
  const auto& bat_lps = system_lp.elements<BatteryLP>();
  REQUIRE(bat_lps.size() == 1);
  const auto& bat_lp = bat_lps.front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto cap_col = bat_lp.capacity_col_at(stage_lp);
  REQUIRE(cap_col.has_value());

  // Positive annual_capcost makes the LP minimize expmod_col down to the
  // value that just satisfies capainst >= capacity = 20, giving
  // capainst = expcap * expmod = 100 * 0.2 = 20.  The column therefore
  // binds at its lower bound.
  const auto cap_val = lp.get_col_sol()[*cap_col];
  CHECK(cap_val == doctest::Approx(20.0).epsilon(1e-6));
}

// NOLINTEND(bugprone-unchecked-optional-access)

// --- New field wiring (pmin_*, lmin, commitment) ----------------------------

TEST_CASE("Battery pmin_charge/pmin_discharge accept TB schedules")  // NOLINT
{
  Battery battery;
  battery.pmin_charge = 5.0;  // scalar broadcast
  battery.pmin_discharge =
      TBRealFieldSched {std::vector<std::vector<Real>> {{2.0, 2.5, 3.0}}};

  REQUIRE(battery.pmin_charge.has_value());
  CHECK(std::get<Real>(*battery.pmin_charge) == doctest::Approx(5.0));

  REQUIRE(battery.pmin_discharge.has_value());
  const auto& v =
      std::get<std::vector<std::vector<Real>>>(*battery.pmin_discharge);
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].size() == 3);
  CHECK(v[0][0] == doctest::Approx(2.0));
  CHECK(v[0][1] == doctest::Approx(2.5));
  CHECK(v[0][2] == doctest::Approx(3.0));
}

TEST_CASE("expand_batteries wires pmin_* onto synthetic gen/demand")  // NOLINT
{
  System system;
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .pmax_charge = 10.0,
          .pmax_discharge = 10.0,
          .pmin_charge = 3.0,
          .pmin_discharge = 2.0,
          .discharge_cost = 1.0,
      },
  };

  system.expand_batteries();

  REQUIRE(system.generator_array.size() == 1);
  const auto& gen = system.generator_array.back();
  // Generator.pmin is intentionally unset on the synthesized discharge
  // generator — `Commitment.pmin` carries the per-unit min stable
  // level as a u-gated row, so the always-on `Generator.pmin` floor
  // would clash with that conditional bound.  See
  // `source/system.cpp::expand_batteries` for the rationale.
  CHECK_FALSE(gen.pmin.has_value());
  // pmax_discharge → Generator.pmax
  REQUIRE(gen.pmax.has_value());
  CHECK(std::get<Real>(gen.pmax.value_or(RealFieldSched2 {0.0})) == 10.0);

  // pmin_discharge → Commitment.pmin (synthesized for the discharge gen)
  REQUIRE(system.commitment_array.size() == 1);
  const auto& cmt = system.commitment_array.back();
  REQUIRE(cmt.pmin.has_value());
  CHECK(std::get<double>(cmt.pmin.value_or(0.0)) == doctest::Approx(2.0));

  REQUIRE(system.demand_array.size() == 1);
  const auto& dem = system.demand_array.back();
  // pmin_charge → Demand.lmin
  REQUIRE(dem.lmin.has_value());
  CHECK(std::get<Real>(dem.lmin.value_or(RealFieldSched2 {0.0})) == 3.0);
  REQUIRE(dem.lmax.has_value());
  CHECK(std::get<Real>(dem.lmax.value_or(RealFieldSched2 {0.0})) == 10.0);
}

TEST_CASE("expand_batteries propagates commitment to Converter")  // NOLINT
{
  System system;
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .pmax_charge = 10.0,
          .pmax_discharge = 10.0,
          .pmin_charge = 3.0,
          .commitment = true,
      },
  };

  system.expand_batteries();
  REQUIRE(system.converter_array.size() == 1);
  const auto& conv = system.converter_array.back();
  REQUIRE(conv.commitment.has_value());
  CHECK(*conv.commitment == true);
}

TEST_CASE(
    "expand_batteries omits Converter.commitment when Battery.commitment unset")
{
  System system;
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  system.battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .pmax_charge = 10.0,
          .pmax_discharge = 10.0,
          // .commitment intentionally unset
      },
  };

  system.expand_batteries();
  REQUIRE(system.converter_array.size() == 1);
  CHECK_FALSE(system.converter_array.back().commitment.has_value());
}

// ── Battery → Commitment synthesis uniqueness (2026-05-20) ──────────────
//
// `expand_batteries` synthesizes a single `Commitment("uc_<bat>_gen")`
// whenever ANY of three triggers fires:
//   * `Battery.pmin_discharge` set
//   * `Battery.pmin_charge` set
//   * `Battery.commitment = true`
//
// The 16fbdde45 refactor centralized this: one CommitmentLP owns the
// `u_commit` column, both Generator (discharge) and Demand (charge)
// share it via `find_status_cols`.  Pin that even when all three
// triggers fire AT ONCE we still get exactly ONE synthesized
// Commitment (no duplicates).  Catches the kind of regression that
// would silently produce two competing `u` columns.

TEST_CASE(
    "expand_batteries — all 3 commitment triggers produce exactly "
    "1 synthesized Commitment")  // NOLINT
{
  System system;
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  system.battery_array = {
      Battery {
          .uid = Uid {1},
          .name = "bat",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .pmax_charge = 10.0,
          .pmax_discharge = 10.0,
          .pmin_charge = 3.0,
          .pmin_discharge = 2.0,
          .discharge_cost = 1.0,
          .commitment = OptBool {true},  // third trigger
      },
  };
  system.expand_batteries();

  // Exactly one synthesized Commitment, even though all three triggers
  // fired.  Name is `uc_<gen_name>` = `uc_bat_gen`.
  REQUIRE(system.commitment_array.size() == 1);
  CHECK(system.commitment_array.front().name == "uc_bat_gen");
  // Commitment.pmin carries the discharge-side floor.
  CHECK(std::get<double>(system.commitment_array.front().pmin.value_or(-1.0))
        == doctest::Approx(2.0));
  // Converter sees the same `commitment = true` flag.
  REQUIRE(system.converter_array.size() == 1);
  CHECK(system.converter_array.front().commitment.value_or(false) == true);
}

TEST_CASE(
    "expand_batteries — plain battery synthesizes a relaxed "
    "Commitment")  // NOLINT
{
  System system;
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  system.battery_array = {
      Battery {
          .uid = Uid {1},
          .name = "bat",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .pmax_charge = 10.0,
          .pmax_discharge = 10.0,
          .discharge_cost = 1.0,
          // No pmin_* and no commitment=true.
      },
  };
  system.expand_batteries();
  // The ``uc_<bat>_gen`` Commitment is synthesized UNCONDITIONALLY so
  // PLEXOS-derived UserConstraints can reference its status column.
  // For a plain battery (no pmin, no ``commitment = true``) it is
  // LP-relaxed (continuous u ∈ [0, 1], zero integer cost) and carries
  // no pmin floor.
  REQUIRE(system.commitment_array.size() == 1);
  CHECK(system.commitment_array.front().name == "uc_bat_gen");
  CHECK(system.commitment_array.front().relax.value_or(false));
  CHECK_FALSE(system.commitment_array.front().pmin.has_value());
}

TEST_CASE(
    "expand_batteries — running twice does not duplicate the synthesized "
    "Commitment")  // NOLINT
{
  // The expand pass is documented as idempotent.  Pin the idempotency
  // for the synthesized Commitment too — a second pass must not
  // re-emit a second `uc_<bat>_gen` entry.
  System system;
  system.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  system.battery_array = {
      Battery {
          .uid = Uid {1},
          .name = "bat",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .pmax_charge = 10.0,
          .pmax_discharge = 10.0,
          .pmin_discharge = 2.0,
          .discharge_cost = 1.0,
      },
  };
  system.expand_batteries();
  REQUIRE(system.commitment_array.size() == 1);
  system.expand_batteries();
  CHECK(system.commitment_array.size() == 1);
}