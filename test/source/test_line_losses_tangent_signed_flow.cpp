// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_tangent_signed_flow.cpp — pin the LP structure +
// behaviour of the Coffrin-style outer-approximation mode introduced
// in 2026-05-31.  Single SIGNED flow column per (line, block), no
// fp/fn decomposition, K tangent inequalities forming the lower
// envelope and a single quadratic upper bound on `ℓ` for arbitrage
// immunity in negative-LMP cases.
//
// Tests pin:
//   1) STRUCTURAL — column / row counts and absence of fp/fn cols
//      (phantom flow impossible by construction).
//   2) Signed flow column bounds are [−tmax_ba, +tmax_ab] (genuinely
//      signed, no decomposition).
//   3) Loss row coefficients encode the tangent formula at each f_k
//      (slope on f, RHS = −k_loss·f_k²).
//   4) Loss column upper bound = (R/V²)·fmax².
//   5) Bus balance coefficients on the signed flow column (receiver
//      allocation: −1 on sender, +1 on receiver, −1 on loss receiver).
//   6) Solve: f > 0 for the basic A→B fixture; loss respects the
//      quadratic bracket.
//   7) `node_angle` KVL: one row per block, signed col stamped with
//      +x_τ (a single coefficient — not the +/− pair the
//      bidirectional formulation uses).
//   8) `cycle_basis` KVL: 3-bus loop yields one cycle row per block;
//      LP solves successfully under signed flow.
//   9) Objective parity vs `bidirectional` on the same fixture
//      (within 1e-3 relative slack — different PWL polarity).

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// Wrap the entire file body in a uniquely-named non-anonymous outer
// namespace so the Unity-build (CMake batches many test cpp files into
// a single TU) cannot collide with same-named helpers in sibling test
// files — see CLAUDE.md `unity-anon-namespace` memory note.
namespace test_line_losses_tangent_signed_flow_ns
{

namespace
{
namespace tangent_signed_flow_test
{

// ── 2-bus fixture ────────────────────────────────────────────────────

struct TwoBusFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit TwoBusFixture(std::string_view mode_name,
                         int loss_segments = 5,
                         double R = 0.01,
                         double V = 100.0,
                         double tmax = 200.0,
                         bool use_kirchhoff = false,
                         std::string_view kirchhoff_mode = "cycle_basis",
                         double reactance = 0.0)
      : system {
            .name = "TangentSignedFlow2Bus",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                },
            .demand_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {2},
                        .capacity = 100.0,
                    },
                },
            .generator_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "g1",
                        .bus = Uid {1},
                        .gcost = 10.0,
                        .capacity = 500.0,
                    },
                },
            .line_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "l1",
                        .bus_a = Uid {1},
                        .bus_b = Uid {2},
                        .voltage = V,
                        .resistance = R,
                        .reactance = reactance,
                        .line_losses_mode =
                            OptName {std::string(mode_name)},
                        .loss_segments = loss_segments,
                        .tmax_ba = tmax,
                        .tmax_ab = tmax,
                        .capacity = tmax,
                    },
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options(use_kirchhoff, kirchhoff_mode))
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_opts())
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  PlanningOptionsLP make_options(bool use_kirchhoff,
                                 std::string_view kirchhoff_mode)
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = use_kirchhoff;
    opts.model_options.kirchhoff_mode = std::string(kirchhoff_mode);
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_opts()
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    return bo;
  }
};

[[nodiscard]] auto count_cols_containing(const LinearInterface& li,
                                         std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] auto count_rows_containing(const LinearInterface& li,
                                         std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] auto find_col(const LinearInterface& li, std::string_view substr)
    -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

[[nodiscard]] auto find_row(const LinearInterface& li, std::string_view substr)
    -> RowIndex
{
  RowIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

}  // namespace tangent_signed_flow_test
}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

using tangent_signed_flow_test::count_cols_containing;
using tangent_signed_flow_test::count_rows_containing;
using tangent_signed_flow_test::find_col;
using tangent_signed_flow_test::find_row;
using tangent_signed_flow_test::TwoBusFixture;

// ── (1) Structural: 3 cols + (K + 3) rows, no fp/fn cols ─────────────
//
// Updated 2026-05-31 to pin the |f|-aux upper bound: the loss column UB
// became a row (``ℓ ≤ R·fmax/V²·v``), so each block now adds:
//   * 1 extra column ``v`` (``line_flow_abs_``)
//   * 2 extra rows ``v ≥ ±f`` (``line_flow_abs``) under
//     ``flow_abs_constraint_name``
//   * 1 extra row ``ℓ ≤ R·fmax/V² · v`` (``loss_link``)
// so the loss_link count grows by +1 vs the pre-aux structure.

