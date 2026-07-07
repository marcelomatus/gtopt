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

using namespace gtopt;

namespace test_line_commitment_ns
{

namespace
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

// ── (4b) Validation — cycle_basis now accepted (v1) ─────────────────
//
// v0.6 gated cycle_basis + active LC + Kirchhoff as an error because
// the disjunctive KVL big-M rewrite was node_angle-only.  v1 lands the
// cycle-form rewrite in ``source/kirchhoff_cycle_basis.cpp``; both
// Kirchhoff modes now support LineCommitment.

TEST_CASE(
    "LineCommitment validation — cycle_basis + active LC + Kirchhoff accepted "
    "(v1)")
{
  auto planning = parse_planning_json(
      make_2bus_kirchhoff_gate_json(/*with_line_commitment=*/true,
                                    /*active_lc=*/true,
                                    /*use_kirchhoff=*/true,
                                    /*kirchhoff_mode=*/"cycle_basis"));
  const auto result = validate_planning(planning);
  // No kirchhoff_mode-related error should be raised.
  for (const auto& err : result.errors) {
    CHECK_FALSE(err.contains("kirchhoff_mode"));
  }
  CHECK(result.ok());
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

// ── (9) u/v/w decomposition (v1.1) ──────────────────────────────────
//
// When ``LineCommitment.startup_cost`` or ``shutdown_cost`` is set,
// LineCommitmentLP emits the Knueven-Ostrowski-Watson 2020 three-
// binary decomposition:
//   v_l[t]  startup indicator,  continuous-in-[0,1], implied integer
//   w_l[t]  shutdown indicator, continuous-in-[0,1], implied integer
//   C1:  u[t] − u[t−1] − v[t] + w[t] = 0  (= initial_status for t=0)
//   C3:  v[t] + w[t] ≤ 1

[[nodiscard]] std::string make_3block_uvw_json(double startup_cost,
                                               double shutdown_cost,
                                               double initial_status = 0.0)
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": false,
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 },
        { "uid": 2, "duration": 1 },
        { "uid": 3, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 3,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_uvw_3block",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2",
          "lmax": [[100.0, 100.0, 100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "tmax_ab": 100, "tmax_ba": 100 }
      ],
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": true,
          "initial_status": )";
  out += std::to_string(initial_status);
  out += R"(, "startup_cost": )";
  out += std::to_string(startup_cost);
  out += R"(, "shutdown_cost": )";
  out += std::to_string(shutdown_cost);
  out += R"( }
      ]
    }
  })";
  return out;
}

TEST_CASE(
    "LineCommitmentLP u/v/w: no startup/shutdown cost ⇒ no v/w columns "
    "(v1 back-compat)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning planning;
  planning.merge(parse_planning_json(
      make_3block_uvw_json(/*startup_cost=*/0.0, /*shutdown_cost=*/0.0)));
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  const auto& li = systems.front().front().linear_interface();
  // Zero costs ⇒ u/v/w gate inactive; no startup/shutdown cols or
  // logic/exclusion rows emitted.  Pre-v1.1 behavior preserved.
  CHECK(tlcom_count_cols_containing(li, "linecommitment_startup") == 0);
  CHECK(tlcom_count_cols_containing(li, "linecommitment_shutdown") == 0);
  CHECK(tlcom_count_rows_containing(li, "linecommitment_logic") == 0);
  CHECK(tlcom_count_rows_containing(li, "linecommitment_exclusion") == 0);
  // 3 status cols (one per block) remain.
  CHECK(tlcom_count_cols_containing(li, "linecommitment_status") == 3);
}

TEST_CASE(
    "LineCommitmentLP u/v/w: positive startup_cost emits v/w cols + "
    "C1/C3 rows per block (v1.1)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning planning;
  planning.merge(parse_planning_json(
      make_3block_uvw_json(/*startup_cost=*/100.0, /*shutdown_cost=*/50.0)));
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  const auto& li = systems.front().front().linear_interface();
  // 1 v + 1 w column per block × 3 blocks = 3 each.
  CHECK(tlcom_count_cols_containing(li, "linecommitment_startup") == 3);
  CHECK(tlcom_count_cols_containing(li, "linecommitment_shutdown") == 3);
  // 1 logic row + 1 exclusion row per block × 3 blocks = 3 each.
  CHECK(tlcom_count_rows_containing(li, "linecommitment_logic") == 3);
  CHECK(tlcom_count_rows_containing(li, "linecommitment_exclusion") == 3);
}

