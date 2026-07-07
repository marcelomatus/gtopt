/**
 * @file      test_battery_cycle_limit.cpp
 * @brief     Tests for Battery.max_cycles_day daily energy-throughput limit
 * @date      2026-05-27
 * @copyright BSD-3-Clause
 *
 * Verifies the HARD daily-cycle constraint
 *   Σ_blocks (fcr / fout_eff_b) · duration · dc_stage_scale · fout[b]
 *       ≤ N · capacity
 * implemented in BatteryLP::add_to_lp.  Two time-model branches:
 *   - PLP daily-cycle (daily_cycle=true, stage>24h, avg block>1h): exactly
 *     ONE `cycle_limit` row summing all blocks, dc_stage_scale=24/duration.
 *   - PLEXOS chronological (daily_cycle=false, dc_stage_scale=1): one row
 *     per rolling 24h window (per day).
 * The constraint is NOT an objective cost: it never adds an objective term.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/battery_lp.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

namespace test_battery_cycle_limit_ns
{

/// Count rows whose constraint_name == "cycle_limit" in a built LP.
[[nodiscard]] int count_cycle_rows(const LinearInterface& lp)
{
  int n = 0;
  const auto nrows = lp.get_numrows();
  for (Index i = 0; i < nrows; ++i) {
    const auto* lbl = lp.row_label_at(RowIndex {i});
    if (lbl != nullptr && lbl->constraint_name == "cycle_limit") {
      ++n;
    }
  }
  return n;
}

[[nodiscard]] const LinearInterface& first_lp(const PlanningLP& planning_lp)
{
  auto&& systems = planning_lp.systems();
  return systems.front().front().linear_interface();
}

// ---------------------------------------------------------------------------
// Daily-cycle branch (PLP-style): 48 h stage, 2 blocks of 24 h.
//   avg block = 24 h > 1 h, stage = 48 h > 24 h, daily_cycle defaults true.
//   ⇒ exactly ONE cycle_limit row.  Battery is pre-loaded (eini=100,
//   emax=100, capacity=100) and a $1000/MWh generator makes battery
//   discharge ($0) the cheap supply, so the LP wants to discharge as much
//   as possible.  Demand 60 MW × 2 blocks × 24 h.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string daily_cycle_json(double max_cycles_day, bool set_cap)
{
  // ENERGY-ARBITRAGE design so the cap actually bites.  The daily-cycle
  // close constraint pins SoC_end = SoC_start, so net discharge is ~0 —
  // but the battery can still CHARGE in the cheap block and DISCHARGE in
  // the expensive block within the day.  The cycle_limit caps that
  // round-trip throughput.
  //   gcost = [1, 1000] (block1 cheap, block2 expensive); demand 60 MW
  //   each block; eini = 50 (room to charge).  Without a cap the battery
  //   shifts a lot of energy from block1→block2; a tight N=1 cap throttles
  //   the throughput, forcing more block2 demand onto the $1000 generator
  //   ⇒ higher objective.
  std::string cap_field =
      set_cap ? R"(, "max_cycles_day": )" + std::to_string(max_cycles_day) : "";
  return R"(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 5000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 24 },
        { "uid": 2, "duration": 24 }
      ],
      "stage_array": [ { "uid": 1, "first_block": 0, "count_block": 2 } ],
      "scenario_array": [ { "uid": 1, "probability_factor": 1 } ]
    },
    "system": {
      "name": "bat_daily_cycle",
      "bus_array": [ { "uid": 1, "name": "b1" } ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": 1, "gcost": [[1, 1000]],
          "capacity": 200 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d1", "bus": 1, "lmax": [[60, 60]] }
      ],
      "battery_array": [
        {
          "uid": 1,
          "name": "bess1",
          "bus": 1,
          "input_efficiency": 1.0,
          "output_efficiency": 1.0,
          "emin": 0,
          "emax": 100,
          "eini": 50,
          "pmax_charge": 60,
          "pmax_discharge": 60,
          "discharge_cost": 0,
          "capacity": 100,
          "use_state_variable": true,
          "daily_cycle": true)"
      + cap_field + R"(
        }
      ]
    }
  }
  )";
}

// ---------------------------------------------------------------------------
// Chronological branch (PLEXOS-style): 48 h stage, 48 blocks of 1 h.
//   avg block = 1 h (NOT > 1 h) ⇒ use_daily_cycle=false ⇒ dc_stage_scale=1.
//   Two rolling 24 h windows ⇒ TWO cycle_limit rows.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string chronological_json(double max_cycles_day,
                                             bool set_cap)
{
  // 48 hourly blocks; demand 60 MW flat.  gcost alternates cheap/
  // expensive hour-by-hour ([1, 1000, 1, 1000, ...]) so the battery has a
  // strong intra-day arbitrage incentive — far more throughput than a
  // single 100 MWh cycle.  The N=1 per-day cap (≤ 100 MWh per 24 h window)
  // therefore BINDS, and tightening it raises cost.
  std::string blocks;
  std::string lmax_row;
  std::string gcost_row;
  for (int i = 1; i <= 48; ++i) {
    blocks += "{ \"uid\": " + std::to_string(i) + ", \"duration\": 1 }";
    lmax_row += "60";
    gcost_row += (i % 2 == 1) ? "1" : "1000";
    if (i != 48) {
      blocks += ", ";
      lmax_row += ", ";
      gcost_row += ", ";
    }
  }
  std::string cap_field =
      set_cap ? R"(, "max_cycles_day": )" + std::to_string(max_cycles_day) : "";
  return R"(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 5000
      }
    },
    "simulation": {
      "block_array": [ )"
      + blocks + R"( ],
      "stage_array": [ { "uid": 1, "first_block": 0, "count_block": 48 } ],
      "scenario_array": [ { "uid": 1, "probability_factor": 1 } ]
    },
    "system": {
      "name": "bat_chrono",
      "bus_array": [ { "uid": 1, "name": "b1" } ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": 1, "gcost": [[)"
      + gcost_row + R"(]], "capacity": 200 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d1", "bus": 1, "lmax": [[)"
      + lmax_row + R"(]] }
      ],
      "battery_array": [
        {
          "uid": 1,
          "name": "bess1",
          "bus": 1,
          "input_efficiency": 1.0,
          "output_efficiency": 1.0,
          "emin": 0,
          "emax": 100,
          "eini": 50,
          "pmax_charge": 60,
          "pmax_discharge": 60,
          "discharge_cost": 0,
          "capacity": 100,
          "use_state_variable": true,
          "daily_cycle": false)"
      + cap_field + R"(
        }
      ]
    }
  }
  )";
}

[[nodiscard]] double solve_obj(const std::string& json)
{
  Planning base;
  base.merge(parse_planning_json(json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);
  return first_lp(planning_lp).get_obj_value_raw();
}

}  // namespace test_battery_cycle_limit_ns

TEST_CASE(
    "BatteryLP cycle_limit — daily-cycle emits exactly ONE row")  // NOLINT
{
  using namespace test_battery_cycle_limit_ns;

  Planning base;
  base.merge(parse_planning_json(daily_cycle_json(1.0, /*set_cap=*/true)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  const auto& lp = first_lp(planning_lp);
  // Daily-cycle branch: stage (48 h) rescaled to one day ⇒ a single
  // cycle_limit row summing all blocks of the stage.
  CHECK(count_cycle_rows(lp) == 1);
}

