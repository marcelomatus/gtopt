// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_commitment_kvl_bigm.cpp — pin the v0.5 Kirchhoff KVL
// big-M disjunction added to ``LineCommitmentLP`` for issue #509.
//
// When ``use_kirchhoff = true`` (``node_angle`` mode) and a
// LineCommitment is present, ``LineCommitmentLP`` rewrites the
// equality KVL row stamped by LineLP
//
//     -θ_a + θ_b + x_τ · f = -φ_rad
//
// into the disjunctive pair
//
//     -θ_a + θ_b + x_τ · f + M·u_l  ≤  M − φ_rad
//     -θ_a + θ_b + x_τ · f − M·u_l  ≥ −M − φ_rad
//
// where ``M = 2·θ_max + |φ_rad|`` (Fisher 2008 baseline).  At u = 1
// both rows collapse to the equality; at u = 0 the two θ angles
// decouple — exactly the physics of an open breaker.
//
// Tests pin:
//   1) Structural: a 2-row big-M pair is emitted per (line, block)
//      replacing the single equality row.
//   2) IEEE 9-bus + OTS on one line: solve succeeds, objective
//      matches the no-OTS baseline (optimal u_l = 1).
//   3) IEEE 9-bus + must_run: identical objective.

#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;

namespace test_line_commitment_kvl_bigm_ns
{

namespace
{

// ── 2-bus Kirchhoff fixture (line w/ reactance) ─────────────────────

[[nodiscard]] std::string make_2bus_kirchhoff_json(bool with_line_commitment,
                                                   bool relax = true)
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "kirchhoff_mode": "node_angle",
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [{ "uid": 1, "duration": 1 }],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_kvl_2bus",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2", "lmax": [[100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200 }
      ])";
  if (with_line_commitment) {
    out += R"(,
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": )";
    out += (relax ? "true" : "false");
    out += R"( }
      ])";
  }
  out += R"(
    }
  })";
  return out;
}

[[nodiscard]] bool tkbm_mip_available()
{
  auto& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    return false;
  }
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] int tkbm_count_rows_containing(const LinearInterface& li,
                                             std::string_view substr)
{
  int count = 0;
  for (const auto& [name, _idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

}  // namespace
}  // namespace test_line_commitment_kvl_bigm_ns

using test_line_commitment_kvl_bigm_ns::make_2bus_kirchhoff_json;
using test_line_commitment_kvl_bigm_ns::tkbm_count_rows_containing;
using test_line_commitment_kvl_bigm_ns::tkbm_mip_available;

// ── (1) Structural — big-M pair replaces the single equality ────────

TEST_CASE(
    "LineCommitmentLP KVL big-M: 1 mutated equality + 1 kvl_minus row "
    "per (line, block)")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // Baseline: no LineCommitment — one ``line_theta_`` equality row
  // per block.
  {
    Planning planning;
    planning.merge(
        parse_planning_json(make_2bus_kirchhoff_json(/*with_lc=*/false)));
    LpMatrixOptions flat_opts;
    flat_opts.row_with_names = true;
    flat_opts.row_with_name_map = true;
    PlanningLP planning_lp(std::move(planning), flat_opts);
    REQUIRE(planning_lp.resolve().has_value());
    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    // One KVL equality row per (line, block).
    CHECK(tkbm_count_rows_containing(li, "line_theta_") == 1);
    // No ``kvl_minus`` row.
    CHECK(tkbm_count_rows_containing(li, "linecommitment_kvl_minus") == 0);
  }

  // OTS on: the existing ``line_theta_`` row is mutated in place to
  // become the upper-side ``≤`` half (still 1 row), and a new
  // ``linecommitment_kvl_minus`` row is added for the lower-side
  // ``≥`` half.
  {
    Planning planning;
    planning.merge(
        parse_planning_json(make_2bus_kirchhoff_json(/*with_lc=*/true)));
    LpMatrixOptions flat_opts;
    flat_opts.row_with_names = true;
    flat_opts.row_with_name_map = true;
    PlanningLP planning_lp(std::move(planning), flat_opts);
    REQUIRE(planning_lp.resolve().has_value());
    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    CHECK(tkbm_count_rows_containing(li, "line_theta_") == 1);
    CHECK(tkbm_count_rows_containing(li, "linecommitment_kvl_minus") == 1);
  }
}

// ── (2) Solve — Kirchhoff + LP-relax OTS matches the no-OTS baseline ─

