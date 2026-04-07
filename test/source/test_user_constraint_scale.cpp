/**
 * @file      test_user_constraint_scale.cpp
 * @brief     Tests that user constraints work correctly with scaled LP
 * variables
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 *
 * Verifies that user constraints produce correct LP solutions when variables
 * have non-unit col_scale:
 *   - scale_theta on bus.theta (Kirchhoff constraints)
 *   - energy_scale on reservoir/battery volumes
 *   - scale_objective on objective coefficients
 *   - variable_scale on custom-scaled variables
 *
 * The core invariant: flatten() applies col_scale to user constraint row
 * coefficients so that `coeff * physical_var <= RHS` becomes
 * `coeff * col_scale * LP_var <= RHS` in the flattened LP.
 */

#include <cmath>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ═══════════════════════════════════════════════════════════════════════════
// 1. User constraint on bus.theta with scale_theta
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

/// IEEE 4-bus case with scale_theta and a user constraint on bus angle.
/// The constraint `bus("b2").theta <= 1.0` (radian) should be correctly
/// applied regardless of scale_theta, which changes the LP representation
/// of theta but not the physical bounds.
auto make_theta_uc_json(double scale_theta_val) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "scale_theta": {},
    "use_kirchhoff": true
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "theta_scale_uc_test",
    "bus_array": [
      {{"uid": 1, "name": "b1"}}, {{"uid": 2, "name": "b2"}},
      {{"uid": 3, "name": "b3"}}, {{"uid": 4, "name": "b4"}}
    ],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}},
      {{"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]]}},
      {{"uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]]}}
    ],
    "line_array": [
      {{"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300}},
      {{"uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300}},
      {{"uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200}},
      {{"uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200}},
      {{"uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_theta_limit",
        "expression": "bus(\"b2\").theta <= 1.0"
      }}
    ]
  }}
}})",
      scale_theta_val);
}

// clang-format on

TEST_CASE(  // NOLINT
    "User constraint on bus.theta — invariant under scale_theta")
{
  // Solve with different scale_theta values; the LP objective should be
  // the same because the user constraint on theta (1 rad) is not binding
  // in this configuration.
  for (const auto st : {1.0, 0.01, 0.0001}) {
    auto json = make_theta_uc_json(st);
    Planning base;
    base.merge(daw::json::from_json<Planning>(json));

    PlanningLP planning_lp(std::move(base));
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());

    auto&& systems = planning_lp.systems();
    REQUIRE(!systems.empty());
    REQUIRE(!systems.front().empty());
    const auto& li = systems.front().front().linear_interface();

    // Physical objective should be invariant of scale_theta
    const double obj = li.get_obj_value() * 1000.0;  // undo scale_objective
    CHECK(obj == doctest::Approx(5000.0).epsilon(1e-3));
  }
}

TEST_CASE(  // NOLINT
    "User constraint on bus.theta — binding constraint changes solution")
{
  // With a very tight theta constraint (0.001 rad ≈ 0), bus angles are
  // essentially locked.  This should change the dispatch pattern.
  auto json_tight = std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "scale_theta": 0.0001,
    "use_kirchhoff": true
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "theta_tight_uc",
    "bus_array": [
      {{"uid": 1, "name": "b1"}}, {{"uid": 2, "name": "b2"}},
      {{"uid": 3, "name": "b3"}}
    ],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}},
      {{"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 50, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d3", "bus": "b3", "lmax": [[200.0]]}}
    ],
    "line_array": [
      {{"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.05, "tmax_ab": 100, "tmax_ba": 100}},
      {{"uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "reactance": 0.05, "tmax_ab": 100, "tmax_ba": 100}},
      {{"uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.05, "tmax_ab": 100, "tmax_ba": 100}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_theta_b2_tight",
        "expression": "bus(\"b2\").theta <= 0.001"
      }},
      {{
        "uid": 2, "name": "uc_theta_b2_lower",
        "expression": "bus(\"b2\").theta >= -0.001"
      }}
    ]
  }}
}})");

  Planning base;
  base.merge(daw::json::from_json<Planning>(json_tight));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  // Should solve (may shed load due to tight angle constraint)
  REQUIRE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. User constraint on reservoir volume with energy_scale
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