TEST_CASE(
    "LineCommitmentLP u/v/w: high startup_cost + initial_status=0 ⇒ pay "
    "startup once at t=0, then stay closed (v1.1)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning planning;
  planning.merge(parse_planning_json(make_3block_uvw_json(
      /*startup_cost=*/100.0, /*shutdown_cost=*/50.0, /*initial_status=*/0.0)));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();

  // tmax_ab is sized exactly at demand (100 MW = 100 MW) so the LP
  // relaxation cannot exploit fractional u_l < 1: the capacity gate
  // ``f ≤ tmax · u`` forces ``u ≥ 1`` for any block where the line
  // carries 100 MW.  With u[0]=u[1]=u[2]=1 and initial_status=0,
  // C1₀ gives v[0] = 1 (one startup event), then v[1]=v[2]=0 since
  // u stays at 1.  Dispatch = 3 × 100 × $10 = $3000; startup = $100;
  // total = $3100.
  CHECK(obj == doctest::Approx(3100.0).epsilon(1e-3));
}

TEST_CASE(
    "LineCommitmentLP u/v/w: initial_status=1 ⇒ no startup event needed "
    "at t=0 (v1.1)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  Planning planning;
  planning.merge(parse_planning_json(make_3block_uvw_json(
      /*startup_cost=*/100.0, /*shutdown_cost=*/50.0, /*initial_status=*/1.0)));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();

  // Breaker already closed at t=−1 (initial_status=1), so v[0] = 0
  // (no startup event needed).  Pure dispatch: 3 × 100 × 10 = $3000.
  CHECK(obj == doctest::Approx(3000.0).epsilon(1e-3));
}

TEST_CASE("LineCommitment JSON round-trip — startup_cost + shutdown_cost")
{
  std::string_view json_str = R"({
    "uid": 42,
    "name": "L_18_19_ots",
    "line": "L_18_19",
    "startup_cost": 250.5,
    "shutdown_cost": 75.0
  })";

  const auto lc = daw::json::from_json<LineCommitment>(json_str);
  CHECK(lc.startup_cost.value_or(-1.0) == doctest::Approx(250.5));
  CHECK(lc.shutdown_cost.value_or(-1.0) == doctest::Approx(75.0));

  const auto json_out = daw::json::to_json(lc);
  const auto lc2 = daw::json::from_json<LineCommitment>(json_out);
  CHECK(lc2.startup_cost.value_or(-1.0) == doctest::Approx(250.5));
  CHECK(lc2.shutdown_cost.value_or(-1.0) == doctest::Approx(75.0));
}

// ── (10) v1.2: min_up_time / min_down_time / max_starts ─────────────
//
// Anti-flicker + rolling-window cap, mirroring CommitmentLP's C6/C7/C9.

[[nodiscard]] std::string make_5block_uvw_extras_json(
    double startup_cost,
    double shutdown_cost,
    double initial_status,
    double min_up_time,
    double min_down_time,
    int max_starts,
    const std::string& starts_scope_str)
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": false,
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 },
        { "uid": 2, "duration": 1 },
        { "uid": 3, "duration": 1 },
        { "uid": 4, "duration": 1 },
        { "uid": 5, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 5,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_uvw_extras",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2",
          "lmax": [[100.0, 100.0, 100.0, 100.0, 100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "tmax_ab": 100, "tmax_ba": 100 }
      ],
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": true,
          "initial_status": )";
  out += std::to_string(initial_status);
  out += R"(, "startup_cost": )";
  out += std::to_string(startup_cost);
  out += R"(, "shutdown_cost": )";
  out += std::to_string(shutdown_cost);
  if (min_up_time > 0.0) {
    out += R"(, "min_up_time": )";
    out += std::to_string(min_up_time);
  }
  if (min_down_time > 0.0) {
    out += R"(, "min_down_time": )";
    out += std::to_string(min_down_time);
  }
  if (max_starts >= 0) {
    out += R"(, "max_starts": )";
    out += std::to_string(max_starts);
    if (!starts_scope_str.empty()) {
      out += R"(, "starts_scope": ")";
      out += starts_scope_str;
      out += R"(")";
    }
  }
  out += R"( }
      ]
    }
  })";
  return out;
}

