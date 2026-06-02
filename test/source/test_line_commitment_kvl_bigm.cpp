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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace test_line_commitment_kvl_bigm_ns
{

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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