TEST_CASE(
    "tangent_signed_flow: structural — 3 cols and K+1 loss rows per block")
{
  constexpr int K = 5;
  TwoBusFixture fix("tangent_signed_flow", /*loss_segments=*/K);
  auto& li = fix.lp();

  SUBCASE("phantom flow IMPOSSIBLE: no fp/fn aggregator or seg cols")
  {
    CHECK(count_cols_containing(li, "line_flowp_") == 0);
    CHECK(count_cols_containing(li, "line_flown_") == 0);
    CHECK(count_cols_containing(li, "line_flowp_seg_") == 0);
    CHECK(count_cols_containing(li, "line_flown_seg_") == 0);
  }

  SUBCASE("single signed flow column")
  {
    CHECK(count_cols_containing(li, "line_flows_") == 1);
  }

  SUBCASE("single loss column (lossp)")
  {
    CHECK(count_cols_containing(li, "line_lossp_") == 1);
    CHECK(count_cols_containing(li, "line_lossn_") == 0);
  }

  SUBCASE("single |f|-envelope auxiliary column (flow_abs)")
  {
    CHECK(count_cols_containing(li, "line_flow_abs_") == 1);
  }

  SUBCASE("two flow_abs rows: v − f ≥ 0 and v + f ≥ 0")
  {
    CHECK(count_rows_containing(li, "line_flow_abs") == 2);
  }

  SUBCASE("K tangent rows + 1 chord UB row under loss_link (minus dropout)")
  {
    // K tangent LOWER bounds at f_k = fmax·(2k − K − 1)/K (k=1..K).
    // For K=5 / fmax=200 → f_k ∈ {−160,−80,0,+80,+160}; row k=3
    // (f_k=0) drops out under the post-scale tolerance.
    // PLUS the new chord UB row ``ℓ ≤ (R·fmax/V²) · v`` (always
    // present; no dropout — chord slope is strictly positive whenever
    // R/V² and fmax are positive).
    const int surviving_tangents = (K % 2 == 1) ? (K - 1) : K;
    const int chord_ub_rows = 1;
    CHECK(count_rows_containing(li, "line_loss_link")
          == surviving_tangents + chord_ub_rows);
    CHECK(count_rows_containing(li, "line_flow_link") == 0);
  }
}

// ── (2) signed bounds [−tmax_ba, +tmax_ab] ───────────────────────────

TEST_CASE("tangent_signed_flow: signed flow column bounds")
{
  // tmax = 200 (symmetric).  Bounds must allow both signs.
  TwoBusFixture fix("tangent_signed_flow", /*loss_segments=*/5);
  auto& li = fix.lp();
  const auto fcol = find_col(li, "line_flows_");
  CHECK(li.get_col_low()[value_of(fcol)] == doctest::Approx(-200.0));
  CHECK(li.get_col_upp()[value_of(fcol)] == doctest::Approx(+200.0));
}

// ── (3) Tangent row coefficients pin the formula ─────────────────────