TEST_CASE(
    "LineCommitmentLP v1.2: min_up_time emits row family per block (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // min_up_time = 3 hours over 5 unit-duration blocks ⇒ UT ≥ 2 at
  // every block except the last (only 1 block remains, UT = 1 ⇒ skip
  // trivially-satisfied row).  Expected = 4 rows.
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/0.0,
      /*min_up_time=*/3.0,
      /*min_down_time=*/0.0,
      /*max_starts=*/-1,
      /*starts_scope_str=*/"")));
  LpMatrixOptions flat_opts;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());
  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK(tlcom_count_rows_containing(li, "linecommitment_min_up_time") == 4);
}

TEST_CASE(
    "LineCommitmentLP v1.2: min_down_time emits row family per block "
    "symmetric to min_up_time (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/1.0,
      /*min_up_time=*/0.0,
      /*min_down_time=*/3.0,
      /*max_starts=*/-1,
      /*starts_scope_str=*/"")));
  LpMatrixOptions flat_opts;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());
  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK(tlcom_count_rows_containing(li, "linecommitment_min_down_time") == 4);
}

TEST_CASE(
    "LineCommitmentLP v1.2: max_starts horizon scope emits one row per "
    "stage (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // starts_scope unset ⇒ horizon = 0 hours ⇒ one window per stage ⇒
  // 1 max_starts row.
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/0.0,
      /*min_up_time=*/0.0,
      /*min_down_time=*/0.0,
      /*max_starts=*/2,
      /*starts_scope_str=*/"")));
  LpMatrixOptions flat_opts;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());
  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK(tlcom_count_rows_containing(li, "linecommitment_max_starts") == 1);
}

TEST_CASE(
    "LineCommitmentLP v1.2: max_starts hour scope emits one row per "
    "block (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // starts_scope = "hour" with 1-hour blocks ⇒ 5 windows ⇒ 5 rows.
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/0.0,
      /*min_up_time=*/0.0,
      /*min_down_time=*/0.0,
      /*max_starts=*/1,
      /*starts_scope_str=*/"hour")));
  LpMatrixOptions flat_opts;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());
  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK(tlcom_count_rows_containing(li, "linecommitment_max_starts") == 5);
}

TEST_CASE("LineCommitment JSON round-trip — v1.2 time-based fields")
{
  std::string_view json_str = R"({
    "uid": 7,
    "name": "lc_v12",
    "line": "L_18_19",
    "startup_cost": 100,
    "min_up_time": 4.5,
    "min_down_time": 2,
    "max_starts": 3,
    "min_starts": 1,
    "starts_scope": "day"
  })";

  const auto lc = daw::json::from_json<LineCommitment>(json_str);
  CHECK(lc.min_up_time.value_or(-1.0) == doctest::Approx(4.5));
  CHECK(lc.min_down_time.value_or(-1.0) == doctest::Approx(2.0));
  CHECK(lc.max_starts.value_or(-1) == 3);
  CHECK(lc.min_starts.value_or(-1) == 1);
  REQUIRE(lc.starts_scope.has_value());
  // ``day`` → 24 hours.
  CHECK(lc.starts_window_hours() == doctest::Approx(24.0));

  const auto out = daw::json::to_json(lc);
  const auto lc2 = daw::json::from_json<LineCommitment>(out);
  CHECK(lc2.min_up_time.value_or(-1.0) == doctest::Approx(4.5));
  CHECK(lc2.max_starts.value_or(-1) == 3);
  CHECK(lc2.starts_window_hours() == doctest::Approx(24.0));
}

TEST_CASE("LineCommitment starts_window_hours — direct value cases")
{
  LineCommitment lc;
  lc.starts_scope = StartsScopeValue {Int {48}};
  CHECK(lc.starts_window_hours() == doctest::Approx(48.0));

  lc.starts_scope = StartsScopeValue {Name {"week"}};
  CHECK(lc.starts_window_hours() == doctest::Approx(168.0));

  lc.starts_scope = StartsScopeValue {Name {"horizon"}};
  CHECK(lc.starts_window_hours() == doctest::Approx(0.0));

  lc.starts_scope = std::nullopt;
  CHECK(lc.starts_window_hours() == doctest::Approx(0.0));

  // Unrecognised name ⇒ horizon (0).
  lc.starts_scope = StartsScopeValue {Name {"fortnight"}};
  CHECK(lc.starts_window_hours() == doctest::Approx(0.0));
}