TEST_CASE(
    "LineCommitmentLP KVL big-M: relaxed solve picks u_l = 1, matches "
    "no-OTS objective")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning base;
  base.merge(parse_planning_json(make_2bus_kirchhoff_json(/*with_lc=*/false)));
  PlanningLP planning_baseline(std::move(base));
  REQUIRE(planning_baseline.resolve().has_value());
  const auto obj_baseline = planning_baseline.systems()
                                .front()
                                .front()
                                .linear_interface()
                                .get_obj_value();

  Planning ots;
  ots.merge(parse_planning_json(
      make_2bus_kirchhoff_json(/*with_lc=*/true, /*relax=*/true)));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  const auto obj_ots =
      planning_ots.systems().front().front().linear_interface().get_obj_value();

  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-6));
  // Sanity: g1 × 100 MW × $10/MWh = $1000.
  CHECK(obj_baseline == doctest::Approx(1000.0).epsilon(1e-3));
}

// ── (3) IEEE 9-bus + OTS on l4_6 ────────────────────────────────────
//
// The IEEE 9-bus canonical OPF case is the standard testbed for DC
// Kirchhoff OPF.  We attach a LineCommitment to a single line
// (``l4_6`` — a high-reactance loop-completing branch) and verify
// the LP-relax optimum picks u_l = 1 (opening the line would force
// flow redistribution but the case is feasible without it; with the
// loss-free / cost-free baseline the obj equals the no-OTS solve).

[[nodiscard]] std::string make_ieee9b_with_ots_json(bool with_lc,
                                                    bool must_run = false)
{
  std::string base = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "kirchhoff_mode": "node_angle",
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [{ "uid": 1, "duration": 1 }],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_ieee9b",
      "bus_array": [
        { "uid": 1, "name": "b1" }, { "uid": 2, "name": "b2" },
        { "uid": 3, "name": "b3" }, { "uid": 4, "name": "b4" },
        { "uid": 5, "name": "b5" }, { "uid": 6, "name": "b6" },
        { "uid": 7, "name": "b7" }, { "uid": 8, "name": "b8" },
        { "uid": 9, "name": "b9" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250 },
        { "uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300 },
        { "uid": 3, "name": "g3", "bus": "b3", "pmin": 0,  "pmax": 270, "gcost": 30, "capacity": 270 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]] },
        { "uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]] },
        { "uid": 3, "name": "d3", "bus": "b9", "lmax": [[90.0]]  }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576, "tmax_ab": 250, "tmax_ba": 250 },
        { "uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625, "tmax_ab": 300, "tmax_ba": 300 },
        { "uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586, "tmax_ab": 270, "tmax_ba": 270 },
        { "uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,  "tmax_ab": 250, "tmax_ba": 250 },
        { "uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,  "tmax_ab": 250, "tmax_ba": 250 },
        { "uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,  "tmax_ab": 250, "tmax_ba": 250 },
        { "uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,   "tmax_ab": 250, "tmax_ba": 250 },
        { "uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,  "tmax_ab": 250, "tmax_ba": 250 },
        { "uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008, "tmax_ab": 250, "tmax_ba": 250 }
      ])";
  if (with_lc) {
    base += R"(,
      "line_commitment_array": [
        { "uid": 1, "name": "l4_6_ots", "line": "l4_6", "relax": true)";
    if (must_run) {
      base += R"(, "must_run": true)";
    }
    base += R"( }
      ])";
  }
  base += R"(
    }
  })";
  return base;
}

TEST_CASE(
    "LineCommitmentLP KVL big-M: IEEE 9b + LP-relax OTS on l4_6 matches "
    "no-OTS objective")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning base;
  base.merge(parse_planning_json(make_ieee9b_with_ots_json(/*with_lc=*/false)));
  PlanningLP planning_baseline(std::move(base));
  REQUIRE(planning_baseline.resolve().has_value());
  const auto obj_baseline = planning_baseline.systems()
                                .front()
                                .front()
                                .linear_interface()
                                .get_obj_value();

  Planning ots;
  ots.merge(parse_planning_json(make_ieee9b_with_ots_json(/*with_lc=*/true)));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  const auto obj_ots =
      planning_ots.systems().front().front().linear_interface().get_obj_value();

  // Objective parity within solver tolerance — the LP-relax of the
  // OTS MIP must reproduce the no-OTS LP since opening any single
  // line in IEEE 9b is feasible BUT incurs the same dispatch cost
  // (loss-free model, no congestion).
  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-3));
}