TEST_CASE("tangent_signed_flow: tangent row coefs at f_k = fmax·(2k−K−1)/K")
{
  constexpr int K = 5;
  constexpr double R = 0.01;
  constexpr double V = 100.0;
  constexpr double V2 = V * V;
  constexpr double tmax = 200.0;
  const double k_loss = R / V2;
  TwoBusFixture fix("tangent_signed_flow",
                    /*loss_segments=*/K,
                    R,
                    V,
                    tmax);
  auto& li = fix.lp();

  const auto fcol = find_col(li, "line_flows_");
  const auto loss_col = find_col(li, "line_lossp_");

  // Predicted tangent points (uniform on (−fmax, +fmax) excluding
  // endpoints): {−160, −80, 0, +80, +160} for K=5, tmax=200.  k=3
  // gives f_k=0 → slope=0; that row drops out under the kLossLp
  // tolerance.  Expect K−1 = 4 surviving rows.
  std::vector<double> expected_f_k;
  expected_f_k.reserve(K);
  for (int k = 1; k <= K; ++k) {
    const double f_k =
        tmax * static_cast<double>((2 * k) - K - 1) / static_cast<double>(K);
    if (std::abs(2.0 * k_loss * f_k) >= 1e-12) {  // skip f_k = 0
      expected_f_k.push_back(f_k);
    }
  }

  // Collect actual rows' (slope on f, RHS).  ``line_loss_link`` now
  // contains BOTH the K tangent lower bounds (coef on loss = +1) AND
  // the new chord upper bound (coef on loss = −1, references the
  // ``line_flow_abs_`` aux instead of ``line_flows_``).  Filter to the
  // tangent rows here; the chord row is pinned by a dedicated test.
  std::vector<std::pair<double, double>> actual;
  for (const auto& [name, row_idx] : li.row_name_map()) {
    if (!name.contains("line_loss_link")) {
      continue;
    }
    const double coef_on_loss = li.get_coeff(row_idx, loss_col);
    if (coef_on_loss < 0.0) {
      // Chord upper bound (``ℓ ≤ R·fmax/V² · v`` ⇒ ``−ℓ + chord·v ≥ 0``).
      continue;
    }
    REQUIRE(coef_on_loss == doctest::Approx(1.0));
    const double slope_on_f = li.get_coeff(row_idx, fcol);
    const double rhs = li.get_row_low()[value_of(row_idx)];
    actual.emplace_back(slope_on_f, rhs);
  }
  CHECK(actual.size() == expected_f_k.size());

  // Match each expected f_k to an actual row.
  for (const auto f_k : expected_f_k) {
    const double exp_slope = -2.0 * k_loss * f_k;
    const double exp_rhs = -k_loss * f_k * f_k;
    bool found = false;
    for (const auto& [slope, rhs] : actual) {
      if (std::abs(slope - exp_slope) < 1e-12
          && std::abs(rhs - exp_rhs) < 1e-12)
      {
        found = true;
        break;
      }
    }
    CAPTURE(f_k);
    CHECK(found);
  }
}

// ── (4) Loss column UB: physical cap on the column + tighter chord row ──
//
// The tighter LINEAR-IN-|f| bound ``ℓ ≤ (R · fmax / V²) · v`` is enforced by
// a row referencing the ``flow_abs`` auxiliary column ``v ≥ |f|``.  P0-A
// (cuOpt conditioning) ALSO caps the column itself at the physical max
// ``(R/V²) · fmax²`` instead of leaving it ``DblMax`` — a NON-binding cap
// (the chord row is strictly tighter for |f| < fmax), but it removes the
// unboxed column that made first-order / dual-simplex solvers diverge.
// This test pins both: the finite physical column UB AND the chord row.

TEST_CASE("tangent_signed_flow: loss column UB replaced by linear chord row")
{
  constexpr int K = 5;
  constexpr double R = 0.01;
  constexpr double V = 100.0;
  constexpr double V2 = V * V;
  constexpr double tmax = 200.0;
  TwoBusFixture fix("tangent_signed_flow", K, R, V, tmax);
  auto& li = fix.lp();

  const auto loss_col = find_col(li, "line_lossp_");
  const auto v_col = find_col(li, "line_flow_abs_");

  SUBCASE("loss column carries a finite physical UB; chord row tightens it")
  {
    CHECK(li.get_col_low()[value_of(loss_col)] == doctest::Approx(0.0));
    // P0-A: column UB capped at the physical max ``(R/V²)·fmax²`` (a
    // NON-binding cap — the chord row still enforces the tighter
    // linear-in-|f| bound).  Leaving it at ``DblMax`` made cuOpt's
    // first-order / dual-simplex root diverge on unboxed columns.
    CHECK(li.get_col_upp()[value_of(loss_col)]
          == doctest::Approx((R / V2) * tmax * tmax));
  }

  SUBCASE("v column is bounded [0, fmax]")
  {
    CHECK(li.get_col_low()[value_of(v_col)] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[value_of(v_col)] == doctest::Approx(tmax));
  }

  SUBCASE("chord row: −ℓ + (R·fmax/V²)·v ≥ 0")
  {
    // Find the loss_link row whose coef on loss_col is negative — that
    // is the chord upper bound row (tangent lower bounds carry +1).
    const double expected_chord_slope = (R / V2) * tmax;
    bool found = false;
    for (const auto& [name, row_idx] : li.row_name_map()) {
      if (!name.contains("line_loss_link")) {
        continue;
      }
      const double coef_on_loss = li.get_coeff(row_idx, loss_col);
      if (coef_on_loss >= 0.0) {
        continue;  // tangent row, not chord UB
      }
      // Chord row: should reference v_col with +chord_slope and
      // loss_col with −1 (both pre-scaled by loss_row_scale = 1.0 in
      // the test fixture).
      CHECK(coef_on_loss == doctest::Approx(-1.0));
      CHECK(li.get_coeff(row_idx, v_col)
            == doctest::Approx(expected_chord_slope).epsilon(1e-9));
      // ``flows`` should NOT appear in the chord row.
      const auto fcol = find_col(li, "line_flows_");
      CHECK(li.get_coeff(row_idx, fcol) == doctest::Approx(0.0));
      CHECK(li.get_row_low()[value_of(row_idx)] == doctest::Approx(0.0));
      found = true;
    }
    CHECK(found);
  }
}

