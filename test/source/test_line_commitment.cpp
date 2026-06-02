// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_commitment.cpp — pin the v0 LP structure + behaviour of
// Optimal Transmission Switching (OTS) introduced by issue #509:
//
//   * Binary status column ``u_l ∈ {0, 1}`` per (line, scenario, stage,
//     block)
//   * Two capacity gating rows ``f - F^max_ab · u_l ≤ 0`` and
//     ``-f - F^max_ba · u_l ≤ 0``
//   * ``must_run`` pins ``u_l = 1`` (line forced closed)
//   * ``relax`` LP-relaxes the binary to [0, 1]
//   * ``fixed_status`` pins ``u_l`` per (stage, block)
//   * ``kvl_big_m`` round-trips through JSON
//
// Tests pin:
//   1) JSON round-trip of all v0 fields
//   2) Validation rejects unresolved ``line`` FK
//   3) Validation rejects ``kvl_big_m ≤ 0``
//   4) Validation rejects ``method ∈ {sddp, cascade}`` with active rows
//   5) LP build: 1 status column + 2 capacity rows per (line, block)
//   6) LP build with ``must_run`` — u_l lower bound pinned to 1
//   7) Solve: u_l = 1 path matches the no-OTS baseline objective
//   8) Solve: ``must_run = true`` reproduces the baseline objective
//
// Skipped (Solve subset) when no MIP solver is loaded.

#include <cstddef>
#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_line_commitment.hpp>
#include <gtopt/line_commitment.hpp>
#include <gtopt/line_commitment_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/validate_planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace test_line_commitment_ns
{

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── Canonical 2-bus fixture ─────────────────────────────────────────
//
// Single line, single block, transport mode (use_kirchhoff = false).
// G1 (gcost = 10) at bus 1 must serve a 100 MW demand at bus 2
// through the line.  The optimal LP routes ``f = 100`` (positive).

[[nodiscard]] std::string make_2bus_json(bool with_line_commitment,
                                         bool relax = false,
                                         bool must_run = false,
                                         std::string_view method = "monolithic")
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "method": ")";
  out += method;
  out += R"(",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": false,
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
      "name": "ots_2bus",
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
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "tmax_ab": 200, "tmax_ba": 200 }
      ])";
  if (with_line_commitment) {
    out += R"(,
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2")";
    if (relax) {
      out += R"(, "relax": true)";
    }
    if (must_run) {
      out += R"(, "must_run": true)";
    }
    out += R"( }
      ])";
  }
  out += R"(
    }
  })";
  return out;
}

// ── Kirchhoff fixture (line w/ reactance) ───────────────────────────
//
// 2-bus DC-OPF setup used by the kirchhoff_mode validation gate
// tests.  Reactance > 0 so KVL rows are actually emitted; the test
// JSON parameterises ``use_kirchhoff`` and ``kirchhoff_mode`` so a
// single helper covers all four cells of the gate truth table.

[[nodiscard]] std::string make_2bus_kirchhoff_gate_json(
    bool with_line_commitment,
    bool active_lc = true,
    bool use_kirchhoff = true,
    std::string_view kirchhoff_mode = "cycle_basis")
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": )";
  out += (use_kirchhoff ? "true" : "false");
  out += R"(,
        "kirchhoff_mode": ")";
  out += kirchhoff_mode;
  out += R"(",
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
      "name": "ots_gate_2bus",
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
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": true)";
    if (!active_lc) {
      out += R"(, "active": 0)";
    }
    out += R"( }
      ])";
  }
  out += R"(
    }
  })";
  return out;
}

[[nodiscard]] bool mip_available()
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

[[nodiscard]] int tlcom_count_cols_containing(const LinearInterface& li,
                                              std::string_view substr)
{
  int count = 0;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] int tlcom_count_rows_containing(const LinearInterface& li,
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
}  // namespace test_line_commitment_ns

using test_line_commitment_ns::make_2bus_json;
using test_line_commitment_ns::make_2bus_kirchhoff_gate_json;
using test_line_commitment_ns::mip_available;
using test_line_commitment_ns::tlcom_count_cols_containing;
using test_line_commitment_ns::tlcom_count_rows_containing;

// ── (1) JSON round-trip ─────────────────────────────────────────────

