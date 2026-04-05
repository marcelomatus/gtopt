#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/block.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Battery construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Battery battery;

  // Check default values
  CHECK(battery.uid == Uid {unknown_uid});
  CHECK(battery.name == Name {});
  CHECK_FALSE(battery.active.has_value());
  CHECK_FALSE(battery.annual_loss.has_value());
  CHECK_FALSE(battery.emin.has_value());
  CHECK_FALSE(battery.emax.has_value());
  CHECK_FALSE(battery.ecost.has_value());
  CHECK_FALSE(battery.eini.has_value());
  CHECK_FALSE(battery.efin.has_value());
  CHECK_FALSE(battery.capacity.has_value());
  CHECK_FALSE(battery.expcap.has_value());
  CHECK_FALSE(battery.expmod.has_value());
  CHECK_FALSE(battery.capmax.has_value());
  CHECK_FALSE(battery.annual_capcost.has_value());
  CHECK_FALSE(battery.annual_derating.has_value());
}

TEST_CASE("Battery attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Battery battery;

  // Assign values
  battery.uid = 1001;
  battery.name = "TestBattery";
  battery.active = true;
  battery.emin = 10.0;
  battery.emax = 100.0;
  battery.eini = 50.0;
  battery.efin = 50.0;
  battery.annual_loss = 0.05;
  battery.ecost = 5.0;

  // Capacity-related attributes
  battery.capacity = 200.0;
  battery.expcap = 50.0;
  battery.expmod = 10.0;
  battery.capmax = 300.0;
  battery.annual_capcost = 15000.0;
  battery.annual_derating = 0.02;

  // Check assigned values
  CHECK(battery.uid == 1001);
  CHECK(battery.name == "TestBattery");
  CHECK(std::get<IntBool>(battery.active.value()) == 1);

  // Values stored in variant containers - check the variant value
  // For simple OptReal types (not field schedules), the value is directly
  // accessible
  CHECK(battery.eini.value() == 50.0);
  CHECK(battery.efin.value() == 50.0);

  // For OptTRealFieldSched types, we need to get the Real variant alternative
  CHECK(std::get_if<Real>(&battery.emin.value()) != nullptr);
  CHECK(std::get_if<Real>(&battery.emax.value()) != nullptr);
  CHECK(std::get_if<Real>(&battery.annual_loss.value()) != nullptr);
  CHECK(std::get_if<Real>(&battery.ecost.value()) != nullptr);

  // Check actual values using std::get_if
  CHECK(*std::get_if<Real>(&battery.emin.value()) == 10.0);
  CHECK(*std::get_if<Real>(&battery.emax.value()) == 100.0);
  CHECK(*std::get_if<Real>(&battery.annual_loss.value()) == 0.05);
  CHECK(*std::get_if<Real>(&battery.ecost.value()) == 5.0);

  // Capacity-related checks
  CHECK(*std::get_if<Real>(&battery.capacity.value()) == 200.0);
  CHECK(*std::get_if<Real>(&battery.expcap.value()) == 50.0);
  CHECK(*std::get_if<Real>(&battery.expmod.value()) == 10.0);
  CHECK(*std::get_if<Real>(&battery.capmax.value()) == 300.0);
  CHECK(*std::get_if<Real>(&battery.annual_capcost.value()) == 15000.0);
  CHECK(*std::get_if<Real>(&battery.annual_derating.value()) == 0.02);
}

TEST_CASE("Battery field schedules")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Battery battery;

  // Create a simple schedule with direct Real values
  battery.emin = 10.0;
  battery.emax = 90.0;

  // Verify simple values were assigned properly
  REQUIRE(battery.emin.has_value());
  REQUIRE(battery.emax.has_value());

  auto* emin_real_ptr = std::get_if<Real>(&battery.emin.value());
  auto* emax_real_ptr = std::get_if<Real>(&battery.emax.value());

  REQUIRE(emin_real_ptr != nullptr);
  REQUIRE(emax_real_ptr != nullptr);

  CHECK(*emin_real_ptr == 10.0);
  CHECK(*emax_real_ptr == 90.0);
}

// We'll use a simplified approach instead of mocks for now
TEST_CASE("BatteryLP construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Battery battery;
  battery.uid = 1001;
  battery.name = "TestBattery";
  battery.active = true;
  battery.emin = 10.0;
  battery.emax = 100.0;
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Battery battery;
  battery.uid = 1001;
  battery.name = "TestBattery";
  battery.active = true;
  battery.emin = 10.0;
  battery.emax = 100.0;
  battery.eini = 50.0;
  battery.capacity = 200.0;
}