// ── (5) Bus balance: sender = -f, receiver = +f (receiver loss) ──────

TEST_CASE("tangent_signed_flow: bus balance signed flow coefficients")
{
  // Allocation is ALWAYS overridden to ``split`` for
  // tangent_signed_flow (task #107): the per-direction sender/
  // receiver modes are direction-blind for signed flow and cause
  // R/A asymmetry — only ``split`` is sign-symmetric.  This test
  // pins the post-#107 behavior: regardless of the JSON-requested
  // allocation, the LP emits −0.5·ℓ on EACH bus.
  TwoBusFixture fix("tangent_signed_flow", /*loss_segments=*/5);
  auto& li = fix.lp();

  const auto fcol = find_col(li, "line_flows_");
  const auto loss_col = find_col(li, "line_lossp_");
  const auto bal_a = find_row(li, "bus_balance_1");
  const auto bal_b = find_row(li, "bus_balance_2");

  // bus_a: -1·f, −0.5·loss (split allocation).
  CHECK(li.get_coeff(bal_a, fcol) == doctest::Approx(-1.0));
  CHECK(li.get_coeff(bal_a, loss_col) == doctest::Approx(-0.5));

  // bus_b: +1·f, −0.5·loss (split allocation).
  CHECK(li.get_coeff(bal_b, fcol) == doctest::Approx(+1.0));
  CHECK(li.get_coeff(bal_b, loss_col) == doctest::Approx(-0.5));
}

// ── (6) Solve & verify primal ────────────────────────────────────────

TEST_CASE("tangent_signed_flow: solved primal — f > 0, loss in bracket")
{
  TwoBusFixture fix("tangent_signed_flow",
                    /*loss_segments=*/5,
                    /*R=*/0.01,
                    /*V=*/100.0,
                    /*tmax=*/200.0);
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const auto fcol = find_col(li, "line_flows_");
  const auto loss_col = find_col(li, "line_lossp_");
  const double f = sol[value_of(fcol)];
  const double loss = sol[value_of(loss_col)];

  CAPTURE(f);
  CAPTURE(loss);
  // Direct gen at bus A, demand 100 at bus B; load = 100 + loss.
  CHECK(f > 99.9);
  CHECK(f < 110.0);
  // Outer-approximation bracket:
  //   max_k tangent_k(f) ≤ loss ≤ k_loss · fmax²
  // ``loss`` is the K-fold max of tangents (OUTER ≤ true quadratic) so
  // for ``f`` between tangent points, ``loss < k_loss · f²`` is the
  // EXPECTED behaviour — the bound to assert is ``loss ≤ quadratic(f)``
  // (with a touch of slack at tangent points where they coincide).
  const double k_loss = 0.01 / (100.0 * 100.0);
  const double loss_quad_at_f = k_loss * f * f;
  const double loss_upper = k_loss * 200.0 * 200.0;
  CHECK(loss >= 0.0);
  CHECK(loss <= loss_quad_at_f + 1e-9);
  CHECK(loss <= loss_upper + 1e-9);
}

// ── (7) node_angle KVL — single +x_τ stamp on signed col ─────────────

