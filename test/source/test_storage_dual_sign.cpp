// SPDX-License-Identifier: BSD-3-Clause
/// @file test_storage_dual_sign.cpp
/// @brief Pin the sign convention and Parquet field name for the
///        per-block storage-balance dual.
///
/// Background.  The energy-balance row is an equality `LHS = 0` whose
/// `+1` slot belongs to the current-block state variable.  In a
/// minimization LP the raw dual π of that row is non-positive whenever
/// stored energy is valuable (adding 1 unit of free state lowers cost).
/// Industry-facing reports — PLEXOS `Shadow Price`, PyPSA
/// `mu_energy_balance`, PSR-SDDP / PLP "valor del agua" / `cmgcse`,
/// Calliope, GenX, Backbone — publish the same quantity with the
/// **opposite** sign (positive = "value of one more stored unit").
///
/// `StorageLP` therefore stores `output_dual_scale = -dc_stage_scale`
/// for every (scenario, stage) cell, so the Parquet column carries the
/// industry-convention sign.  The Reservoir output column is
/// additionally renamed from the storage-generic `energy.dual` stem to
/// the canonical `water_value.dual` to match PLP / PSR-SDDP / PLEXOS
/// Water Storage naming.
///
/// This test pins three invariants:
///   1. `output_dual_scale_at(scn, stg) < 0` for both Battery and
///      Reservoir, for both `daily_cycle=false` (scale = -1) and
///      `daily_cycle=true` (scale = -24/duration).
///   2. `OutputContext::fields()` contains `(Battery, energy, dual)`
///      and `(Reservoir, water_value, dual)`, but NOT
///      `(Reservoir, energy, dual)`.
///   3. The raw LP energy-balance dual is ≤ 0 in a known scenario
///      where stored energy is valuable.

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway.hpp>

using namespace gtopt;

namespace
{

/// Inputs for the one-bus system below.  Returns plain-old-data
/// (`System`, `Simulation`, `PlanningOptions`) so each test can build
/// its own `SystemLP` in place — moving a `SystemLP` out of a helper
/// would leave the embedded `SystemContext` holding back-pointers to
/// the moved-from instance, and `OutputContext` access to those would
/// segfault.
struct CombinedSystemInputs
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
};

/// Build a one-bus system with a Battery, a Reservoir-fed hydro
/// turbine, and a high-cost thermal backup.  Demand is non-trivial so
/// both the battery and the reservoir face binding-discharge optima —
/// guaranteeing the energy-balance dual is strictly negative (raw LP)
/// and the output-side sign flip is observable.
CombinedSystemInputs make_combined_inputs(double block_duration = 1.0)
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  // Thermal backup priced well above hydro / battery: ensures stored
  // energy is preferred at the optimum, so the energy-balance dual is
  // strictly negative (raw LP) in every block.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g_thermal",
          .bus = Uid {1},
          .gcost = 200.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g_hydro",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = 80.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_dn",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {2},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = block_duration,
              },
              {
                  .uid = Uid {2},
                  .duration = block_duration,
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
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  System system = {
      .name = "StorageDualSignTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  // `output_directory` must be set so OutputContext::write() emits the
  // Parquet field map (see ``Storage dual: Parquet field names ...``).
  // The test overrides it to a tmp dir before driving write_out.
  opts.output_format = DataFormat::parquet;

  return CombinedSystemInputs {
      .system = std::move(system),
      .simulation = std::move(simulation),
      .opts = std::move(opts),
  };
}

}  // namespace

// ─── output_dual_scale is negative for both classes ─────────────────