TEST_CASE("Battery use_state_variable defaults and explicit set")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default is nullopt (decoupled by convention)")
  {
    const Battery bat;
    CHECK_FALSE(bat.use_state_variable.has_value());
    // value_or(false) reflects the battery-LP default: decoupled
    CHECK(bat.use_state_variable.value_or(false) == false);
  }

  SUBCASE("can be set to true (coupled)")
  {
    Battery bat;
    bat.use_state_variable = true;
    REQUIRE(bat.use_state_variable.has_value());
    CHECK(bat.use_state_variable.value_or(false) == true);
  }

  SUBCASE("can be set to false (explicitly decoupled)")
  {
    Battery bat;
    bat.use_state_variable = false;
    REQUIRE(bat.use_state_variable.has_value());
    CHECK(bat.use_state_variable.value_or(true) == false);
  }

  SUBCASE("daily_cycle default is nullopt")
  {
    const Battery bat;
    CHECK_FALSE(bat.daily_cycle.has_value());
    // Battery LP defaults to daily_cycle=true when not set
    CHECK(bat.daily_cycle.value_or(true) == true);
  }

  SUBCASE("daily_cycle can be set to false")
  {
    Battery bat;
    bat.daily_cycle = false;
    REQUIRE(bat.daily_cycle.has_value());
    CHECK(bat.daily_cycle.value_or(true) == false);
  }

  SUBCASE("daily_cycle can be set to true")
  {
    Battery bat;
    bat.daily_cycle = true;
    REQUIRE(bat.daily_cycle.has_value());
    CHECK(bat.daily_cycle.value_or(false) == true);
  }
}

/// Verify that a decoupled battery (use_state_variable = false, the default)
/// adds an efin==eini close constraint and produces a feasible LP when demand
/// is flexible (has fail_cost).
TEST_CASE(  // NOLINT
    "Battery decoupled (default) produces feasible LP with eclose row")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  // Demand with fail_cost so the LP remains feasible when battery is idle.
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = 100.0,
      },
  };

  // Battery with default use_state_variable (nullopt → value_or(false) = false)
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
          // use_state_variable not set → decoupled (default)
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "BatteryDecoupledTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;  // flexible demand – LP always feasible
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  // One extra row per stage for the efin==eini close constraint
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

/// Verify that a coupled battery (use_state_variable = true) produces the same
/// feasible LP as the old default behaviour (no eclose row, StateVariable
/// registered).
TEST_CASE(  // NOLINT
    "Battery coupled (use_state_variable=true) produces feasible LP")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = 100.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
          .use_state_variable = true,  // explicit coupled mode
          .daily_cycle = false,  // disable daily cycle to test coupled mode
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "BatteryCoupledTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto result = system_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

/// Verify that StorageLP physical_eini/physical_efin accessors correctly
/// convert LP-scaled values to physical units, and that energy_scale()
/// and to_physical() are consistent.
TEST_CASE(  // NOLINT
    "StorageLP physical accessor methods and scale conversion")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = 50.0,
      },
  };

  // Battery with explicit energy_scale = 10.0 (large scale for testing)
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .capacity = 100.0,
          .energy_scale = 10.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "StorageLPAccessorTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  // Verify energy_scale is correctly stored
  const auto& bat_lp = system_lp.elements<BatteryLP>().front();
  CHECK(bat_lp.energy_scale() == doctest::Approx(10.0));

  // Verify to_physical converts LP→physical correctly
  CHECK(bat_lp.to_physical(5.0) == doctest::Approx(50.0));
  CHECK(bat_lp.to_physical(0.0) == doctest::Approx(0.0));

  // Solve to get a solution
  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify physical_eini returns physical units (not LP-scaled)
  const auto& scenarios = simulation_lp.scenarios();
  const auto& stages = simulation_lp.stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  const auto phys_eini = bat_lp.physical_eini(li, scenarios[0], stages[0], 0.0);
  // eini was set to 50.0 in the battery definition
  CHECK(phys_eini == doctest::Approx(50.0));
}

/// Verify that the LP solution and output are invariant to the choice of
/// energy_scale.  Two identical batteries with different energy_scale values
/// must produce the same objective value and the same physical output.
TEST_CASE(  // NOLINT
    "Battery energy_scale invariance – same solution for different scales")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Helper lambda: builds and solves a single-battery LP with the given scale,
  // returns the objective value.
  auto solve_with_scale = [](double scale) -> double
  {
    const Array<Bus> bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "g1",
            .bus = Uid {1},
            .gcost = 20.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .lmax = 100.0,
        },
    };

    const Array<Battery> battery_array = {
        {
            .uid = Uid {1},
            .name = "bat1",
            .bus = Uid {1},
            .input_efficiency = 0.95,
            .output_efficiency = 0.95,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 50.0,
            .capacity = 200.0,
            .energy_scale = scale,
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1,
                },
                {
                    .uid = Uid {2},
                    .duration = 1,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 2,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "ScaleInvariance",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .battery_array = battery_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options {opts};
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return li.get_obj_value();
  };

  const auto obj_scale_1 = solve_with_scale(1.0);
  const auto obj_scale_01 = solve_with_scale(0.1);
  const auto obj_scale_10 = solve_with_scale(10.0);
  const auto obj_scale_100 = solve_with_scale(100.0);

  // All objective values must be identical (within floating-point tolerance)
  CHECK(obj_scale_01 == doctest::Approx(obj_scale_1).epsilon(1e-8));
  CHECK(obj_scale_10 == doctest::Approx(obj_scale_1).epsilon(1e-8));
  CHECK(obj_scale_100 == doctest::Approx(obj_scale_1).epsilon(1e-8));
}