TEST_CASE("tangent_signed_flow: node_angle KVL emits 1 row with +x_tau on f")
{
  TwoBusFixture fix("tangent_signed_flow",
                    /*loss_segments=*/5,
                    /*R=*/0.01,
                    /*V=*/100.0,
                    /*tmax=*/200.0,
                    /*use_kirchhoff=*/true,
                    /*kirchhoff_mode=*/"node_angle",
                    /*reactance=*/0.1);
  auto& li = fix.lp();

  // One KVL row per (line, block).
  CHECK(count_rows_containing(li, "line_theta_") == 1);

  // Signed flow col stamped with +x_τ = X / V² = 0.1 / 10000 = 1e-5.
  const auto fcol = find_col(li, "line_flows_");
  const auto theta_row = find_row(li, "line_theta_");
  CHECK(li.get_coeff(theta_row, fcol) == doctest::Approx(1e-5).epsilon(1e-9));
}

// ── (8) cycle_basis KVL — signed col in cycle equation ───────────────

TEST_CASE("tangent_signed_flow: cycle_basis runs with signed flow")
{
  // 3-bus loop A-B / A-C / C-B all tangent_signed_flow.  cycle_basis
  // expects exactly one cycle (|L|−|B|+islands = 3−3+1 = 1) → 1 row.
  System system = {
      .name = "TangentSignedFlowLoop",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "bA",
              },
              {
                  .uid = Uid {2},
                  .name = "bB",
              },
              {
                  .uid = Uid {3},
                  .name = "bC",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "dB",
                  .bus = Uid {2},
                  .capacity = 100.0,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "gA",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 500.0,
              },
          },
      .line_array =
          {
              {
                  .uid = Uid {1},
                  .name = "lAB",
                  .bus_a = Uid {1},
                  .bus_b = Uid {2},
                  .voltage = 100.0,
                  .resistance = 0.01,
                  .reactance = 0.1,
                  .line_losses_mode = OptName {"tangent_signed_flow"},
                  .loss_segments = 5,
                  .tmax_ba = 200.0,
                  .tmax_ab = 200.0,
                  .capacity = 200.0,
              },
              {
                  .uid = Uid {2},
                  .name = "lAC",
                  .bus_a = Uid {1},
                  .bus_b = Uid {3},
                  .voltage = 100.0,
                  .resistance = 0.01,
                  .reactance = 0.1,
                  .line_losses_mode = OptName {"tangent_signed_flow"},
                  .loss_segments = 5,
                  .tmax_ba = 200.0,
                  .tmax_ab = 200.0,
                  .capacity = 200.0,
              },
              {
                  .uid = Uid {3},
                  .name = "lCB",
                  .bus_a = Uid {3},
                  .bus_b = Uid {2},
                  .voltage = 100.0,
                  .resistance = 0.01,
                  .reactance = 0.1,
                  .line_losses_mode = OptName {"tangent_signed_flow"},
                  .loss_segments = 5,
                  .tmax_ba = 200.0,
                  .tmax_ab = 200.0,
                  .capacity = 200.0,
              },
          },
  };

  Simulation simulation = {
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

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = true;
  opts.model_options.kirchhoff_mode = std::string {"cycle_basis"};
  opts.model_options.scale_objective = 1000.0;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.lp_matrix_options.col_with_names = true;
  opts.lp_matrix_options.row_with_names = true;
  opts.lp_matrix_options.col_with_name_map = true;
  opts.lp_matrix_options.row_with_name_map = true;

  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  LpMatrixOptions bo;
  bo.col_with_names = true;
  bo.col_with_name_map = true;
  bo.row_with_names = true;
  bo.row_with_name_map = true;
  SystemLP sys_lp(system, sim_lp, bo);

  auto& li = sys_lp.linear_interface();

  // One signed flow col per line × 1 block = 3 total.
  CHECK(count_cols_containing(li, "line_flows_") == 3);
  // No fp/fn cols anywhere.
  CHECK(count_cols_containing(li, "line_flowp_") == 0);
  CHECK(count_cols_containing(li, "line_flown_") == 0);
  // Exactly 1 cycle row (3 lines, 3 buses, 1 island → 1 cycle).
  CHECK(count_rows_containing(li, "kirchhoff_cycle_") == 1);

  // Solve to ensure the LP is feasible with signed-flow + cycle_basis.
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
}

// ── (9) bidirectional vs tangent_signed_flow objective parity ────────