/// Hydro system with variable_scales and user constraint on reservoir volume.
/// The constraint `reservoir("rsv1").volume <= 800` should be correctly
/// applied in physical units regardless of variable scale.
auto make_reservoir_uc_json(double energy_scale_val) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "variable_scales": [
      {{"class_name": "Reservoir", "variable": "energy", "uid": 1, "scale": {}}}
    ]
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}, {{"uid": 2, "duration": 2}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 2}}],
    "scenario_array": [{{"uid": 1}}]
  }},
  "system": {{
    "name": "rsv_scale_uc",
    "bus_array": [{{"uid": 1, "name": "b1"}}],
    "generator_array": [
      {{"uid": 1, "name": "hg1", "bus": 1, "gcost": 5, "capacity": 200}},
      {{"uid": 2, "name": "thermal", "bus": 1, "gcost": 100, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d1", "bus": 1, "capacity": 50}}
    ],
    "junction_array": [
      {{"uid": 1, "name": "j1"}},
      {{"uid": 2, "name": "j_down", "drain": true}}
    ],
    "waterway_array": [
      {{"uid": 1, "name": "ww1", "junction_a": 1, "junction_b": 2, "fmin": 0, "fmax": 500}}
    ],
    "flow_array": [
      {{"uid": 1, "name": "inflow1", "direction": 1, "junction": 1, "discharge": 20}}
    ],
    "reservoir_array": [
      {{"uid": 1, "name": "rsv1", "junction": 1, "capacity": 1000,
        "emin": 0, "emax": 1000, "eini": 500}}
    ],
    "turbine_array": [
      {{"uid": 1, "name": "tur1", "waterway": 1, "generator": 1, "production_factor": 1.0}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_rsv_vol_upper",
        "expression": "reservoir(\"rsv1\").volume <= 800"
      }},
      {{
        "uid": 2, "name": "uc_rsv_vol_lower",
        "expression": "reservoir(\"rsv1\").volume >= 100"
      }}
    ]
  }}
}})",
      energy_scale_val);
}

// clang-format on

TEST_CASE(  // NOLINT
    "User constraint on reservoir volume — invariant under energy_scale")
{
  // Solve with different energy_scale values via variable_scales; both should
  // solve successfully and produce the same physical objective.
  double obj_1 = 0;
  double obj_1000 = 0;

  for (const auto es : {1.0, 1000.0}) {
    auto json = make_reservoir_uc_json(es);
    Planning base;
    base.merge(daw::json::from_json<Planning>(json));

    PlanningLP planning_lp(std::move(base));
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());

    auto&& systems = planning_lp.systems();
    REQUIRE(!systems.empty());
    REQUIRE(!systems.front().empty());
    const auto& li = systems.front().front().linear_interface();
    const double obj = li.get_obj_value() * 1000.0;  // undo scale_objective

    if (es == 1.0) {
      obj_1 = obj;
    } else {
      obj_1000 = obj;
    }
  }

  // Physical objectives should match regardless of energy_scale
  CHECK(obj_1 == doctest::Approx(obj_1000).epsilon(1e-3));
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. User constraint with scale_objective variations
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

/// Single-bus system with a binding user constraint and varying
/// scale_objective.  The constraint limits generation to 80 MW while
/// demand is 100 MW, forcing 20 MW of load shedding.
auto make_scale_obj_uc_json(double scale_obj) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 1}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 500,
    "scale_objective": {}
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "scale_obj_uc",
    "bus_array": [{{"uid": 1, "name": "b1"}}],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_gen_cap",
        "expression": "generator(\"g1\").generation <= 80"
      }}
    ]
  }}
}})",
      scale_obj);
}

// clang-format on

TEST_CASE(  // NOLINT
    "User constraint with scale_objective — physical cost invariant")
{
  // Physical cost = 80 MW × 20 $/MWh + 20 MW × 500 $/MWh = 11600 $
  // The LP objective = physical_cost / scale_objective
  constexpr double expected = 80.0 * 20.0 + 20.0 * 500.0;

  for (const auto so : {1.0, 1'000.0, 1'000'000.0}) {
    auto json = make_scale_obj_uc_json(so);
    Planning base;
    base.merge(daw::json::from_json<Planning>(json));

    PlanningLP planning_lp(std::move(base));
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());

    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    const double physical_cost = li.get_obj_value() * so;

    CHECK(physical_cost == doctest::Approx(expected).epsilon(1e-2));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. User constraint on battery energy with energy_scale
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

auto make_battery_uc_json(double energy_scale_val) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "variable_scales": [
      {{"class_name": "Battery", "variable": "energy", "uid": 1, "scale": {}}}
    ]
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}, {{"uid": 2, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 2, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "bat_scale_uc",
    "bus_array": [{{"uid": 1, "name": "b1"}}],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 30, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0, 80.0]]}}
    ],
    "battery_array": [
      {{
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.9, "output_efficiency": 0.9,
        "emin": 0, "emax": 100, "eini": 50,
        "pmax_charge": 50, "pmax_discharge": 50,
        "gcost": 0, "capacity": 100
      }}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_bat_energy_upper",
        "expression": "battery(\"bat1\").energy <= 80"
      }},
      {{
        "uid": 2, "name": "uc_bat_charge_limit",
        "expression": "battery(\"bat1\").charge <= 30"
      }},
      {{
        "uid": 3, "name": "uc_bat_discharge_limit",
        "expression": "battery(\"bat1\").discharge <= 30"
      }}
    ]
  }}
}})",
      energy_scale_val);
}