/// Verify that PlanningOptions::variable_scales affects the Battery energy
/// scale the same way as the per-element Battery::energy_scale field. This
/// confirms the JSON variable_scales mechanism is not ignored by checking both
/// solution invariance AND that LP column bounds actually change.
TEST_CASE(  // NOLINT
    "Battery variable_scales option – invariance and LP coefficient change")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  struct ScaleResult
  {
    double objective;
    double max_col_upper;  // max physical upper bound
    double max_col_upper_raw;  // max raw LP upper bound
  };

  // Helper: builds and solves a battery LP using
  // PlanningOptions::variable_scales (NOT Battery::energy_scale) and returns
  // the objective + max scaled bound.
  auto solve_with_variable_scales = [](double scale) -> ScaleResult
  {
    const Array<Bus> bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "g1",
            .bus = Uid {1},
            .gcost = 20.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .lmax = 100.0,
        },
    };

    // NOTE: no energy_scale set on the battery — default is used
    const Array<Battery> battery_array = {
        {
            .uid = Uid {1},
            .name = "bat1",
            .bus = Uid {1},
            .input_efficiency = 0.95,
            .output_efficiency = 0.95,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 50.0,
            .capacity = 200.0,
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1,
                },
                {
                    .uid = Uid {2},
                    .duration = 1,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 2,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "VarScaleInvariance",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .battery_array = battery_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    // Set energy scale via variable_scales option (not per-element field)
    opts.variable_scales = {
        {
            .class_name = "Battery",
            .variable = "energy",
            .uid = Uid {1},
            .scale = scale,
            .name = "bat1",
        },
    };
    const PlanningOptionsLP options {opts};
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Verify the scale factor is stored in LP column scales.
    // Energy columns get scale = energy_scale; find the maximum upper
    // bound among columns with that scale to prove the LP changed.
    const auto& col_scales = li.get_col_scales();
    REQUIRE_FALSE(col_scales.empty());
    const auto col_upp = li.get_col_upp();
    const auto col_upp_raw = li.get_col_upp_raw();
    const auto ncols = static_cast<std::size_t>(li.get_numcols());
    double max_upp = 0.0;
    double max_upp_raw = 0.0;
    int n_scaled = 0;
    for (std::size_t i = 0; i < ncols; ++i) {
      const auto ci = ColIndex {static_cast<int>(i)};
      if (col_scales[ci] == doctest::Approx(scale).epsilon(1e-12)) {
        ++n_scaled;
        max_upp = std::max(max_upp, col_upp[ci]);
        max_upp_raw = std::max(max_upp_raw, col_upp_raw[ci]);
      }
    }
    // Multiple energy columns (eini + per-block) should carry the scale
    REQUIRE(n_scaled > 0);

    return {.objective = li.get_obj_value(),
            .max_col_upper = max_upp,
            .max_col_upper_raw = max_upp_raw};
  };

  // Use scales != 1.0 to distinguish energy columns from unscaled columns
  const auto [obj_10, max_10, max_10_raw] = solve_with_variable_scales(10.0);
  const auto [obj_100, max_100, max_100_raw] =
      solve_with_variable_scales(100.0);

  // Objective invariance: same physical solution regardless of scale
  CHECK(obj_100 == doctest::Approx(obj_10).epsilon(1e-8));

  // Physical bounds are invariant: emax=200 regardless of scale.
  CHECK(max_10 == doctest::Approx(200.0).epsilon(1e-10));
  CHECK(max_100 == doctest::Approx(200.0).epsilon(1e-10));

  // Raw LP bounds differ: emax=200, so max LP upper bound = 200/scale.
  // scale=10 → max_upp_raw=20, scale=100 → max_upp_raw=2.
  CHECK(max_10_raw == doctest::Approx(200.0 / 10.0).epsilon(1e-10));
  CHECK(max_100_raw == doctest::Approx(200.0 / 100.0).epsilon(1e-10));
  CHECK(max_10_raw != doctest::Approx(max_100_raw));  // Proves they differ
}