TEST_CASE("LineCommitmentLP KVL big-M: IEEE 9b + must_run reproduces baseline")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning base;
  base.merge(parse_planning_json(make_ieee9b_with_ots_json(/*with_lc=*/false)));
  PlanningLP planning_baseline(std::move(base));
  REQUIRE(planning_baseline.resolve().has_value());
  const auto obj_baseline = planning_baseline.systems()
                                .front()
                                .front()
                                .linear_interface()
                                .get_obj_value();

  Planning ots;
  ots.merge(parse_planning_json(
      make_ieee9b_with_ots_json(/*with_lc=*/true, /*must_run=*/true)));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  const auto obj_ots =
      planning_ots.systems().front().front().linear_interface().get_obj_value();

  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-3));
}

// ── (5) cycle_basis disjunctive form (v1) ───────────────────────────
//
// In ``kirchhoff_mode = cycle_basis`` the cycle KVL equality
// ``Σ sign_e · (x_τ·f_e + φ_e)·row_scale = 0`` is replaced by two
// inequalities whenever at least one line on the cycle is switchable.
// Per-cycle big-M ``M_C = 2θ_max · |C| · row_scale + Σ |φ|·row_scale``.
// At ``u_l = 1`` both halves collapse to the original equality.
//
// 3-bus triangle fixture: lines l1_2, l2_3, l3_1 form one fundamental
// cycle.  Generator at b1, demand at b2.  Without OTS the LP picks
// an Δθ pattern that pushes some flow around the triangle.

[[nodiscard]] std::string make_3bus_triangle_cycle_json(bool with_lc,
                                                        bool relax = true)
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "kirchhoff_mode": "cycle_basis",
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [{ "uid": 1, "duration": 1 }],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_cycle_triangle",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" },
        { "uid": 3, "name": "b3" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2", "lmax": [[100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200 },
        { "uid": 2, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200 },
        { "uid": 3, "name": "l3_1", "bus_a": "b3", "bus_b": "b1", "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200 }
      ])";
  if (with_lc) {
    out += R"(,
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": )";
    out += (relax ? "true" : "false");
    out += R"( }
      ])";
  }
  out += R"(
    }
  })";
  return out;
}

TEST_CASE(
    "LineCommitmentLP cycle_basis: kvl_minus row added when switchable line "
    "is on the cycle (v1)")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // Baseline: no LineCommitment — one ``cycle`` equality row per block.
  {
    Planning planning;
    planning.merge(
        parse_planning_json(make_3bus_triangle_cycle_json(/*with_lc=*/false)));
    LpMatrixOptions flat_opts;
    flat_opts.row_with_names = true;
    flat_opts.row_with_name_map = true;
    PlanningLP planning_lp(std::move(planning), flat_opts);
    REQUIRE(planning_lp.resolve().has_value());
    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    // One cycle × one block = 1 equality row tagged ``kirchhoff_cycle_``.
    CHECK(tkbm_count_rows_containing(li, "kirchhoff_cycle_") == 1);
    // No ``kvl_minus`` row in baseline.
    CHECK(tkbm_count_rows_containing(li, "kirchhoff_kvl_minus") == 0);
  }

  // OTS on l1_2: the cycle has one switchable line, so the equality is
  // replaced by 1 upper-side ``≤`` + 1 lower-side ``≥`` row.
  {
    Planning planning;
    planning.merge(
        parse_planning_json(make_3bus_triangle_cycle_json(/*with_lc=*/true)));
    LpMatrixOptions flat_opts;
    flat_opts.row_with_names = true;
    flat_opts.row_with_name_map = true;
    PlanningLP planning_lp(std::move(planning), flat_opts);
    REQUIRE(planning_lp.resolve().has_value());
    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();
    // Upper-side half reuses the ``cycle`` label.
    CHECK(tkbm_count_rows_containing(li, "kirchhoff_cycle_") == 1);
    // Lower-side half tagged ``kvl_minus``.
    CHECK(tkbm_count_rows_containing(li, "kirchhoff_kvl_minus") == 1);
  }
}

TEST_CASE(
    "LineCommitmentLP cycle_basis: relaxed solve picks u_l = 1, objective "
    "matches no-OTS baseline (v1)")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning base;
  base.merge(
      parse_planning_json(make_3bus_triangle_cycle_json(/*with_lc=*/false)));
  PlanningLP planning_baseline(std::move(base));
  REQUIRE(planning_baseline.resolve().has_value());
  const auto obj_baseline = planning_baseline.systems()
                                .front()
                                .front()
                                .linear_interface()
                                .get_obj_value();

  Planning ots;
  ots.merge(parse_planning_json(
      make_3bus_triangle_cycle_json(/*with_lc=*/true, /*relax=*/true)));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  const auto obj_ots =
      planning_ots.systems().front().front().linear_interface().get_obj_value();

  // At u_l = 1, both KVL halves collapse to equality; objective must
  // match no-OTS baseline.
  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-6));
  // Sanity: g1 × 100 MW × $10/MWh = $1000.
  CHECK(obj_baseline == doctest::Approx(1000.0).epsilon(1e-3));
}