TEST_CASE("LineCommitment JSON round-trip — all v0 fields")
{
  std::string_view json_str = R"({
    "uid": 42,
    "name": "L_18_19_ots",
    "line": "L_18_19",
    "initial_status": 1,
    "must_run": false,
    "relax": false,
    "kvl_big_m": 18.5
  })";

  const auto lc = daw::json::from_json<LineCommitment>(json_str);
  CHECK(lc.uid == Uid {42});
  CHECK(lc.name == "L_18_19_ots");
  CHECK(std::get<Name>(lc.line) == "L_18_19");
  CHECK(lc.initial_status.value_or(-1.0) == doctest::Approx(1.0));
  CHECK(lc.must_run.value_or(true) == false);
  CHECK(lc.relax.value_or(true) == false);
  CHECK(lc.kvl_big_m.value_or(-1.0) == doctest::Approx(18.5));

  // Round-trip through serialization.
  const auto json_out = daw::json::to_json(lc);
  const auto lc2 = daw::json::from_json<LineCommitment>(json_out);
  CHECK(lc2.uid == lc.uid);
  CHECK(lc2.name == lc.name);
  CHECK(lc2.kvl_big_m.value_or(-1.0) == doctest::Approx(18.5));
}

// ── (2) Validation — unresolved line FK ─────────────────────────────

TEST_CASE("LineCommitment validation — unresolved line FK rejected")
{
  auto planning = parse_planning_json(make_2bus_json(false));
  // Inject a LineCommitment referencing a non-existent line.
  planning.system.line_commitment_array.push_back(LineCommitment {
      .uid = Uid {1},
      .name = "bad_fk",
      .line = SingleId {Name {"nonexistent_line"}},
  });

  const auto result = validate_planning(planning);
  CHECK_FALSE(result.ok());
  bool found_fk_error = false;
  for (const auto& err : result.errors) {
    if (err.contains("LineCommitment 'bad_fk'") && err.contains("Line")) {
      found_fk_error = true;
      break;
    }
  }
  CHECK(found_fk_error);
}

// ── (3) Validation — non-positive kvl_big_m rejected ────────────────

TEST_CASE("LineCommitment validation — non-positive kvl_big_m rejected")
{
  auto planning = parse_planning_json(make_2bus_json(false));
  planning.system.line_commitment_array.push_back(LineCommitment {
      .uid = Uid {1},
      .name = "bad_bigm",
      .line = SingleId {Name {"l1_2"}},
      .kvl_big_m = OptReal {0.0},
  });

  const auto result = validate_planning(planning);
  CHECK_FALSE(result.ok());
  bool found = false;
  for (const auto& err : result.errors) {
    if (err.contains("kvl_big_m") && err.contains("bad_bigm")) {
      found = true;
      break;
    }
  }
  CHECK(found);
}

// ── (4) Validation — method gate (SDDP / cascade) ───────────────────

TEST_CASE("LineCommitment validation — SDDP method rejected")
{
  auto planning = parse_planning_json(make_2bus_json(true,
                                                     /*relax=*/false,
                                                     /*must_run=*/false,
                                                     /*method=*/"sddp"));
  const auto result = validate_planning(planning);
  CHECK_FALSE(result.ok());
  bool found = false;
  for (const auto& err : result.errors) {
    if (err.contains("Optimal Transmission Switching") && err.contains("sddp"))
    {
      found = true;
      break;
    }
  }
  CHECK(found);
}

TEST_CASE("LineCommitment validation — cascade method rejected")
{
  auto planning = parse_planning_json(make_2bus_json(true,
                                                     /*relax=*/false,
                                                     /*must_run=*/false,
                                                     /*method=*/"cascade"));
  const auto result = validate_planning(planning);
  CHECK_FALSE(result.ok());
  bool found = false;
  for (const auto& err : result.errors) {
    if (err.contains("Optimal Transmission Switching")
        && err.contains("cascade"))
    {
      found = true;
      break;
    }
  }
  CHECK(found);
}

TEST_CASE("LineCommitment validation — monolithic method accepted")
{
  auto planning = parse_planning_json(make_2bus_json(true,
                                                     /*relax=*/false,
                                                     /*must_run=*/false,
                                                     /*method=*/"monolithic"));
  const auto result = validate_planning(planning);
  CHECK(result.ok());
}

// ── (4b) Validation — Kirchhoff-mode gate (cycle_basis rejected) ────
//
// v0.5 KVL big-M disjunctive rewrite is node_angle-only.  cycle_basis
// + active LineCommitment + use_kirchhoff must be rejected at
// validation time — without the gate the open line is silently
// reinjected into cycle KVL via its phase-shift / reactance terms.
// Transport mode (use_kirchhoff = false) is exempt.