TEST_CASE("BatteryLP cycle_limit — tight cap raises cost / caps discharge")
{
  using namespace test_battery_cycle_limit_ns;

  // Unconstrained: the battery arbitrages freely (charge cheap block,
  // discharge expensive block) up to its SoC room.
  const double obj_unconstrained =
      solve_obj(daily_cycle_json(0.0, /*set_cap=*/false));

  // Tight cap (N=0.3 ⇒ Σ discharge·Δt ≤ 30 MWh/day, cell-side) throttles
  // the round-trip throughput below the SoC-room limit, so less expensive-
  // block demand is displaced and the $1000 generator covers more ⇒ cost
  // rises.  (N=1 here would not bind: the emax−eini SoC headroom already
  // caps throughput below 1 cycle/day.)
  const double obj_tight = solve_obj(daily_cycle_json(0.3, /*set_cap=*/true));

  // The HARD cap can only make the LP cost worse (higher) or equal.
  CHECK(obj_tight > obj_unconstrained);
}

TEST_CASE("BatteryLP cycle_limit — chronological emits one row per day")
{
  using namespace test_battery_cycle_limit_ns;

  Planning base;
  base.merge(parse_planning_json(chronological_json(1.0, /*set_cap=*/true)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  const auto& lp = first_lp(planning_lp);
  // 48 h of 1 h blocks ⇒ two rolling 24 h windows ⇒ TWO cycle_limit rows.
  CHECK(count_cycle_rows(lp) == 2);
}

TEST_CASE("BatteryLP cycle_limit — chronological tight cap raises cost")
{
  using namespace test_battery_cycle_limit_ns;

  const double obj_unconstrained =
      solve_obj(chronological_json(0.0, /*set_cap=*/false));
  const double obj_tight = solve_obj(chronological_json(1.0, /*set_cap=*/true));

  CHECK(obj_tight > obj_unconstrained);
}

TEST_CASE("BatteryLP cycle_limit — unset ⇒ no row, objective unchanged")
{
  using namespace test_battery_cycle_limit_ns;

  // Daily-cycle JSON with NO max_cycles_day field.
  Planning base;
  base.merge(parse_planning_json(daily_cycle_json(0.0, /*set_cap=*/false)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  const auto& lp = first_lp(planning_lp);
  CHECK(count_cycle_rows(lp) == 0);
}

TEST_CASE("BatteryLP cycle_limit — discharge-loss accounting (cell-side)")
{
  using namespace test_battery_cycle_limit_ns;

  // With output_efficiency < 1 the cap binds on CELL-side throughput:
  // the coefficient is (fcr / fout_eff) · duration · dc_stage_scale, so a
  // less-efficient battery hits the same N·capacity cap with LESS grid-side
  // discharge.  Reuse the daily-cycle arbitrage system (gcost [1, 1000],
  // eini = 50) where the N=1 cap binds; lowering output_efficiency from
  // 1.0 to 0.8 raises the per-MW cell coefficient (1/0.8 vs 1/1.0), so the
  // same RHS permits less grid-side discharge ⇒ less expensive-block
  // displacement ⇒ higher objective.
  auto with_eff = [](double eff)
  {
    std::string j = R"(
    {
      "options": {
        "annual_discount_rate": 0.0,
        "output_compression": "uncompressed",
        "model_options": {
          "use_single_bus": true,
          "scale_objective": 1,
          "demand_fail_cost": 5000
        }
      },
      "simulation": {
        "block_array": [
          { "uid": 1, "duration": 24 },
          { "uid": 2, "duration": 24 }
        ],
        "stage_array": [ { "uid": 1, "first_block": 0, "count_block": 2 } ],
        "scenario_array": [ { "uid": 1, "probability_factor": 1 } ]
      },
      "system": {
        "name": "bat_loss",
        "bus_array": [ { "uid": 1, "name": "b1" } ],
        "generator_array": [
          { "uid": 1, "name": "g1", "bus": 1, "gcost": [[1, 1000]],
            "capacity": 200 }
        ],
        "demand_array": [
          { "uid": 1, "name": "d1", "bus": 1, "lmax": [[60, 60]] }
        ],
        "battery_array": [
          {
            "uid": 1,
            "name": "bess1",
            "bus": 1,
            "input_efficiency": 1.0,
            "output_efficiency": )"
        + std::to_string(eff) + R"(,
            "emin": 0,
            "emax": 100,
            "eini": 50,
            "pmax_charge": 60,
            "pmax_discharge": 60,
            "discharge_cost": 0,
            "capacity": 100,
            "use_state_variable": true,
            "daily_cycle": true,
            "max_cycles_day": 1
          }
        ]
      }
    }
    )";
    return j;
  };

  const double obj_eff_100 = solve_obj(with_eff(1.0));
  const double obj_eff_80 = solve_obj(with_eff(0.8));

  // Same cell-side cap, but the 80%-efficient battery delivers less to the
  // grid, so the $1000 generator must cover more demand ⇒ higher cost.
  CHECK(obj_eff_80 > obj_eff_100);
}
