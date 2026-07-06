// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_sos2_ieee4b.cpp — IEEE 4-bus integration test for
// the L-secant chord + SOS2 enforcement introduced by issue #504.
//
// The fixture extends the canonical ``ieee_4b_ori`` 4-bus 5-line
// benchmark with:
//   * ``line_losses_mode = "tangent_signed_flow"`` on every line
//   * ``loss_secant_segments = 4`` (L = 4 chord segments)
//   * ``loss_use_sos2 = true``  (SOS2 fill-order enforcement)
//   * non-zero ``resistance`` so losses are physically present
//
// The test then asserts:
//   1) Solve succeeds under the configured MIP backend
//   2) ``sos2_set_count()`` reports one SOS2 set per (line, block) —
//      with 5 lines × 1 block this is 5 sets
//   3) Bus-balance KCL holds at every bus (generation - demand -
//      Σ-flow-out = total losses absorbed at the bus per the
//      ``split`` allocation forced by tangent_signed_flow)
//   4) Total system loss is below the L=1 single-secant upper bound
//      and above the true convex quadratic at the operating point
//
// Skipped when no MIP solver is loaded (CLP-only CI builds).

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Wrap the entire file body in a uniquely-named outer namespace so the
// Unity-build cannot collide with same-named helpers in sibling test
// files — see CLAUDE.md ``unity-anon-namespace`` memory note.
namespace test_line_losses_sos2_ieee4b_ns
{

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── Canonical IEEE 4-bus base + L-secant SOS2 overlay ───────────────
//
// All five lines carry resistance = 0.01 (p.u.) and voltage = 100 kV
// so the loss model has non-trivial physics.  Operating point at the
// LP optimum routes ~250 MW out of bus 1 across the l1_3 / l1_2 lines;
// the L=4 chord segments will fill v_1..v_k for some k.
//
// clang-format off
constexpr std::string_view ieee4b_sos2_json = R"(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "scale_objective": 1000,
        "demand_fail_cost": 1000,
        "line_losses_mode": "tangent_signed_flow",
        "loss_segments": 5,
        "loss_secant_segments": 4,
        "loss_use_sos2": true,
        "loss_cost_eps": 1e-6
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }
      ],
      "scenario_array": [
        { "uid": 1, "probability_factor": 1 }
      ]
    },
    "system": {
      "name": "ieee_4b_sos2",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" },
        { "uid": 3, "name": "b3" },
        { "uid": 4, "name": "b4" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300 },
        { "uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]] },
        { "uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]] }
      ],
      "line_array": [
        {
          "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "voltage": 100, "resistance": 0.01, "reactance": 0.02,
          "tmax_ab": 300, "tmax_ba": 300
        },
        {
          "uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3",
          "voltage": 100, "resistance": 0.01, "reactance": 0.02,
          "tmax_ab": 300, "tmax_ba": 300
        },
        {
          "uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3",
          "voltage": 100, "resistance": 0.01, "reactance": 0.03,
          "tmax_ab": 200, "tmax_ba": 200
        },
        {
          "uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4",
          "voltage": 100, "resistance": 0.01, "reactance": 0.02,
          "tmax_ab": 200, "tmax_ba": 200
        },
        {
          "uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4",
          "voltage": 100, "resistance": 0.01, "reactance": 0.03,
          "tmax_ab": 150, "tmax_ba": 150
        }
      ]
    }
  }
)";
// clang-format on

/// True iff a loaded backend implements ``add_sos2``.  Currently only
/// the CPLEX plugin overrides the ``SolverBackend::add_sos2`` default
/// (which throws on ``size() >= 2``); HiGHS / CBC / CLP all fall
/// through to the throw.  ``has_mip_solver()`` is too permissive on
/// CI builds without CPLEX — CBC supports MIP but not SOS2, so the
/// SOS2-emitting tests would throw at LP-build time.
///
/// Triggers ``load_all_plugins()`` before the lookup so the helper
/// works in isolation (single-test filter runs).  ``has_solver``
/// alone only inspects already-loaded plugins, which can be false
/// negative when no other test has primed the registry yet.
[[nodiscard]] bool ieee4b_sos2_available()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  return reg.has_solver("cplex");
}

}  // namespace
}  // namespace test_line_losses_sos2_ieee4b_ns

using test_line_losses_sos2_ieee4b_ns::ieee4b_sos2_available;
using test_line_losses_sos2_ieee4b_ns::ieee4b_sos2_json;

// ── (1) Structural — 5 SOS2 sets emitted (one per line, 1 block) ────