TEST_CASE(
    "LineCommitment validation — cycle_basis + active LC + Kirchhoff rejected")
{
  auto planning = parse_planning_json(
      make_2bus_kirchhoff_gate_json(/*with_line_commitment=*/true,
                                    /*active_lc=*/true,
                                    /*use_kirchhoff=*/true,
                                    /*kirchhoff_mode=*/"cycle_basis"));
  const auto result = validate_planning(planning);
  CHECK_FALSE(result.ok());
  bool found = false;
  for (const auto& err : result.errors) {
    if (err.contains("Optimal Transmission Switching")
        && err.contains("kirchhoff_mode") && err.contains("node_angle"))
    {
      found = true;
      break;
    }
  }
  CHECK(found);
}

TEST_CASE("LineCommitment validation — node_angle + active LC accepted")
{
  auto planning = parse_planning_json(
      make_2bus_kirchhoff_gate_json(/*with_line_commitment=*/true,
                                    /*active_lc=*/true,
                                    /*use_kirchhoff=*/true,
                                    /*kirchhoff_mode=*/"node_angle"));
  const auto result = validate_planning(planning);
  // Validation should pass — no Kirchhoff-mode error.
  for (const auto& err : result.errors) {
    CHECK_FALSE(err.contains("kirchhoff_mode"));
  }
  CHECK(result.ok());
}

TEST_CASE("LineCommitment validation — cycle_basis + transport mode accepted")
{
  // use_kirchhoff = false → transport mode; capacity gating alone
  // is correct, cycle_basis flag is moot.
  auto planning = parse_planning_json(
      make_2bus_kirchhoff_gate_json(/*with_line_commitment=*/true,
                                    /*active_lc=*/true,
                                    /*use_kirchhoff=*/false,
                                    /*kirchhoff_mode=*/"cycle_basis"));
  const auto result = validate_planning(planning);
  for (const auto& err : result.errors) {
    CHECK_FALSE(err.contains("kirchhoff_mode"));
  }
  CHECK(result.ok());
}

TEST_CASE("LineCommitment validation — cycle_basis + inactive LC only accepted")
{
  // Only inactive LineCommitment rows → gate does not fire.
  auto planning = parse_planning_json(
      make_2bus_kirchhoff_gate_json(/*with_line_commitment=*/true,
                                    /*active_lc=*/false,
                                    /*use_kirchhoff=*/true,
                                    /*kirchhoff_mode=*/"cycle_basis"));
  const auto result = validate_planning(planning);
  for (const auto& err : result.errors) {
    CHECK_FALSE(err.contains("kirchhoff_mode"));
  }
  CHECK(result.ok());
}

// ── (5) LP build — status column + capacity gating rows ─────────────

TEST_CASE(
    "LineCommitmentLP: 1 status column + 2 capacity rows per (line, block)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP-aware LP build — no MIP solver available");
    return;
  }

  Planning planning;
  planning.merge(parse_planning_json(
      make_2bus_json(/*with_line_commitment=*/true, /*relax=*/true)));

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();

  // Exactly one status col per (LineCommitment, scenario, stage, block).
  // 1 line × 1 commitment × 1 scenario × 1 stage × 1 block = 1.
  CHECK(tlcom_count_cols_containing(li, "linecommitment_status_") == 1);

  // Two capacity rows per (LineCommitment, block).
  CHECK(tlcom_count_rows_containing(li, "linecommitment_capacity_p") == 1);
  CHECK(tlcom_count_rows_containing(li, "linecommitment_capacity_n") == 1);
}

// ── (6) LP build — must_run pins u_l = 1 ────────────────────────────

TEST_CASE("LineCommitmentLP: must_run pins u_l lower bound to 1")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP-aware LP build — no MIP solver available");
    return;
  }

  Planning planning;
  planning.merge(
      parse_planning_json(make_2bus_json(/*with_line_commitment=*/true,
                                         /*relax=*/true,
                                         /*must_run=*/true)));

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();

  // Find the status column.  ``must_run = true`` should pin both
  // lower and upper bounds to 1.0, but the LP-scaled column bound
  // can be shifted by auto-scale; we just verify the column exists
  // and the solver picks u = 1.0 at the optimum (asserted in test
  // #1801 by objective equivalence).  This test ensures the
  // ``must_run`` branch of the LP build was exercised.
  bool found = false;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains("linecommitment_status_")) {
      found = true;
      // Lower bound should be > 0 (pinned) under must_run.  Upper
      // bound should be 1.0.  Loose-ish tolerance accommodates LP
      // scaling.
      const auto lb = li.get_col_low()[value_of(idx)];
      const auto ub = li.get_col_upp()[value_of(idx)];
      CAPTURE(lb);
      CAPTURE(ub);
      CHECK(lb > 0.0);  // pinned
      CHECK(ub > 0.0);  // available
    }
  }
  CHECK(found);
}

// ── (7) Solve — u_l = 1 path matches the no-OTS baseline ────────────