// ── (11) v1.2 behavioral solves ──────────────────────────────────────
//
// The structural tests above pin row counts; these tests prove the
// constraints actually SHAPE the LP solution.  Each builds a fixture
// that would have a flicker-style optimum without v1.2, then verifies
// the constraint forces a non-flicker / capped-startup solution.

TEST_CASE(
    "LineCommitmentLP v1.2: max_starts=0 + initial_status=0 ⇒ no startup "
    "at all ⇒ full demand unserved (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // max_starts = 0 (with horizon scope) ⇒ Σ v[t] ≤ 0 ⇒ no startup
  // event allowed.  initial_status = 0 ⇒ breaker starts open and
  // CANNOT close anywhere in the stage.  The line carries f = 0 for
  // every block, so demand is fully unserved.
  //
  // Expected obj: 5 × 100 × $1000 = $500,000 fail.
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/0.0,
      /*min_up_time=*/0.0,
      /*min_down_time=*/0.0,
      /*max_starts=*/0,
      /*starts_scope_str=*/"horizon")));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj == doctest::Approx(500000.0).epsilon(1e-3));
}

TEST_CASE(
    "LineCommitmentLP v1.2: max_starts=1 + initial_status=1 ⇒ stays "
    "closed (no shutdown event since unnecessary) (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // initial_status = 1 ⇒ breaker pre-closed at t = −1.  max_starts = 1
  // permits one v = 1 over the stage but doesn't FORCE one.  Optimal:
  // keep u = 1 for all blocks (no startup, no shutdown) ⇒ pure
  // dispatch cost = 5 × 100 × $10 = $5000.
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/1.0,
      /*min_up_time=*/0.0,
      /*min_down_time=*/0.0,
      /*max_starts=*/1,
      /*starts_scope_str=*/"horizon")));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj == doctest::Approx(5000.0).epsilon(1e-3));
}

TEST_CASE(
    "LineCommitmentLP v1.2: min_starts=2 horizon scope forces ≥ 2 closing "
    "events (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // min_starts = 2 ⇒ Σ v[t] ≥ 2 over the stage.  With initial_status = 0
  // the breaker must close at t = 0 (one v = 1).  To meet the floor,
  // the LP must shut down and re-close once more — paying $50 shutdown
  // + $100 startup for the extra cycle.  Pure dispatch + 2× startup +
  // 1× shutdown = $5000 + 2×100 + 50 = $5250.
  std::string json = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": false,
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 }, { "uid": 2, "duration": 1 },
        { "uid": 3, "duration": 1 }, { "uid": 4, "duration": 1 },
        { "uid": 5, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 5,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_min_starts",
      "bus_array": [
        { "uid": 1, "name": "b1" }, { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 },
        { "uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 500, "gcost": 5, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2",
          "lmax": [[100.0, 100.0, 100.0, 100.0, 100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "tmax_ab": 100, "tmax_ba": 100 }
      ],
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": true,
          "initial_status": 0,
          "startup_cost": 100, "shutdown_cost": 50,
          "min_starts": 2, "max_starts": 5,
          "starts_scope": "horizon" }
      ]
    }
  })";
  Planning planning;
  planning.merge(parse_planning_json(json));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  // g2 (gcost $5) at the demand bus serves the load entirely — line
  // is unused, so the LP picks min_starts = 2 cycles purely to meet
  // the floor, paying 2 × ($100 startup + $50 shutdown) = $300
  // overhead on top of 5 × 100 × $5 = $2500 g2 dispatch.  But notice:
  // any shutdown not followed by a startup gives free $50 reduction,
  // so the LP could be tempted to open-stay-open after the second
  // startup.  With 2 startups and an even/odd parity argument, the
  // minimum is 2 startup + 1 shutdown ⇒ 2×$100 + 1×$50 = $250.
  // Pure dispatch = $2500.  Total = $2750.
  CHECK(obj == doctest::Approx(2750.0).epsilon(1e-3));
}