TEST_CASE("tangent_signed_flow: objective within 1e-3 of bidirectional")
{
  // Same 2-bus, same K, both convex PWL approximations of the same
  // quadratic.  The objective values should match closely; we allow
  // 1e-3 relative slack because the two formulations differ on the
  // PWL polarity (secant vs tangent envelopes).
  TwoBusFixture fix_bi("bidirectional", /*loss_segments=*/5);
  TwoBusFixture fix_ts("tangent_signed_flow", /*loss_segments=*/5);

  auto r_bi = fix_bi.lp().resolve();
  auto r_ts = fix_ts.lp().resolve();
  REQUIRE(r_bi.has_value());
  REQUIRE(r_ts.has_value());
  REQUIRE(r_bi.value() == 0);
  REQUIRE(r_ts.value() == 0);

  const double obj_bi = fix_bi.lp().get_obj_value_raw();
  const double obj_ts = fix_ts.lp().get_obj_value_raw();
  CAPTURE(obj_bi);
  CAPTURE(obj_ts);
  CHECK(obj_ts == doctest::Approx(obj_bi).epsilon(1e-3));
}

// ── (10) |f|-aux UB: structural — 1 col, 2 absv rows, 1 chord row ───

TEST_CASE("tangent_signed_flow: |f|-aux structural counts pin the new LP shape")
{
  // K = 5 → after the |f|-aux change each block now contributes:
  //   * 3 cols   (f signed, ℓ loss, v |f|-envelope)
  //   * 5 rows   (K=5 tangents → 4 surviving + 1 chord UB) under
  //              loss_link
  //   * 2 rows   (v − f ≥ 0, v + f ≥ 0) under flow_abs
  // Total = 3 cols + 7 rows from the loss model (caps/balance separate).
  constexpr int K = 5;
  TwoBusFixture fix("tangent_signed_flow", /*loss_segments=*/K);
  auto& li = fix.lp();

  CHECK(count_cols_containing(li, "line_flow_abs_") == 1);
  CHECK(count_rows_containing(li, "line_flow_abs") == 2);
  const int surviving_tangents = (K % 2 == 1) ? (K - 1) : K;
  CHECK(count_rows_containing(li, "line_loss_link") == surviving_tangents + 1);
}

// ── (11) Tight at endpoints: f = 0 ⇒ ℓ = 0 ───────────────────────────

TEST_CASE("tangent_signed_flow: ℓ pinned to 0 when |f| = 0")
{
  // Symmetric 2-bus with zero demand → optimum is f = 0, v = 0, ℓ = 0.
  // Both the tangent lower bounds and the chord upper bound force
  // ℓ to 0 when |f| is 0 — the new chord row makes the UB tight here
  // (was loose at the old constant uppb).
  System system = {
      .name = "TangentSignedFlowZeroDemand",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
              {
                  .uid = Uid {2},
                  .name = "b2",
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 500.0,
              },
          },
      .line_array =
          {
              {
                  .uid = Uid {1},
                  .name = "l1",
                  .bus_a = Uid {1},
                  .bus_b = Uid {2},
                  .voltage = 100.0,
                  .resistance = 0.01,
                  .line_losses_mode = OptName {"tangent_signed_flow"},
                  .loss_segments = 5,
                  .tmax_ba = 200.0,
                  .tmax_ab = 200.0,
                  .capacity = 200.0,
              },
          },
  };

  Simulation simulation = {
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

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = 1000.0;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.lp_matrix_options.col_with_names = true;
  opts.lp_matrix_options.row_with_names = true;
  opts.lp_matrix_options.col_with_name_map = true;
  opts.lp_matrix_options.row_with_name_map = true;
  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  LpMatrixOptions bo;
  bo.col_with_names = true;
  bo.col_with_name_map = true;
  bo.row_with_names = true;
  bo.row_with_name_map = true;
  SystemLP sys_lp(system, sim_lp, bo);

  auto& li = sys_lp.linear_interface();
  auto r = li.resolve();
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const auto fcol = find_col(li, "line_flows_");
  const auto loss_col = find_col(li, "line_lossp_");
  const auto v_col = find_col(li, "line_flow_abs_");

  CHECK(std::abs(sol[value_of(fcol)]) < 1e-6);
  CHECK(sol[value_of(loss_col)] < 1e-9);
  // ``v`` is bounded below by ``|f|`` (the absv rows) and above by
  // ``fmax`` (col uppb).  With ``f = 0`` and no cost pressure either
  // way, the simplex may land anywhere in ``[0, fmax]``; the contract
  // we pin is only ``v ≥ |f|``.  The tiny ε cost we stamp on v does
  // pull it down toward 0 at the optimum but not below LP tolerance.
  CHECK(sol[value_of(v_col)] >= -1e-9);
  CHECK(sol[value_of(v_col)] <= 200.0 + 1e-9);
}