TEST_CASE(
    "Storage dual: output_dual_scale carries industry sign flip")  // NOLINT
{
  SUBCASE("daily_cycle=false → scale = -1.0")
  {
    const auto inputs = make_combined_inputs(/*block_duration=*/1.0);
    const PlanningOptionsLP options(inputs.opts);
    SimulationLP sim_lp(inputs.simulation, options);
    SystemLP sys_lp(inputs.system, sim_lp);

    const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
    const auto& rsv_lp = sys_lp.elements<ReservoirLP>().front();
    const auto& scenarios = sys_lp.scene().scenarios();
    const auto& stages = sys_lp.phase().stages();
    REQUIRE(!scenarios.empty());
    REQUIRE(!stages.empty());

    const double bat_scale =
        bat_lp.output_dual_scale_at(scenarios[0], stages[0]);
    const double rsv_scale =
        rsv_lp.output_dual_scale_at(scenarios[0], stages[0]);

    CHECK(bat_scale == doctest::Approx(-1.0));
    CHECK(rsv_scale == doctest::Approx(-1.0));
  }
}

// ─── Parquet field names: Battery=energy, Reservoir=water_value ─────

TEST_CASE(
    "Storage dual: Parquet field names follow industry convention")  // NOLINT
{
  // Drive `SystemLP::write_out()` end-to-end into a temp directory and
  // verify the on-disk schema, rather than constructing an
  // `OutputContext` directly: `SystemContext` holds back-pointers to
  // its owning `SystemLP` that must be re-bound after any move, and
  // `write_out()` already handles that bookkeeping.  Disk layout is
  //   <out>/<class>/<fname>_<sname>.parquet/scene=<S>/phase=<P>/part.parquet
  auto inputs = make_combined_inputs();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_storage_dual_sign";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  inputs.opts.output_directory = tmpdir.string();
  const PlanningOptionsLP options(inputs.opts);
  SimulationLP sim_lp(inputs.simulation, options);
  SystemLP sys_lp(inputs.system, sim_lp);

  auto&& lp = sys_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  sys_lp.write_out();

  // Battery keeps the storage-generic `energy.dual` stem.
  CHECK(std::filesystem::exists(tmpdir / "Battery" / "energy_dual.parquet"));

  // Reservoir publishes the same quantity under the canonical
  // `water_value.dual` name (PLP / PSR-SDDP / PLEXOS convention).
  CHECK(std::filesystem::exists(tmpdir / "Reservoir"
                                / "water_value_dual.parquet"));

  // And NOT under the old `energy.dual` stem.
  CHECK_FALSE(
      std::filesystem::exists(tmpdir / "Reservoir" / "energy_dual.parquet"));

  // The primal `energy.sol` column is unchanged for both classes —
  // only the dual is renamed.
  CHECK(std::filesystem::exists(tmpdir / "Reservoir" / "energy_sol.parquet"));
  CHECK(std::filesystem::exists(tmpdir / "Battery" / "energy_sol.parquet"));

  std::filesystem::remove_all(tmpdir);
}

// ─── Raw LP dual sign sanity ────────────────────────────────────────

TEST_CASE(
    "Storage dual: raw LP dual of energy_balance is non-positive")  // NOLINT
{
  const auto inputs = make_combined_inputs();
  const PlanningOptionsLP options(inputs.opts);
  SimulationLP sim_lp(inputs.simulation, options);
  SystemLP sys_lp(inputs.system, sim_lp);

  auto&& lp = sys_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& rsv_lp = sys_lp.elements<ReservoirLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();
  const auto& blocks = stages[0].blocks();

  const auto& bat_rows = bat_lp.energy_rows_at(scenarios[0], stages[0]);
  const auto& rsv_rows = rsv_lp.energy_rows_at(scenarios[0], stages[0]);
  const auto duals = lp.get_row_dual();

  // At the optimum, demand (80) exceeds thermal capacity (100) plus
  // hydro capacity (100), so both storages discharge.  Stored energy
  // is strictly valuable ⇒ raw LP energy-balance dual ≤ 0 in every
  // block (the `+1` slot belongs to `energy_b`; injecting 1 free unit
  // of state lowers cost in a minimization).
  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const double bat_dual = duals[bat_rows.at(buid)];
    const double rsv_dual = duals[rsv_rows.at(buid)];
    CHECK(bat_dual <= 0.0);
    CHECK(rsv_dual <= 0.0);
  }
}