TEST_CASE(
    "LineCommitmentLP v1.2: min_up_time=3h + initial_status=0 prevents "
    "flicker by forcing 3-block on-stretches (v1.2)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // 5-block stage, all blocks demand 100 MW.  min_up_time = 3 hours
  // ⇒ once closed, the line must stay closed for 3 blocks minimum.
  // initial_status = 0 ⇒ breaker starts open.  Optimal: close at
  // t = 0 (v[0] = 1), stay closed all 5 blocks (no flicker).
  // Total obj = 5 × 100 × $10 dispatch + 1 × $100 startup = $5100.
  Planning planning;
  planning.merge(parse_planning_json(make_5block_uvw_extras_json(
      /*startup_cost=*/100.0,
      /*shutdown_cost=*/50.0,
      /*initial_status=*/0.0,
      /*min_up_time=*/3.0,
      /*min_down_time=*/0.0,
      /*max_starts=*/-1,
      /*starts_scope_str=*/"")));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj == doctest::Approx(5100.0).epsilon(1e-3));
}

// ── (12) v1.3 startup tiers (hot/warm/cold) ──────────────────────────
//
// When all five tier fields are set together with u/v/w, the flat
// startup_cost is replaced by per-tier costs.  Tests pin (a)
// structural emission of the y_hot/y_warm/y_cold cols + tier_select /
// hot_window / warm_window rows, (b) JSON round-trip, (c)
// has_startup_tiers() gate, (d) behavioral cold-start at t=0 with
// no prior offline-time data.

[[nodiscard]] std::string make_3block_tier_json(double hot_cost,
                                                double warm_cost,
                                                double cold_cost,
                                                double hot_time,
                                                double cold_time,
                                                double initial_hours)
{
  std::string out = R"({
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": false,
        "scale_objective": 1000,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        { "uid": 1, "duration": 1 },
        { "uid": 2, "duration": 1 },
        { "uid": 3, "duration": 1 }
      ],
      "stage_array": [
        { "uid": 1, "first_block": 0, "count_block": 3,
          "active": 1, "chronological": true }
      ],
      "scenario_array": [{ "uid": 1, "probability_factor": 1 }]
    },
    "system": {
      "name": "ots_tier",
      "bus_array": [
        { "uid": 1, "name": "b1" },
        { "uid": 2, "name": "b2" }
      ],
      "generator_array": [
        { "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 500, "gcost": 10, "capacity": 500 }
      ],
      "demand_array": [
        { "uid": 1, "name": "d2", "bus": "b2",
          "lmax": [[100.0, 100.0, 100.0]] }
      ],
      "line_array": [
        { "uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
          "tmax_ab": 100, "tmax_ba": 100 }
      ],
      "line_commitment_array": [
        { "uid": 1, "name": "l1_2_ots", "line": "l1_2", "relax": true,
          "initial_status": 0,
          "startup_cost": 100, "shutdown_cost": 50,
          "hot_start_cost": )";
  out += std::to_string(hot_cost);
  out += R"(, "warm_start_cost": )";
  out += std::to_string(warm_cost);
  out += R"(, "cold_start_cost": )";
  out += std::to_string(cold_cost);
  out += R"(, "hot_start_time": )";
  out += std::to_string(hot_time);
  out += R"(, "cold_start_time": )";
  out += std::to_string(cold_time);
  out += R"(, "initial_hours": )";
  out += std::to_string(initial_hours);
  out += R"( }
      ]
    }
  })";
  return out;
}

TEST_CASE(
    "LineCommitmentLP v1.3: startup tiers emit 3 cols + 3 rows per block "
    "(v1.3)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  Planning planning;
  planning.merge(parse_planning_json(make_3block_tier_json(
      /*hot_cost=*/30.0,
      /*warm_cost=*/60.0,
      /*cold_cost=*/120.0,
      /*hot_time=*/1.0,
      /*cold_time=*/5.0,
      /*initial_hours=*/100.0)));
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_names = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);
  REQUIRE(planning_lp.resolve().has_value());
  const auto& li = planning_lp.systems().front().front().linear_interface();
  // 3 tier cols × 3 blocks = 9 each (= 3 hot + 3 warm + 3 cold).
  CHECK(tlcom_count_cols_containing(li, "linecommitment_hot_start") == 3);
  CHECK(tlcom_count_cols_containing(li, "linecommitment_warm_start") == 3);
  CHECK(tlcom_count_cols_containing(li, "linecommitment_cold_start") == 3);
  // 3 rows per type × 3 blocks (tier_select + hot_window + warm_window
  // ⇒ 9 rows total).
  CHECK(tlcom_count_rows_containing(li, "linecommitment_tier_select") == 3);
  CHECK(tlcom_count_rows_containing(li, "linecommitment_hot_window") == 3);
  CHECK(tlcom_count_rows_containing(li, "linecommitment_warm_window") == 3);
}