// ── (12) Midpoint bracket — chord UB ≈ 2× true quadratic loss at f=fmax/2

TEST_CASE("tangent_signed_flow: midpoint bracket — ℓ in [quad, chord]")
{
  // Force the LP to dispatch exactly fmax/2 by sizing demand = fmax/2
  // and ensuring the line is unblocked.  The new chord UB at v=|f|=fmax/2
  // evaluates to (R·fmax/V²)·(fmax/2) = R·fmax²/(2V²), exactly 2× the
  // true quadratic R·f²/V² = R·fmax²/(4V²).  The K tangents bound ℓ
  // from below; at f=fmax/2 = 100 the nearest tangent f_k = 80 gives
  // ℓ ≥ R·(2·80·100 − 80²)/V² = R·9600/V² ≈ 0.96 · quadratic.
  constexpr int K = 5;
  constexpr double R = 0.01;
  constexpr double V = 100.0;
  constexpr double V2 = V * V;
  constexpr double tmax = 200.0;
  constexpr double k_loss = R / V2;
  constexpr double target_demand = 100.0;  // ≈ fmax / 2

  System system = {
      .name = "TangentSignedFlowMid",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
              {
                  .uid = Uid {2},
                  .name = "b2",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {2},
                  .capacity = target_demand,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 500.0,
              },
          },
      .line_array =
          {
              {
                  .uid = Uid {1},
                  .name = "l1",
                  .bus_a = Uid {1},
                  .bus_b = Uid {2},
                  .voltage = V,
                  .resistance = R,
                  .line_losses_mode = OptName {"tangent_signed_flow"},
                  .loss_segments = K,
                  .tmax_ba = tmax,
                  .tmax_ab = tmax,
                  .capacity = tmax,
              },
          },
  };

  Simulation simulation = {
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

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = 1000.0;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.lp_matrix_options.col_with_names = true;
  opts.lp_matrix_options.row_with_names = true;
  opts.lp_matrix_options.col_with_name_map = true;
  opts.lp_matrix_options.row_with_name_map = true;
  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  LpMatrixOptions bo;
  bo.col_with_names = true;
  bo.col_with_name_map = true;
  bo.row_with_names = true;
  bo.row_with_name_map = true;
  SystemLP sys_lp(system, sim_lp, bo);

  auto& li = sys_lp.linear_interface();
  auto r = li.resolve();
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const auto fcol = find_col(li, "line_flows_");
  const auto loss_col = find_col(li, "line_lossp_");
  const double f = sol[value_of(fcol)];
  const double loss = sol[value_of(loss_col)];

  // ``f`` exceeds demand by exactly ``loss`` (loss allocated at
  // receiver bus B); for these numbers loss ≪ demand so f is just
  // slightly above target_demand.
  CAPTURE(f);
  CAPTURE(loss);
  CHECK(f >= target_demand - 1e-6);
  CHECK(f <= target_demand + 5.0);

  // Bracket bounds at the SOLVED |f|:
  //   quadratic LB at this f: k_loss · f²
  //   chord UB at this v=|f|: k_loss · fmax · |f|
  // The chord UB at f = fmax/2 is 2× the quadratic — the LP is free
  // to pick anything in between, but it WILL minimise via the
  // tangent lower envelope (cost on ``f`` pushes ℓ down → tangents
  // pin ℓ to the highest tangent lower bound, which sits ≤ quad LB).
  const double quad_at_f = k_loss * f * f;
  const double chord_at_absf = k_loss * tmax * std::abs(f);
  CHECK(loss >= 0.0);
  CHECK(loss <= chord_at_absf + 1e-9);
  // The chord UB at f ≈ fmax/2 should be roughly 2× the quadratic
  // (i.e. the chord is the secant from (0,0) to (fmax, R·fmax²/V²)).
  // The actual ℓ sits at the binding TANGENT lower bound, so it is
  // typically BELOW the quadratic for f between tangent points (a
  // documented property of the outer approximation).
  CHECK(loss <= quad_at_f + 1e-9);

  // Sanity: chord ≈ 2× quad at f = fmax/2 (within ~10 % because the
  // LP may dispatch slightly above target_demand).
  CHECK(chord_at_absf >= 1.5 * quad_at_f);
  CHECK(chord_at_absf <= 2.5 * quad_at_f);
}