// ── (6) Per-line kvl_big_m override ─────────────────────────────────
//
// ``LineCommitment.kvl_big_m`` lets the user (or a future
// iterative-tightening pre-solve à la Pineda 2024) inject a tighter
// per-line big-M than the default ``2·θ_max + |φ|``.  The override
// flows through to the upper-side ``≤`` row's RHS exactly: row.uppb
// becomes ``kvl_big_m + rhs_eq`` (where ``rhs_eq = -φ ≈ 0`` for a
// line with no phase shift).
//
// This test pins the override → RHS relationship in node_angle mode.
// cycle_basis ignores the per-line value (cycle big-M is a sum-of-edges
// bound), so this test is node_angle-only.

[[nodiscard]] std::string make_2bus_kirchhoff_bigm_json(double kvl_big_m)
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "kirchhoff_mode": "node_angle",
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [{ "uid": 1, "duration": 1 }],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 1,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_bigm_2bus",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2", "lmax": [[100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200 }
      ],
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": true,
          "kvl_big_m": )";
  out += std::to_string(kvl_big_m);
  out += R"( }
      ]
    }
  })";
  return out;
}

TEST_CASE(
    "LineCommitmentLP node_angle: per-line kvl_big_m override changes the "
    "upper-side row's uppb (v1)")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // The 2-bus line has no phase shift (φ = 0), so rhs_eq = -φ = 0
  // and the upper-side row's uppb collapses to ``kvl_big_m`` exactly.
  //
  // Use two distinct override values that are both well below the
  // default ``2·θ_max + |φ|`` (~ 2·100 · 0.05 · 2 = 20 for this fixture,
  // give or take row_scale).  The difference between the two solves'
  // uppbs must equal the difference between the override values.
  constexpr double M1 = 5.0;
  constexpr double M2 = 17.0;

  auto upper_row_uppb = [](const LinearInterface& li) -> double
  {
    // Find the ``line_theta_`` row (upper-side ``≤`` half of the
    // big-M disjunctive pair) and read its uppb.
    const auto& names = li.row_name_map();
    for (const auto& [name, idx] : names) {
      if (name.contains("line_theta_")) {
        const auto upps = li.get_row_upp();
        return upps[value_of(idx)];
      }
    }
    return 0.0;
  };

  LpMatrixOptions flat_opts;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;

  double uppb1 = 0.0;
  {
    Planning planning;
    planning.merge(parse_planning_json(make_2bus_kirchhoff_bigm_json(M1)));
    PlanningLP planning_lp(std::move(planning), flat_opts);
    REQUIRE(planning_lp.resolve().has_value());
    uppb1 = upper_row_uppb(
        planning_lp.systems().front().front().linear_interface());
  }

  double uppb2 = 0.0;
  {
    Planning planning;
    planning.merge(parse_planning_json(make_2bus_kirchhoff_bigm_json(M2)));
    PlanningLP planning_lp(std::move(planning), flat_opts);
    REQUIRE(planning_lp.resolve().has_value());
    uppb2 = upper_row_uppb(
        planning_lp.systems().front().front().linear_interface());
  }

  // The uppb is M + rhs_eq, with rhs_eq = -φ = 0 for this fixture.
  // Cross-mode coefficient scaling (row equilibration) could rescale
  // the row by a uniform factor, but the RATIO uppb2/uppb1 should equal
  // M2/M1 = 17/5 = 3.4 exactly because both rows are scaled identically.
  REQUIRE(uppb1 > 0.0);
  REQUIRE(uppb2 > 0.0);
  const auto ratio = uppb2 / uppb1;
  const auto expected_ratio = M2 / M1;
  CHECK(ratio == doctest::Approx(expected_ratio).epsilon(1e-6));
}

// ── (7) IEEE 9-bus + cycle_basis cross-mode parity ───────────────────
//
// The IEEE 9-bus is the canonical OPF testbed used in tests (3) / (4)
// under node_angle.  This test runs the same case under cycle_basis
// + LineCommitment on ``l4_6`` (the high-reactance loop-completing
// branch) and verifies the LP-relax objective matches:
//   (a) the no-OTS baseline under cycle_basis  (u_l = 1 picks itself)
//   (b) the no-OTS baseline under node_angle    (cross-mode parity)
//
// Topology: 9 buses, 9 lines, 1 island → 9 − 9 + 1 = 1 fundamental
// cycle.  ``l4_6`` is on that cycle, so the disjunctive rewrite
// activates one ``kvl_minus`` row per block.

