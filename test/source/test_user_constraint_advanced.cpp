/**
 * @file      test_user_constraint_advanced.cpp
 * @brief     Advanced user constraint tests: type filters, F4/F5/F7 lowering,
 * penalty, soft constraints
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

#include "log_capture.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// clang-format off

/// Single-bus case with a tight generator capacity constraint to produce a
/// non-zero dual on the user constraint row.
static constexpr std::string_view single_bus_uc_dual_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_dual_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "gen_upper",
        "expression": "generator('g1').generation <= 80",
        "constraint_type": "power"
      }
    ]
  }
})json";

// clang-format on

// clang-format off

/// Sum with type filter: sum(generator(all, type="thermal").generation).
static constexpr std::string_view uc_type_filter_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_type_filter",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20,
       "capacity": 200, "type": "thermal"},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 10,
       "capacity": 100, "type": "solar"}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_thermal_only",
        "expression": "sum(generator(all, type='thermal').generation) <= 150"
      },
      {
        "uid": 2, "name": "uc_solar_only",
        "expression": "sum(generator(all, type='solar').generation) <= 80"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - sum with type filter")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_type_filter_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// F4: sum with new-style `sum(... : pred and pred)` multi-predicate filter.
/// Only generators of type=thermal at bus=1 are summed — here just g1.
static constexpr std::string_view uc_f4_multi_pred_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_f4_multi_pred",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20,
       "capacity": 200, "type": "thermal"},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 10,
       "capacity": 100, "type": "solar"}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_f4_thermal_at_b1",
        "expression": "sum(generator(all : type='thermal' and bus=1).generation) <= 150"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - F4 multi-predicate sum filter (new syntax)")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_f4_multi_pred_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Battery drain alias for spill, and battery charge/discharge
/// in a binding constraint scenario to verify the constraint is effective.
// Note: a previous test exercised a `battery('bat').drain` alias here,
// but the Battery struct has no drain field — drain is a Reservoir/
// generic Storage feature that BatteryLP does not opt into.  The test
// was passing only because of the silent-skip bug in user-constraint
// element-ref resolution.  If battery drain support is added, restore
// the test using a `drain_cost`-bearing Battery entry.

// ══════════════════════════════════════════════════════════════════════════════
// Additional coverage tests for converter, seepage, reserve_zone,
// reserve_provision element types in resolve_single_col and collect_sum_cols,
// plus RANGE constraint type in apply_constraint_bounds.
// ══════════════════════════════════════════════════════════════════════════════

// clang-format off

/// System with a converter (battery + generator + demand) and user constraints
/// referencing converter discharge and charge attributes.
static constexpr std::string_view uc_converter_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_converter",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "gen_charge", "bus": 1, "gcost": 10, "capacity": 200},
      {"uid": 2, "name": "thermal", "bus": 1, "gcost": 100, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "dem_discharge", "bus": 1, "capacity": 100},
      {"uid": 2, "name": "d_load", "bus": 1, "capacity": 50}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1",
        "input_efficiency": 0.95, "output_efficiency": 0.95,
        "emin": 0, "emax": 100, "eini": 50,
        "pmax_charge": 100, "pmax_discharge": 100,
        "gcost": 0, "capacity": 100
      }
    ],
    "converter_array": [
      {
        "uid": 1, "name": "conv1",
        "battery": 1, "generator": 1, "demand": 1,
        "capacity": 200
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_conv_discharge",
        "expression": "converter('conv1').discharge <= 150"
      },
      {
        "uid": 2, "name": "uc_conv_charge",
        "expression": "converter('conv1').charge <= 80"
      },
      {
        "uid": 3, "name": "uc_sum_conv_all",
        "expression": "sum(converter(all).discharge) <= 180"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - converter discharge and charge attributes")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_converter_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// System with seepage (waterway -> reservoir seepage) and user constraints
/// referencing seepage flow attribute.  The hydro topology uses a single
/// junction chain: j1 -> ww1 -> j_down, with a second waterway ww_filt from
/// j1 -> j_down carrying the seepage flow.  Both inflow and seepage feed
/// junction j1, and j_down drains everything.
static constexpr std::string_view uc_seepage_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_seepage",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "hg1", "bus": 1, "gcost": 5, "capacity": 200},
      {"uid": 2, "name": "thermal", "bus": 1, "gcost": 100, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
    ],
    "junction_array": [
      {"uid": 1, "name": "j1"},
      {"uid": 2, "name": "j_down", "drain": true}
    ],
    "waterway_array": [
      {"uid": 1, "name": "ww1", "junction_a": 1, "junction_b": 2, "fmin": 0, "fmax": 500},
      {"uid": 2, "name": "ww_filt", "junction_a": 1, "junction_b": 2, "fmin": 0, "fmax": 100}
    ],
    "flow_array": [
      {"uid": 1, "name": "inflow1", "direction": 1, "junction": 1, "discharge": 20}
    ],
    "reservoir_array": [
      {"uid": 1, "name": "rsv1", "junction": 1, "capacity": 1000, "emin": 0, "emax": 1000, "eini": 500}
    ],
    "turbine_array": [
      {"uid": 1, "name": "tur1", "waterway": 1, "generator": 1, "production_factor": 1.0}
    ],
    "reservoir_seepage_array": [
      {
        "uid": 1, "name": "filt1",
        "waterway": 2, "reservoir": 1,
        "slope": 0.02, "constant": 0.5
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_filt_flow",
        "expression": "seepage('filt1').flow <= 80"
      },
      {
        "uid": 3, "name": "uc_sum_filt_all",
        "expression": "sum(seepage(all).flow) <= 100"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - seepage flow attribute")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_seepage_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// System with reserve zones and provisions, and user constraints referencing
/// reserve_zone up/dn requirement and reserve_provision up/dn provision.
static constexpr std::string_view uc_reserve_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "reserve_fail_cost": 10000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_reserve",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 20, "capacity": 400}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 100}
    ],
    "reserve_zone_array": [
      {
        "uid": 1, "name": "rz1",
        "urreq": 50, "drreq": 30,
        "urcost": 1000, "drcost": 800
      }
    ],
    "reserve_provision_array": [
      {
        "uid": 1, "name": "rp1",
        "generator": 1, "reserve_zones": "1",
        "urmax": 100, "drmax": 80,
        "ur_provision_factor": 1.0,
        "dr_provision_factor": 1.0
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_rz_up",
        "expression": "reserve_zone('rz1').up <= 80"
      },
      {
        "uid": 2, "name": "uc_rz_urequirement",
        "expression": "reserve_zone('rz1').up <= 70"
      },
      {
        "uid": 3, "name": "uc_rz_dn",
        "expression": "reserve_zone('rz1').dn <= 60"
      },
      {
        "uid": 4, "name": "uc_rz_drequirement",
        "expression": "reserve_zone('rz1').dn <= 50"
      },
      {
        "uid": 5, "name": "uc_rp_up",
        "expression": "reserve_provision('rp1').up <= 90"
      },
      {
        "uid": 6, "name": "uc_rp_uprovision",
        "expression": "reserve_provision('rp1').up <= 85"
      },
      {
        "uid": 7, "name": "uc_rp_dn",
        "expression": "reserve_provision('rp1').dn <= 70"
      },
      {
        "uid": 8, "name": "uc_rp_dprovision",
        "expression": "reserve_provision('rp1').dn <= 65"
      },
      {
        "uid": 9, "name": "uc_sum_rz_all",
        "expression": "sum(reserve_zone(all).up) <= 200"
      },
      {
        "uid": 10, "name": "uc_sum_rp_all",
        "expression": "sum(reserve_provision(all).up) <= 200"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE(
    "User constraint - reserve zone and provision "
    "attributes (up/dn)")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_reserve_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Test the RANGE constraint type in apply_constraint_bounds.
/// Syntax: "10 <= generator(...).generation <= 150" or equivalent.
/// The ConstraintParser should produce ConstraintType::RANGE with
/// lower_bound and upper_bound.
static constexpr std::string_view uc_range_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_range",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_range_bound",
        "expression": "10 <= generator('g1').generation <= 150"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - RANGE constraint type (double-sided bound)")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_range_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Test unknown element type hitting the final warning path (line 460-462).
/// The existing unknown_ref test uses "widget" which does not match any known
/// type, triggering the catch-based path. This test ensures the warning path
/// for truly unknown element types is reached.
static constexpr std::string_view uc_demand_unknown_attr_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "constraint_mode": "normal"
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_demand_unknown_attr",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_demand_bogus_attr",
        "expression": "demand('d1').bogus_attr <= 100"
      },
      {
        "uid": 2, "name": "uc_line_bogus_attr",
        "expression": "line('nonexistent').bogus <= 100"
      },
      {
        "uid": 3, "name": "uc_battery_bogus_attr",
        "expression": "battery('nonexistent').bogus <= 100"
      },
      {
        "uid": 4, "name": "uc_reservoir_bogus_attr",
        "expression": "reservoir('nonexistent').bogus <= 100"
      },
      {
        "uid": 5, "name": "uc_bus_bogus_attr",
        "expression": "bus('nonexistent').power <= 100"
      },
      {
        "uid": 6, "name": "uc_junction_bogus",
        "expression": "junction('nonexistent').bogus <= 100"
      },
      {
        "uid": 7, "name": "uc_flow_bogus",
        "expression": "flow('nonexistent').bogus <= 100"
      },
      {
        "uid": 8, "name": "uc_waterway_bogus",
        "expression": "waterway('nonexistent').bogus <= 100"
      },
      {
        "uid": 9, "name": "uc_turbine_bogus",
        "expression": "turbine('nonexistent').bogus <= 100"
      },
      {
        "uid": 10, "name": "uc_converter_bogus",
        "expression": "converter('nonexistent').bogus <= 100"
      },
      {
        "uid": 11, "name": "uc_seepage_bogus",
        "expression": "seepage('nonexistent').bogus <= 100"
      },
      {
        "uid": 12, "name": "uc_reserve_provision_bogus",
        "expression": "reserve_provision('nonexistent').bogus <= 100"
      },
      {
        "uid": 13, "name": "uc_reserve_zone_bogus",
        "expression": "reserve_zone('nonexistent').bogus <= 100"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE(
    "User constraint - unknown/missing attributes for all element types "
    "(graceful skip)")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_demand_unknown_attr_json);
  PlanningLP planning_lp(std::move(planning));

  // Should solve — all constraints with unknown/nonexistent refs are skipped
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

// clang-format off

/// Exercise the named-parameter (UserParamMap) resolution path in
/// user_constraint_lp.cpp (resolve_param + param_shift accumulation for both
/// scalar and monthly-indexed parameters).
static constexpr std::string_view uc_named_param_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "active": 1, "first_block": 0, "count_block": 1, "month": "april"}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_named_param",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 20, "capacity": 500}
    ],
    "user_param_array": [
      {"name": "pct_elec", "value": 35.0},
      {"name": "irr_seasonal", "monthly": [0, 0, 0, 100, 100, 100, 100, 100, 100, 100, 0, 0]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_scalar_param",
        "expression": "generator('g1').generation <= pct_elec + 200"
      },
      {
        "uid": 2, "name": "uc_monthly_param",
        "expression": "generator('g1').generation <= irr_seasonal + 150"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - named parameter resolution (scalar + monthly)")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_named_param_json);
  PlanningLP planning_lp(std::move(planning));

  // Both constraints resolve their param references via UserParamMap;
  // the resulting RHS leaves room for the 80 MW demand.
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

// clang-format off

/// Same case but with constraint_mode = "normal" → the unknown parameter
/// emits a warning and the term is skipped (no throw).
static constexpr std::string_view uc_normal_unknown_param_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "constraint_mode": "normal"
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_normal_unknown_param",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_missing_param_normal",
        "expression": "generator('g1').generation <= nonexistent_param + 180"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE(
    "User constraint - normal mode skips unknown named parameter with warning")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_normal_unknown_param_json);
  PlanningLP planning_lp(std::move(planning));

  // Should solve: the unresolved parameter term is simply skipped.
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

// ── F5: abs(x) lowering — end-to-end planning smoke tests ───────────────

// clang-format off
static constexpr std::string_view uc_abs_ok_json = R"json({
  "options": {
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_abs_ok",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[50.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "abs_cap",
        "expression": "abs(generator('g1').generation - 50) <= 50"
      }
    ]
  }
})json";
// clang-format on

TEST_CASE("User constraint - F5 abs(x) lowering (convex <=) solves")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_abs_ok_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// clang-format off
static constexpr std::string_view uc_abs_nonconvex_json = R"json({
  "options": {
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "constraint_mode": "strict"
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_abs_bad",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[50.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "abs_nonconvex",
        "expression": "abs(generator('g1').generation) >= 10"
      }
    ]
  }
})json";
// clang-format on

TEST_CASE("User constraint - F5 non-convex abs(x) >= k is rejected")
{
  using namespace gtopt;

  // With `constraint_mode: strict` (the default), a non-convex
  // `abs(x) >= k` (c > 0) must surface as an exception during LP
  // build so the author sees the problem instead of silently
  // dropping a constraint that influences dispatch.
  auto planning = parse_planning_json(uc_abs_nonconvex_json);
  CHECK_THROWS_AS(PlanningLP(std::move(planning)),  // NOLINT
                  std::runtime_error);
}

// ── F7: min/max(arg1, arg2, ...) lowering ───────────────────────────────

// clang-format off
static constexpr std::string_view uc_max_ok_json = R"json({
  "options": {
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_max_ok",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 10, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[120.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "max_cap",
        "expression": "max(generator('g1').generation, generator('g2').generation) <= 80"
      }
    ]
  }
})json";
// clang-format on

TEST_CASE("User constraint - F7 max(a,b) <= k lowers and solves")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_max_ok_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// clang-format off
static constexpr std::string_view uc_min_ok_json = R"json({
  "options": {
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_min_ok",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 25, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "min_floor",
        "expression": "min(generator('g1').generation, generator('g2').generation) >= 5"
      }
    ]
  }
})json";
// clang-format on

TEST_CASE("User constraint - F7 min(a,b) >= k lowers and solves")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_min_ok_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// ── F8: if-then-else data-only conditional ──────────────────────────────

// clang-format off
static constexpr std::string_view uc_if_stage_json = R"json({
  "options": {
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1},
      {"uid": 2, "first_block": 0, "count_block": 1}
    ],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_if_stage",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[40.0], [40.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "if_by_stage",
        "expression": "if stage = 1 then (generator('g1').generation) else (generator('g1').generation) <= 60"
      }
    ]
  }
})json";
// clang-format on

TEST_CASE(
    "User constraint - F8 if-then-else lowers per (scenario, stage, block)")
{
  using namespace gtopt;

  auto planning = parse_planning_json(uc_if_stage_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// ── F5: error path — non-convex abs error text reaches spdlog ───────────

TEST_CASE(
    "User constraint - non-convex abs(x) surfaces a diagnostic on the logger")
{
  using namespace gtopt;

  // Run the failing build under a log sink so we can verify the
  // diagnostic's *content* (not just that something threw).  The
  // catch re-throws the original exception in strict mode; the
  // SystemLP boundary logs the error text via SPDLOG_ERROR on its
  // way out.
  gtopt::test::LogCapture logs;

  auto planning = parse_planning_json(uc_abs_nonconvex_json);
  CHECK_THROWS_AS(PlanningLP(std::move(planning)),  // NOLINT
                  std::runtime_error);

  // Message must name the offending feature ("abs") and the
  // constraint name so the author can locate it.
  CHECK(logs.contains("abs_nonconvex"));
  CHECK(logs.contains("non-convex"));
  CHECK(logs.contains("abs"));
}

// ── F5: abs() over a line.flow pseudo-attribute expands cleanly ─────────

// clang-format off
static constexpr std::string_view uc_abs_line_flow_json = R"json({
  "options": {
    "output_compression": "uncompressed",
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_abs_line_flow",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "line_array": [
      {"uid": 1, "name": "l1", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.1, "tmax_ab": 100, "tmax_ba": 100}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100,
       "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b2", "lmax": [[50.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "cap_abs_flow",
        "expression": "abs(line('l1').flow) <= 100"
      }
    ]
  }
})json";
// clang-format on

TEST_CASE("User constraint - F5 abs(line.flow) lowers via flowp - flown")
{
  using namespace gtopt;

  // line.flow is not a primitive LP column — it is expanded at parse
  // time into `flowp − flown`.  Wrapping it in abs() must still lower
  // correctly and produce a feasible LP.  This test locks that path
  // so a future regression doesn't silently turn abs(line.flow) into
  // a no-op.
  auto planning = parse_planning_json(uc_abs_line_flow_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// -- Soft constraints (Tier 0+ A4) --------------------------------------

// clang-format off

/// Soft `<=` constraint: a tight cap below the natural dispatch
/// optimum.  With penalty < generator gcost, the LP must prefer paying
/// the slack penalty over forgoing dispatch — slack > 0, problem still
/// feasible.  Without the soft sugar this would either force load
/// curtailment or refuse to relax the bound.
static constexpr std::string_view soft_le_uc_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "soft_le_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "soft_cap",
        "expression": "generator('g1').generation <= 50",
        "constraint_type": "raw",
        "penalty": 5
      }
    ]
  }
})json";

/// Soft `=` constraint: an equality with two slacks.  Demand of 90
/// MW would normally be split between dispatch and demand failure,
/// but the soft equality `gen = 70` introduces both slacks so the
/// optimizer can deviate either way at penalty cost.
static constexpr std::string_view soft_eq_uc_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "soft_eq_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "soft_target",
        "expression": "generator('g1').generation = 70",
        "constraint_type": "raw",
        "penalty": 1
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - penalty field round-trips through JSON")
{
  using namespace gtopt;

  auto planning = parse_planning_json(soft_le_uc_json);
  REQUIRE(planning.system.user_constraint_array.size() == 1);
  const auto& uc = planning.system.user_constraint_array[0];

  REQUIRE(uc.penalty.has_value());
  CHECK(uc.penalty.value_or(0.0) == doctest::Approx(5.0));
}

TEST_CASE("User constraint - missing penalty field defaults to nullopt")
{
  using namespace gtopt;

  // The legacy fixture has no `penalty` field at all — verify that
  // the optional stays empty rather than defaulting to 0 (a future
  // refactor might be tempted to coerce missing → 0 and accidentally
  // turn every constraint into a soft one).
  auto planning = parse_planning_json(single_bus_uc_dual_json);
  REQUIRE(planning.system.user_constraint_array.size() == 1);
  CHECK_FALSE(planning.system.user_constraint_array[0].penalty.has_value());
}

TEST_CASE("User constraint - soft `<=` builds and solves")
{
  using namespace gtopt;

  // The constraint `g1.generation <= 50` is tighter than the natural
  // dispatch optimum (90 MW demand at gcost 20).  With penalty=5, the
  // LP must add a slack column with cost 5 and let the constraint be
  // violated.  Without the soft sugar this would force the optimizer
  // to leave 40 MW of demand unserved at fail_cost=1000.
  auto planning = parse_planning_json(soft_le_uc_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

TEST_CASE("User constraint - soft `=` builds and solves with two slacks")
{
  using namespace gtopt;

  auto planning = parse_planning_json(soft_eq_uc_json);
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

TEST_CASE("User constraint - soft slack columns appear in CSV output")
{
  using namespace gtopt;

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_uc_soft_slack_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning = parse_planning_json(soft_le_uc_json);
  planning.options.output_directory = tmpdir.string();

  const PlanningOptionsLP options(planning.options);
  SimulationLP sim_lp(planning.simulation, options);
  SystemLP sys_lp(planning.system, sim_lp);

  auto res = sys_lp.linear_interface().resolve();
  REQUIRE(res.has_value());

  sys_lp.write_out();

  // The visible-slack contract requires the slack primal value AND
  // its realized cost to land in the standard output stream.
  const auto sol_file = tmpdir / "UserConstraint" / "slack_sol.csv";
  const auto cost_file = tmpdir / "UserConstraint" / "slack_cost.csv";
  CHECK(std::filesystem::exists(sol_file));
  CHECK(std::filesystem::exists(cost_file));

  if (std::filesystem::exists(sol_file)) {
    std::ifstream f(sol_file);
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    CHECK_FALSE(content.empty());
    CHECK(content.find("scenario") != std::string::npos);
    CHECK(content.find("stage") != std::string::npos);
    CHECK(content.find("block") != std::string::npos);
  }

  std::filesystem::remove_all(tmpdir);
}

// -- penalty_class: hydro-flow unit conversion ---------------------------

// clang-format off

/// Same LP as `soft_le_uc_json` but with `penalty_class: "hydro_flow"`.
/// Authoring intent: `penalty` is $/m³ and should be converted to
/// $/(m³/s) per block via `× duration[h] × 3600`.  With duration=1h
/// and penalty=5, the slack column cost becomes 5 × 1 × 3600 = 18 000,
/// which is well above demand_fail_cost=1000 — so the LP should
/// *refuse* to relax the cap and fall back on demand failure, instead
/// of the raw-mode behavior that relaxes at a flat cost of 5.
static constexpr std::string_view soft_le_hydro_flow_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "soft_le_hydro_flow_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "soft_cap",
        "expression": "generator('g1').generation <= 50",
        "constraint_type": "raw",
        "penalty": 5,
        "penalty_class": "hydro_flow"
      }
    ]
  }
})json";

/// Invalid `penalty_class` string — the constructor must reject it
/// hard rather than silently falling back to `raw`.
static constexpr std::string_view soft_le_bad_class_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "soft_le_bad_class_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "soft_cap",
        "expression": "generator('g1').generation <= 50",
        "penalty": 5,
        "penalty_class": "not_a_real_class"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - penalty_class field round-trips through JSON")
{
  using namespace gtopt;

  auto planning = parse_planning_json(soft_le_hydro_flow_json);
  REQUIRE(planning.system.user_constraint_array.size() == 1);
  const auto& uc = planning.system.user_constraint_array[0];

  REQUIRE(uc.penalty_class.has_value());
  CHECK(uc.penalty_class.value_or("") == std::string {"hydro_flow"});
  CHECK(enum_from_name<PenaltyClass>(uc.penalty_class.value_or(""))
            .value_or(PenaltyClass::Raw)
        == PenaltyClass::HydroFlow);
}

TEST_CASE(
    "User constraint - missing penalty_class defaults to PenaltyClass::Raw")
{
  using namespace gtopt;

  // Legacy soft-LE fixture has no `penalty_class` field — the LP must
  // behave as if `penalty_class = "raw"` so pre-2026-04 inputs continue
  // to produce identical objectives.
  auto planning = parse_planning_json(soft_le_uc_json);
  REQUIRE(planning.system.user_constraint_array.size() == 1);
  CHECK_FALSE(
      planning.system.user_constraint_array[0].penalty_class.has_value());
}

TEST_CASE(
    "User constraint - penalty_class='hydro_flow' scales slack by dur*3600")
{
  using namespace gtopt;

  // Raw penalty_class: penalty=5 is used verbatim → slack cheap, LP
  // relaxes the 50 MW cap to serve all 90 MW demand via slack.
  auto planning_raw = parse_planning_json(soft_le_uc_json);
  PlanningLP raw_lp(std::move(planning_raw));
  auto raw_res = raw_lp.resolve();
  REQUIRE(raw_res.has_value());
  const auto raw_obj =
      raw_lp.systems().front().front().linear_interface().get_obj_value();

  // hydro_flow penalty_class: penalty=5 × duration(1h) × 3600 = 18 000
  // per unit, which is >> demand_fail_cost=1000.  The LP must now keep
  // the cap tight (slack≈0) and pay demand failure instead.
  auto planning_hf = parse_planning_json(soft_le_hydro_flow_json);
  PlanningLP hf_lp(std::move(planning_hf));
  auto hf_res = hf_lp.resolve();
  REQUIRE(hf_res.has_value());
  const auto hf_obj =
      hf_lp.systems().front().front().linear_interface().get_obj_value();

  // Raw: gen=90 @ $20 + slack=40 @ $5 = 1800 + 200 = 2000
  CHECK(raw_obj == doctest::Approx(2000.0));

  // hydro_flow: gen=50 @ $20 + dfail=40 @ $1000 = 1000 + 40000 = 41000
  CHECK(hf_obj == doctest::Approx(41000.0));

  // Sanity: the two modes MUST differ — otherwise penalty_class is a no-op.
  CHECK(hf_obj > raw_obj + 1000.0);
}

TEST_CASE("User constraint - unknown penalty_class string throws runtime_error")
{
  using namespace gtopt;

  // A typo in `penalty_class` must be a hard error at construction
  // time, not a silent fallback to `raw`.  The 2026-04 fail-fast audit
  // explicitly listed this pattern as the kind of silent behaviour
  // that must be eliminated.
  const auto build = [&]()
  {
    auto planning = parse_planning_json(soft_le_bad_class_json);
    PlanningLP planning_lp(std::move(planning));
    auto r = planning_lp.resolve();
    (void)r;
  };

  CHECK_THROWS_AS(build(), std::runtime_error);
}