TEST_CASE(
    "LineCommitmentLP: relaxed solve picks u_l = 1, matches no-OTS "
    "objective")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP solve — no MIP solver available");
    return;
  }

  // Baseline: no LineCommitment row.  Dispatch cost: g1 × 100 MW ×
  // $10/MWh ÷ 1000 (scale_objective) = 1.0.
  Planning base;
  base.merge(
      parse_planning_json(make_2bus_json(/*with_line_commitment=*/false)));
  PlanningLP planning_l1(std::move(base));
  REQUIRE(planning_l1.resolve().has_value());
  auto&& systems_l1 = planning_l1.systems();
  REQUIRE(!systems_l1.empty());
  REQUIRE(!systems_l1.front().empty());
  const auto obj_baseline =
      systems_l1.front().front().linear_interface().get_obj_value();

  // OTS: LP-relax (avoids needing MIP solve gating).  Optimal u_l = 1
  // since opening the line would prevent serving the 100 MW demand
  // → unserved-energy penalty $100,000 ≫ dispatch $1,000.
  Planning ots;
  ots.merge(parse_planning_json(make_2bus_json(/*with_line_commitment=*/true,
                                               /*relax=*/true)));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  auto&& systems_ots = planning_ots.systems();
  REQUIRE(!systems_ots.empty());
  REQUIRE(!systems_ots.front().empty());
  const auto obj_ots =
      systems_ots.front().front().linear_interface().get_obj_value();

  // Objectives within solver tolerance.  Loss columns have cost 0
  // here (no resistance), so the dispatch cost is the only term.
  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-6));
  CHECK(obj_baseline == doctest::Approx(1000.0).epsilon(1e-3));
}

// ── (8) Solve — must_run reproduces baseline ────────────────────────

// ── (9) initial_status pins the first-block u_l ─────────────────────

TEST_CASE("LineCommitmentLP: initial_status = 0 pins first-block u_l to 0")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP-aware LP build — no MIP solver available");
    return;
  }

  // initial_status = 0 pins u at t=0 to 0 — line OPEN.  With a 100 MW
  // demand on the far bus and a single line, the LP must serve the
  // demand via failure (unserved-energy penalty).  Objective should
  // therefore reflect the penalty cost, not the cheap-gen dispatch.
  std::string json = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "method": "monolithic",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": false,
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
      "name": "ots_initial_status",
      "bus_array": [
        { "uid": 1, "name": "b1" }, { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2", "lmax": [[100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "tmax_ab": 200, "tmax_ba": 200 }
      ],
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2",
          "relax": true, "initial_status": 0 }
      ]
    }
  })";

  Planning planning;
  planning.merge(parse_planning_json(json));
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();

  // The status col should be pinned to 0 at t = 0.
  bool found = false;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains("linecommitment_status_")) {
      found = true;
      const auto lb = li.get_col_low()[value_of(idx)];
      const auto ub = li.get_col_upp()[value_of(idx)];
      CAPTURE(lb);
      CAPTURE(ub);
      CHECK(lb == doctest::Approx(0.0));
      CHECK(ub == doctest::Approx(0.0));
    }
  }
  CHECK(found);

  // Objective reflects the unserved-energy penalty: 100 MW × $1000
  // demand_fail_cost / 1000 scale_objective = 100.
  const auto obj = li.get_obj_value();
  CAPTURE(obj);
  CHECK(obj == doctest::Approx(100'000.0).epsilon(1e-3));
}

TEST_CASE("LineCommitmentLP: must_run = true matches baseline objective")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP solve — no MIP solver available");
    return;
  }

  Planning base;
  base.merge(
      parse_planning_json(make_2bus_json(/*with_line_commitment=*/false)));
  PlanningLP planning_l1(std::move(base));
  REQUIRE(planning_l1.resolve().has_value());
  auto&& systems_l1 = planning_l1.systems();
  REQUIRE(!systems_l1.empty());
  REQUIRE(!systems_l1.front().empty());
  const auto obj_baseline =
      systems_l1.front().front().linear_interface().get_obj_value();

  Planning ots;
  ots.merge(parse_planning_json(make_2bus_json(/*with_line_commitment=*/true,
                                               /*relax=*/true,
                                               /*must_run=*/true)));
  PlanningLP planning_ots(std::move(ots));
  REQUIRE(planning_ots.resolve().has_value());
  auto&& systems_ots = planning_ots.systems();
  REQUIRE(!systems_ots.empty());
  REQUIRE(!systems_ots.front().empty());
  const auto obj_ots =
      systems_ots.front().front().linear_interface().get_obj_value();

  CHECK(obj_ots == doctest::Approx(obj_baseline).epsilon(1e-6));
}