TEST_CASE(
    "LineCommitmentLP cycle_basis: IEEE 9b + LP-relax OTS matches "
    "node_angle baseline (v1)")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // Build helper that produces the IEEE 9b JSON in cycle_basis mode by
  // swapping the kirchhoff_mode literal in the existing fixture.
  auto swap_mode = [](std::string s) -> std::string
  {
    constexpr std::string_view from {"\"kirchhoff_mode\": \"node_angle\""};
    constexpr std::string_view to {"\"kirchhoff_mode\": \"cycle_basis\""};
    const auto pos = s.find(from);
    if (pos != std::string::npos) {
      s.replace(pos, from.size(), to);
    }
    return s;
  };

  // (a) node_angle baseline (no OTS) — the cross-mode anchor.
  double obj_node_angle_baseline = 0.0;
  {
    Planning planning;
    planning.merge(
        parse_planning_json(make_ieee9b_with_ots_json(/*with_lc=*/false)));
    PlanningLP planning_lp(std::move(planning));
    REQUIRE(planning_lp.resolve().has_value());
    obj_node_angle_baseline = planning_lp.systems()
                                  .front()
                                  .front()
                                  .linear_interface()
                                  .get_obj_value();
  }

  // (b) cycle_basis baseline (no OTS).  Must match node_angle: both
  // modes describe the same physical KVL, so the optimum is identical
  // up to numerical noise.
  double obj_cycle_baseline = 0.0;
  {
    Planning planning;
    planning.merge(parse_planning_json(
        swap_mode(make_ieee9b_with_ots_json(/*with_lc=*/false))));
    PlanningLP planning_lp(std::move(planning));
    REQUIRE(planning_lp.resolve().has_value());
    obj_cycle_baseline = planning_lp.systems()
                             .front()
                             .front()
                             .linear_interface()
                             .get_obj_value();
  }
  CHECK(obj_cycle_baseline
        == doctest::Approx(obj_node_angle_baseline).epsilon(1e-3));

  // (c) cycle_basis + OTS on l4_6 (LP-relax).  At u_l = 1 the
  // disjunctive cycle row collapses to equality and the objective must
  // match the cycle_basis baseline.
  double obj_cycle_ots = 0.0;
  {
    Planning planning;
    planning.merge(parse_planning_json(
        swap_mode(make_ieee9b_with_ots_json(/*with_lc=*/true))));
    PlanningLP planning_lp(std::move(planning));
    REQUIRE(planning_lp.resolve().has_value());
    obj_cycle_ots = planning_lp.systems()
                        .front()
                        .front()
                        .linear_interface()
                        .get_obj_value();
  }
  CHECK(obj_cycle_ots == doctest::Approx(obj_cycle_baseline).epsilon(1e-3));
}

// ── (8) IEEE 9-bus + cycle_basis + must_run ─────────────────────────
//
// Mirror of test (4) (node_angle + must_run) for the cycle_basis path.
// ``must_run = true`` pins ``u_l = 1`` via column bounds, which
// reproduces the no-OTS objective exactly because the disjunctive
// cycle row collapses to equality at ``u_l = 1`` (same physics as
// pre-OTS).

TEST_CASE(
    "LineCommitmentLP cycle_basis: IEEE 9b + must_run matches baseline (v1)")
{
  if (!tkbm_mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  auto swap_mode = [](std::string s) -> std::string
  {
    constexpr std::string_view from {"\"kirchhoff_mode\": \"node_angle\""};
    constexpr std::string_view to {"\"kirchhoff_mode\": \"cycle_basis\""};
    const auto pos = s.find(from);
    if (pos != std::string::npos) {
      s.replace(pos, from.size(), to);
    }
    return s;
  };

  // cycle_basis baseline (no OTS).
  Planning base;
  base.merge(parse_planning_json(
      swap_mode(make_ieee9b_with_ots_json(/*with_lc=*/false))));
  PlanningLP planning_baseline(std::move(base));
  REQUIRE(planning_baseline.resolve().has_value());
  const auto obj_baseline = planning_baseline.systems()
                                .front()
                                .front()
                                .linear_interface()
                                .get_obj_value();

  // cycle_basis + must_run OTS.
  Planning ots;
  ots.merge(parse_planning_json(swap_mode(
      make_ieee9b_with_ots_json(/*with_lc=*/true, /*must_run=*/true))));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  const auto obj_ots =
      planning_ots.systems().front().front().linear_interface().get_obj_value();

  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-3));
}