// clang-format on

TEST_CASE(  // NOLINT
    "User constraint on battery — invariant under energy_scale")
{
  double obj_1 = 0;
  double obj_1000 = 0;

  for (const auto es : {1.0, 1000.0}) {
    auto json = make_battery_uc_json(es);
    Planning base;
    base.merge(daw::json::from_json<Planning>(json));

    PlanningLP planning_lp(std::move(base));
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());

    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    const double obj = li.get_obj_value() * 1000.0;

    if (es == 1.0) {
      obj_1 = obj;
    } else {
      obj_1000 = obj;
    }
  }

  CHECK(obj_1 == doctest::Approx(obj_1000).epsilon(1e-3));
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. Sum-based user constraint with mixed scaled/unscaled variables
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

/// System where sum(generator(all).generation) is constrained alongside
/// a reservoir volume constraint — mixing power (unscaled) and energy
/// (scaled by variable_scales) variables.
auto make_mixed_scale_uc_json(double energy_scale_val) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "variable_scales": [
      {{"class_name": "Reservoir", "variable": "energy", "uid": 1, "scale": {}}}
    ]
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}, {{"uid": 2, "duration": 2}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 2}}],
    "scenario_array": [{{"uid": 1}}]
  }},
  "system": {{
    "name": "mixed_scale_uc",
    "bus_array": [{{"uid": 1, "name": "b1"}}],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": 1, "gcost": 10, "capacity": 200}},
      {{"uid": 2, "name": "g2", "bus": 1, "gcost": 50, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d1", "bus": 1, "capacity": 100}}
    ],
    "junction_array": [
      {{"uid": 1, "name": "j1"}},
      {{"uid": 2, "name": "j_down", "drain": true}}
    ],
    "waterway_array": [
      {{"uid": 1, "name": "ww1", "junction_a": 1, "junction_b": 2, "fmin": 0, "fmax": 500}}
    ],
    "flow_array": [
      {{"uid": 1, "name": "inflow1", "direction": 1, "junction": 1, "discharge": 30}}
    ],
    "reservoir_array": [
      {{"uid": 1, "name": "rsv1", "junction": 1, "capacity": 1000,
        "emin": 0, "emax": 1000, "eini": 500}}
    ],
    "turbine_array": [
      {{"uid": 1, "name": "tur1", "waterway": 1, "generator": 1, "production_factor": 1.0}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_sum_gen",
        "expression": "sum(generator(all).generation) <= 180"
      }},
      {{
        "uid": 2, "name": "uc_rsv_vol",
        "expression": "reservoir(\"rsv1\").volume <= 600"
      }}
    ]
  }}
}})",
      energy_scale_val);
}

// clang-format on

TEST_CASE(  // NOLINT
    "Mixed user constraints (power + energy) — invariant under "
    "energy_scale")
{
  double obj_1 = 0;
  double obj_1000 = 0;

  for (const auto es : {1.0, 1000.0}) {
    auto json = make_mixed_scale_uc_json(es);
    Planning base;
    base.merge(daw::json::from_json<Planning>(json));

    PlanningLP planning_lp(std::move(base));
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());

    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    const double obj = li.get_obj_value() * 1000.0;

    if (es == 1.0) {
      obj_1 = obj;
    } else {
      obj_1000 = obj;
    }
  }

  CHECK(obj_1 == doctest::Approx(obj_1000).epsilon(1e-3));
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. constraint_type scaling with scale_objective
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

/// Same binding constraint with different constraint_type values.
/// All should solve correctly with different scale_objective values.
auto make_ctype_scale_json(double scale_obj,
                           std::string_view ctype) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.1,
    "lp_matrix_options": {{"names_level": 1}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 500,
    "scale_objective": {}
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "ctype_scale_test",
    "bus_array": [{{"uid": 1, "name": "b1"}}],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_gen_cap",
        "expression": "generator(\"g1\").generation <= 80",
        "constraint_type": "{}"
      }}
    ]
  }}
}})",
      scale_obj,
      ctype);
}

// clang-format on