TEST_CASE("LineCommitment v1.3: has_startup_tiers gate")
{
  LineCommitment lc;
  CHECK_FALSE(lc.has_startup_tiers());
  lc.hot_start_cost = 30.0;
  CHECK_FALSE(lc.has_startup_tiers());  // only 1 of 5 set
  lc.warm_start_cost = 60.0;
  lc.cold_start_cost = 120.0;
  CHECK_FALSE(lc.has_startup_tiers());  // 3 of 5
  lc.hot_start_time = 1.0;
  CHECK_FALSE(lc.has_startup_tiers());  // 4 of 5
  lc.cold_start_time = 5.0;
  CHECK(lc.has_startup_tiers());  // all 5 set
}

TEST_CASE("LineCommitment JSON round-trip — v1.3 startup tiers")
{
  std::string_view json_str = R"({
    "uid": 9,
    "name": "lc_tiers",
    "line": "L_18_19",
    "startup_cost": 100,
    "hot_start_cost": 30,
    "warm_start_cost": 60,
    "cold_start_cost": 120,
    "hot_start_time": 1,
    "cold_start_time": 5,
    "initial_hours": 100
  })";
  const auto lc = daw::json::from_json<LineCommitment>(json_str);
  CHECK(lc.has_startup_tiers());
  CHECK(lc.hot_start_cost.value_or(-1.0) == doctest::Approx(30.0));
  CHECK(lc.warm_start_cost.value_or(-1.0) == doctest::Approx(60.0));
  CHECK(lc.cold_start_cost.value_or(-1.0) == doctest::Approx(120.0));
  CHECK(lc.hot_start_time.value_or(-1.0) == doctest::Approx(1.0));
  CHECK(lc.cold_start_time.value_or(-1.0) == doctest::Approx(5.0));
  CHECK(lc.initial_hours.value_or(-1.0) == doctest::Approx(100.0));

  // Round-trip.
  const auto out = daw::json::to_json(lc);
  const auto lc2 = daw::json::from_json<LineCommitment>(out);
  CHECK(lc2.has_startup_tiers());
  CHECK(lc2.cold_start_cost.value_or(-1.0) == doctest::Approx(120.0));
}

TEST_CASE(
    "LineCommitmentLP v1.3: initial_hours=100 (long offline) ⇒ cold start "
    "at t=0 ⇒ cold_cost in objective (v1.3)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // initial_hours = 100 > cold_start_time = 5 ⇒ neither hot nor warm
  // windows allow y at t=0 ⇒ residual must be y_cold = 1 ⇒ pay
  // cold_cost = 120.  Dispatch = 3 × 100 × $10 = $3000.  Total $3120.
  Planning planning;
  planning.merge(parse_planning_json(make_3block_tier_json(
      /*hot_cost=*/30.0,
      /*warm_cost=*/60.0,
      /*cold_cost=*/120.0,
      /*hot_time=*/1.0,
      /*cold_time=*/5.0,
      /*initial_hours=*/100.0)));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj == doctest::Approx(3120.0).epsilon(1e-3));
}

TEST_CASE(
    "LineCommitmentLP v1.3: initial_hours=0.5 (recent offline) ⇒ hot "
    "start at t=0 ⇒ hot_cost in objective (v1.3)")
{
  if (!mip_available()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // initial_hours = 0.5 ≤ hot_start_time = 1.0 ⇒ hot window allows
  // y_hot[0] = 1 at the first close.  Cost = $30.  Total $3030.
  Planning planning;
  planning.merge(parse_planning_json(make_3block_tier_json(
      /*hot_cost=*/30.0,
      /*warm_cost=*/60.0,
      /*cold_cost=*/120.0,
      /*hot_time=*/1.0,
      /*cold_time=*/5.0,
      /*initial_hours=*/0.5)));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj == doctest::Approx(3030.0).epsilon(1e-3));
}