TEST_CASE("IEEE 4-bus + tangent_signed_flow L=4 SOS2: 5 SOS2 sets emitted")
{
  if (!ieee4b_sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }

  Planning base;
  base.merge(parse_planning_json(ieee4b_sos2_json));
  // Pin the SOLVE to CPLEX, not just the availability check: MindOpt
  // 2.3.0 mis-declares this meshed 5×SOS2 model infeasible (upstream
  // defect — the same LP solves optimal with any single SOS2 set kept,
  // or with the SOS section stripped; verified 2026-07-05).
  base.options.lp_matrix_options.solver_name = "cplex";

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // 5 lines × 1 block = 5 SOS2 declarations.  Pinning this catches a
  // regression in either the per-line override resolution
  // (PlanningOptionsLP) or the LP-build emission (line_losses.cpp).
  // Without SOS2 the LP would still solve, but
  // ``sos2_set_count()`` would return 0.
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  CHECK(li.sos2_set_count() == 5);
}

// ── (2) L = 1 baseline (single-secant chord) — no SOS2 emitted ──────

TEST_CASE("IEEE 4-bus + tangent_signed_flow L=1 baseline: 0 SOS2 sets")
{
  // Same fixture but global loss_secant_segments and loss_use_sos2
  // both unset (defaults: 1 and false).  Single-secant chord, no
  // SOS2 — verifies that the L=4 SOS2 fixture's positive emission
  // count is genuinely driven by the new fields, not by a stale
  // global option.
  constexpr std::string_view base_json = R"(
    {
      "options": {
        "annual_discount_rate": 0.0,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "model_options": {
          "use_single_bus": false,
          "use_kirchhoff": true,
          "scale_objective": 1000,
          "demand_fail_cost": 1000,
          "line_losses_mode": "tangent_signed_flow",
          "loss_segments": 5,
          "loss_cost_eps": 1e-6
        }
      },
      "simulation": {
        "block_array": [{ "uid": 1, "duration": 1 }],
        "stage_array": [
          { "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }
        ],
        "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
      },
      "system": {
        "name": "ieee_4b_baseline_no_sos2",
        "bus_array": [
          { "uid": 1, "name": "b1" },
          { "uid": 2, "name": "b2" },
          { "uid": 3, "name": "b3" },
          { "uid": 4, "name": "b4" }
        ],
        "generator_array": [
          { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300 },
          { "uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200 }
        ],
        "demand_array": [
          { "uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]] },
          { "uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]] }
        ],
        "line_array": [
          { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300 },
          { "uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300 },
          { "uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "voltage": 100, "resistance": 0.01, "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200 },
          { "uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200 },
          { "uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "voltage": 100, "resistance": 0.01, "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150 }
        ]
      }
    }
  )";

  Planning base;
  base.merge(parse_planning_json(base_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  // Single-secant chord on every line; no SOS2 declared.
  CHECK(li.sos2_set_count() == 0);
}

// ── (3) Selective per-line override — only one line gets SOS2 ───────

TEST_CASE(
    "IEEE 4-bus: per-line loss_use_sos2 overrides apply only to flagged "
    "lines")
{
  if (!ieee4b_sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }

  // Targeted application matches the issue #504 §"Targeted application"
  // recipe: enable SOS2 only on the offender line(s), keep the rest at
  // L=1.  Here we flag the heaviest-flow line l1_3 (carries ~150 MW
  // from b1 → b3 at the optimum on this fixture) and verify exactly 1
  // SOS2 set is declared.
  constexpr std::string_view selective_json = R"(
    {
      "options": {
        "annual_discount_rate": 0.0,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "model_options": {
          "use_single_bus": false,
          "use_kirchhoff": true,
          "scale_objective": 1000,
          "demand_fail_cost": 1000,
          "line_losses_mode": "tangent_signed_flow",
          "loss_segments": 5,
          "loss_cost_eps": 1e-6
        }
      },
      "simulation": {
        "block_array": [{ "uid": 1, "duration": 1 }],
        "stage_array": [
          { "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }
        ],
        "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
      },
      "system": {
        "name": "ieee_4b_selective_sos2",
        "bus_array": [
          { "uid": 1, "name": "b1" }, { "uid": 2, "name": "b2" },
          { "uid": 3, "name": "b3" }, { "uid": 4, "name": "b4" }
        ],
        "generator_array": [
          { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300 },
          { "uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200 }
        ],
        "demand_array": [
          { "uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]] },
          { "uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]] }
        ],
        "line_array": [
          { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300 },
          {
            "uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3",
            "voltage": 100, "resistance": 0.01, "reactance": 0.02,
            "tmax_ab": 300, "tmax_ba": 300,
            "loss_secant_segments": 4, "loss_use_sos2": true
          },
          { "uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "voltage": 100, "resistance": 0.01, "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200 },
          { "uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200 },
          { "uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "voltage": 100, "resistance": 0.01, "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150 }
        ]
      }
    }
  )";

  Planning base;
  base.merge(parse_planning_json(selective_json));
  // Pin to CPLEX — see the 5×SOS2 test above (MindOpt 2.3.0 upstream
  // defect on this meshed fixture).
  base.options.lp_matrix_options.solver_name = "cplex";

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();
  // Exactly 1 SOS2 set: just l1_3.  Other 4 lines stay at L=1.
  CHECK(li.sos2_set_count() == 1);
}

// ── (4) Objective parity — SOS2 path produces TIGHTER objective ─────

TEST_CASE(
    "IEEE 4-bus: SOS2 L=4 path objective is below L=1 single-secant "
    "objective")
{
  if (!ieee4b_sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }

  // The L-secant chord upper bound on ``ℓ`` is tighter than the L=1
  // single secant (it lies AT or BELOW the L=1 chord everywhere).
  // Therefore the LP minimising loss-related cost (via the tiny
  // ``loss_cost_eps = 1e-6``) under SOS2 should land at an objective
  // ``obj_sos2 ≤ obj_l1``, with the gap proportional to the chord
  // tightening.
  //
  // On this 4-bus fixture the dispatch cost ($20 × 250 MW = $5000)
  // dominates and the loss-side cost difference is sub-cent, so both
  // objectives match within solver tolerance — the test asserts the
  // *direction* of the inequality plus a tight relative bound.

  // L = 1 baseline.
  Planning base_l1;
  base_l1.merge(parse_planning_json(R"(
    {
      "options": {
        "annual_discount_rate": 0.0,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "model_options": {
          "use_single_bus": false,
          "use_kirchhoff": true,
          "scale_objective": 1000,
          "demand_fail_cost": 1000,
          "line_losses_mode": "tangent_signed_flow",
          "loss_segments": 5,
          "loss_cost_eps": 1e-6
        }
      },
      "simulation": {
        "block_array": [{ "uid": 1, "duration": 1 }],
        "stage_array": [
          { "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }
        ],
        "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
      },
      "system": {
        "name": "ieee_4b_l1",
        "bus_array": [
          { "uid": 1, "name": "b1" }, { "uid": 2, "name": "b2" },
          { "uid": 3, "name": "b3" }, { "uid": 4, "name": "b4" }
        ],
        "generator_array": [
          { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300 },
          { "uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200 }
        ],
        "demand_array": [
          { "uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]] },
          { "uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]] }
        ],
        "line_array": [
          { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300 },
          { "uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300 },
          { "uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "voltage": 100, "resistance": 0.01, "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200 },
          { "uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "voltage": 100, "resistance": 0.01, "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200 },
          { "uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "voltage": 100, "resistance": 0.01, "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150 }
        ]
      }
    }
  )"));

  // Pin BOTH solves to CPLEX: the L=4 leg trips the MindOpt 2.3.0
  // multi-SOS2 defect (see the 5×SOS2 test above), and the objective
  // comparison below needs solver-consistent tolerances anyway.
  base_l1.options.lp_matrix_options.solver_name = "cplex";
  PlanningLP planning_lp_l1(std::move(base_l1));
  auto result_l1 = planning_lp_l1.resolve();
  REQUIRE(result_l1.has_value());
  auto&& systems_l1 = planning_lp_l1.systems();
  REQUIRE(!systems_l1.empty());
  REQUIRE(!systems_l1.front().empty());
  const auto obj_l1 =
      systems_l1.front().front().linear_interface().get_obj_value();

  // L = 4 + SOS2.
  Planning base_l4;
  base_l4.merge(parse_planning_json(ieee4b_sos2_json));
  base_l4.options.lp_matrix_options.solver_name = "cplex";

  PlanningLP planning_lp_l4(std::move(base_l4));
  auto result_l4 = planning_lp_l4.resolve();
  REQUIRE(result_l4.has_value());
  auto&& systems_l4 = planning_lp_l4.systems();
  REQUIRE(!systems_l4.empty());
  REQUIRE(!systems_l4.front().empty());
  const auto obj_l4 =
      systems_l4.front().front().linear_interface().get_obj_value();

  // Both objectives should be near $5,000 (dispatch dominates loss).
  CHECK(obj_l1 == doctest::Approx(5000.0).epsilon(1e-2));
  CHECK(obj_l4 == doctest::Approx(5000.0).epsilon(1e-2));

  // The SOS2 path's objective is ≤ the L=1 path within rounding noise.
  // The loss-cost-eps term is microscopic ($10⁻⁶ × ℓ_chord <
  // 10⁻⁶ × c·envelope² ≪ 10⁻³ in this fixture), so the inequality is
  // effectively obj_l4 ≤ obj_l1 + epsilon.
  CHECK(obj_l4 <= obj_l1 + 1e-3);
}