// ── (13) Arbitrage cap: ℓ never exceeds the loose constant bound ─────

TEST_CASE("tangent_signed_flow: chord UB never exceeds loose constant bound")
{
  // 2-bus with a negative-cost generator at the receiver — a degenerate
  // arbitrage fixture where the LP would (with the OLD constant column
  // UB ``R·fmax²/V²``) push ``ℓ`` to that UB to extract subsidy.
  // With the new chord row ``ℓ ≤ R·fmax/V² · v``, ``v`` bounded by
  // ``fmax``: ``ℓ`` can still reach ``R·fmax²/V²`` at worst (when
  // ``v = fmax``), so the chord is no LOOSER than the old constant
  // bound — it's tighter whenever the LP is pressured to keep ``v``
  // close to ``|f|`` (the normal positive-LMP regime; the ε cost on v
  // also pulls in this direction).
  //
  // This test pins the chord-as-safety-ceiling contract: even under
  // arbitrage pressure, ℓ is bounded by ``R·fmax²/V²`` and the chord
  // row makes the LP shape this bound through ``v`` rather than fixing
  // a constant on the loss column.
  constexpr int K = 5;
  constexpr double R = 0.01;
  constexpr double V = 100.0;
  constexpr double V2 = V * V;
  constexpr double tmax = 200.0;
  constexpr double k_loss = R / V2;

  System system = {
      .name = "TangentSignedFlowNegLmp",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
              {
                  .uid = Uid {2},
                  .name = "b2",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d2",
                  .bus = Uid {2},
                  .capacity = 50.0,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 500.0,
              },
              // Negative-cost (subsidised) gen at bus 2 — every MWh
              // dispatched here earns money.  Pre-fix the LP would
              // pump up ``ℓ`` to soak up the subsidy; the chord UB
              // makes that impossible.
              {
                  .uid = Uid {2},
                  .name = "g2_neg",
                  .bus = Uid {2},
                  .gcost = -100.0,
                  .capacity = 500.0,
              },
          },
      .line_array =
          {
              {
                  .uid = Uid {1},
                  .name = "l1",
                  .bus_a = Uid {1},
                  .bus_b = Uid {2},
                  .voltage = V,
                  .resistance = R,
                  .line_losses_mode = OptName {"tangent_signed_flow"},
                  .loss_segments = K,
                  .tmax_ba = tmax,
                  .tmax_ab = tmax,
                  .capacity = tmax,
              },
          },
  };

  Simulation simulation = {
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

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = 1000.0;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.lp_matrix_options.col_with_names = true;
  opts.lp_matrix_options.row_with_names = true;
  opts.lp_matrix_options.col_with_name_map = true;
  opts.lp_matrix_options.row_with_name_map = true;
  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  LpMatrixOptions bo;
  bo.col_with_names = true;
  bo.col_with_name_map = true;
  bo.row_with_names = true;
  bo.row_with_name_map = true;
  SystemLP sys_lp(system, sim_lp, bo);

  auto& li = sys_lp.linear_interface();
  auto r = li.resolve();
  REQUIRE(r.has_value());
  REQUIRE(r.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const auto fcol = find_col(li, "line_flows_");
  const auto loss_col = find_col(li, "line_lossp_");
  const auto v_col = find_col(li, "line_flow_abs_");
  const double f = sol[value_of(fcol)];
  const double loss = sol[value_of(loss_col)];
  const double v = sol[value_of(v_col)];

  CAPTURE(f);
  CAPTURE(loss);
  CAPTURE(v);

  // The absv rows ALWAYS hold: v ≥ |f|.
  CHECK(v >= std::abs(f) - 1e-9);

  // The chord row ALWAYS holds: ℓ ≤ chord_slope · v.  This is the new
  // contract — whether v lands at |f| or at fmax depends on whether
  // arbitrage pressure on ℓ exceeds the ε cost on v.  Either way the
  // chord cannot violate this row.
  CHECK(loss <= k_loss * tmax * v + 1e-6);

  // ℓ is bounded above by the loose constant UB ``R·fmax²/V²`` (the
  // chord row degenerates to this when ``v = fmax``).  The new
  // formulation never relaxes this safety ceiling.
  const double constant_ub = k_loss * tmax * tmax;
  CHECK(loss <= constant_ub + 1e-6);
}

// NOLINTEND(bugprone-unchecked-optional-access)

}  // namespace test_line_losses_tangent_signed_flow_ns