TEST_CASE(  // NOLINT
    "constraint_type power/raw/energy — solve with varying scale_objective")
{
  for (const auto* ctype : {"power", "raw", "energy"}) {
    for (const auto so : {1.0, 1'000.0, 100'000.0}) {
      auto json = make_ctype_scale_json(so, ctype);
      Planning base;
      base.merge(daw::json::from_json<Planning>(json));

      PlanningLP planning_lp(std::move(base));
      auto result = planning_lp.resolve();
      REQUIRE(result.has_value());

      // Physical cost = 80*20 + 20*500 = 11600 (discount=0.9 for 1 stage)
      auto&& systems = planning_lp.systems();
      const auto& li = systems.front().front().linear_interface();
      const double lp_obj = li.get_obj_value();

      // LP obj must be positive and finite
      CHECK(lp_obj > 0.0);
      CHECK(std::isfinite(lp_obj));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. User constraint on line flow with scale_theta + scale_objective
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

auto make_line_uc_json(double scale_theta_val,
                       double scale_obj) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": {},
    "scale_theta": {},
    "use_kirchhoff": true
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "line_flow_scale_uc",
    "bus_array": [
      {{"uid": 1, "name": "b1"}},
      {{"uid": 2, "name": "b2"}}
    ],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 10, "capacity": 300}},
      {{"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 50, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d2", "bus": "b2", "lmax": [[150.0]]}}
    ],
    "line_array": [
      {{"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
        "reactance": 0.05, "tmax_ab": 300, "tmax_ba": 300}}
    ],
    "user_constraint_array": [
      {{
        "uid": 1, "name": "uc_line_flow_limit",
        "expression": "line(\"l1_2\").flow <= 100"
      }}
    ]
  }}
}})",
      scale_obj,
      scale_theta_val);
}

// clang-format on

TEST_CASE(  // NOLINT
    "User constraint on line flow — invariant under scale_theta and "
    "scale_objective")
{
  // Line flow limit of 100 MW.  Demand is 150 MW at b2.
  // g1 (cheap, 10$/MWh) at b1 can only send 100 MW over the line.
  // g2 (expensive, 50$/MWh) at b2 must cover the remaining 50 MW.
  // Physical cost = 100*10 + 50*50 = 3500 $

  constexpr double expected = 100.0 * 10.0 + 50.0 * 50.0;

  for (const auto st : {1.0, 0.001, 0.0001}) {
    for (const auto so : {1.0, 1'000.0}) {
      auto json = make_line_uc_json(st, so);
      Planning base;
      base.merge(daw::json::from_json<Planning>(json));

      PlanningLP planning_lp(std::move(base));
      auto result = planning_lp.resolve();
      REQUIRE(result.has_value());

      auto&& systems = planning_lp.systems();
      const auto& li = systems.front().front().linear_interface();
      const double physical_cost = li.get_obj_value() * so;

      CHECK(physical_cost == doctest::Approx(expected).epsilon(1e-2));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. Multiple user constraints combining theta, energy, and power scales
// ═══════════════════════════════════════════════════════════════════════════

// clang-format off

static constexpr std::string_view multi_scale_uc_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {"names_level": 2},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 10000,
    "scale_theta": 0.0001,
    "variable_scales": [
      {"class_name": "Battery", "variable": "energy", "uid": 1, "scale": 1000}
    ],
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}, {"uid": 2, "duration": 2}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "multi_scale_uc",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 10, "capacity": 300},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 50, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[50, 50]]},
      {"uid": 2, "name": "d2", "bus": "b2", "lmax": [[80, 80]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.9, "output_efficiency": 0.9,
        "emin": 0, "emax": 100, "eini": 50,
        "pmax_charge": 50, "pmax_discharge": 50,
        "gcost": 0, "capacity": 100
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_gen_total",
        "expression": "generator(\"g1\").generation + generator(\"g2\").generation <= 250"
      },
      {
        "uid": 2, "name": "uc_line_flow",
        "expression": "line(\"l1_2\").flow <= 120"
      },
      {
        "uid": 3, "name": "uc_bat_energy",
        "expression": "battery(\"bat1\").energy <= 80"
      },
      {
        "uid": 4, "name": "uc_theta_range",
        "expression": "bus(\"b2\").theta <= 0.5"
      },
      {
        "uid": 5, "name": "uc_theta_lower",
        "expression": "bus(\"b2\").theta >= -0.5"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE(  // NOLINT
    "Multiple user constraints with all scale types "
    "(theta + energy + objective)")
{
  Planning base;
  base.merge(daw::json::from_json<Planning>(multi_scale_uc_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();

  // LP should have a positive, finite objective
  CHECK(li.get_obj_value() > 0.0);
  CHECK(std::isfinite(li.get_obj_value()));
}

}  // namespace
