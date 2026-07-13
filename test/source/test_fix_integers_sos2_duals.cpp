// SPDX-License-Identifier: BSD-3-Clause
//
// test_fix_integers_sos2_duals.cpp — the MIP dual-recovery pass must
// preserve SOS2 feasibility in the PUBLISHED primal.
//
// Production bug (found on a CEN PCP case, CPLEX, UC binaries + SOS2
// λ-form line losses): after the MIP solve, `SystemLP::resolve` runs
// `fix_integers_and_resolve` to recover row duals.  SOS2 λ-member
// columns are CONTINUOUS, and an SOS declaration is inert in a pure LP
// re-solve — so a fixed LP that pins only the INTEGER columns leaves
// the λ ladder free to re-optimize.  Under a negative bus-dual
// pair-sum (the M2 KVL corridor of
// test_line_losses_negative_lmp_kvl.cpp) the freed λ mass migrates to
// NON-ADJACENT breakpoints (SOS2-infeasible) and the loss column
// re-inflates to its envelope ceiling.
// `populate_solution_cache_post_solve()` then publishes THAT primal —
// silently replacing the SOS2-feasible MIP incumbent in the output.
//
// Why the sibling SOS2 tests did not catch this:
//   * test_line_losses_sos2.cpp and the SOS2 guard in
//     test_line_losses_negative_lmp_kvl.cpp read the incumbent right
//     after `LinearInterface::resolve()` — the dual-recovery pass
//     (`fix_integers_and_resolve`) never runs on that path.
//   * Their fixtures carry no integer column, so even the production
//     `SystemLP::resolve` gate (`wants_duals && has_integer_cols()`)
//     would have skipped the pass.
//
// These tests go through the REAL output path:
//   1. A per-solver unit MIP (binary + SOS2 arbitrage lure) asserting
//      `fix_integers_and_resolve` publishes the incumbent, not the
//      SOS2-infeasible arbitrage vertex.
//   2. The 4-bus KVL ring + UC binary + SOS2 λ-form losses, solved via
//      `SystemLP::resolve` with duals requested — the exact production
//      trigger — asserting published λ adjacency and loss ≤ c·f² + tol.
//   3. The same ring WITHOUT integer columns, pinning the gate
//      extension: SOS2 sets alone must trigger dual recovery (a pure
//      SOS2 MIP carries no duals either).

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_fix_integers_sos2_duals_ns
{

namespace
{

constexpr double kV = 100.0;
constexpr double kR = 0.5;  ///< c = R/V² = 5e-5 per line
constexpr double kX = 10.0;
constexpr double kTmax = 1000.0;
constexpr double kTmaxLimited = 30.0;  ///< l43 binding limit
constexpr double kCLoss = kR / (kV * kV);  ///< 5e-5
constexpr int kL = 4;  ///< loss_secant_segments

/// SOS2 needs a MIP backend with native SOS2 (CPLEX in this env);
/// CBC/OSI/HiGHS/MindOpt throw at LP-build time.  Same guard as
/// test_line_losses_sos2.cpp::sos2_available.
[[nodiscard]] bool sos2_available()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  return reg.has_solver("cplex");
}

/// 4-bus KVL ring (test_line_losses_negative_lmp_kvl.cpp) with
/// tangent_signed_flow + L=4 + SOS2 λ-form losses on every line, an
/// optional UC binary on g1, and duals requested in the write-out
/// selection so `SystemLP::resolve` runs the fix-integers pass.
///
/// Lossless DC solution: g1 = 160, g3 = 40, l43 binding at 30 MW,
/// π1 = 10, π4 = −35 → the l14 pair-sum π1 + π4 = −25 < 0 is the
/// arbitrage corridor the freed λ ladder would exploit.
struct RingSos2DualsFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit RingSos2DualsFixture(bool with_commitment)
      : system {
            .name = "FixIntSos2Ring",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                    {.uid = Uid {3}, .name = "b3",},
                    {.uid = Uid {4}, .name = "b4",},
                },
            .demand_array =
                {
                    {.uid = Uid {1},
                     .name = "d2",
                     .bus = Uid {2},
                     .capacity = 200.0,},
                },
            .generator_array =
                {
                    {.uid = Uid {1},
                     .name = "g1",
                     .bus = Uid {1},
                     .gcost = 10.0,
                     .capacity = 1000.0,},
                    {.uid = Uid {3},
                     .name = "g3",
                     .bus = Uid {3},
                     .gcost = 100.0,
                     .capacity = 1000.0,},
                },
            .line_array =
                {
                    make_line(Uid {1}, "l12", Uid {1}, Uid {2}, kTmax),
                    make_line(Uid {2}, "l32", Uid {3}, Uid {2}, kTmax),
                    make_line(Uid {3}, "l43", Uid {4}, Uid {3}, kTmaxLimited),
                    make_line(Uid {4}, "l14", Uid {1}, Uid {4}, kTmax),
                },
            .commitment_array = make_commitments(with_commitment),
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
                  .chronological = true,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options())
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_matrix_opts())
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static Line make_line(
      Uid uid, std::string_view name, Uid bus_a, Uid bus_b, double tmax)
  {
    Line ln = {
        .uid = uid,
        .name = std::string(name),
        .bus_a = bus_a,
        .bus_b = bus_b,
        .voltage = kV,
        .resistance = kR,
        .reactance = kX,
        .line_losses_mode = OptName {std::string {"tangent_signed_flow"}},
        .loss_segments = 4,
        .tmax_ba = tmax,
        .tmax_ab = tmax,
        .capacity = tmax,
    };
    ln.loss_secant_segments = kL;
    ln.loss_use_sos2 = true;
    return ln;
  }

  static Array<Commitment> make_commitments(bool with_commitment)
  {
    if (!with_commitment) {
      return {};
    }
    return {
        {
            .uid = Uid {1},
            .name = "cmt_g1",
            .generator = Uid {1},
            .startup_cost = 100.0,
            .shutdown_cost = 50.0,
            .pmin = 30.0,
            .initial_status = 0.0,
        },
    };
  }

  PlanningOptionsLP make_options()
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = true;
    opts.model_options.kirchhoff_mode = std::string("cycle_basis");
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    // Request duals so `SystemLP::resolve` gates the fix-integers
    // dual-recovery pass ON — the production trigger.
    opts.write_out =
        OutputSelection {OutputFlags::solution | OutputFlags::dual};
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_matrix_opts()
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    auto& reg = SolverRegistry::instance();
    reg.load_all_plugins();
    if (reg.has_solver("cplex")) {
      bo.solver_name = "cplex";
    }
    return bo;
  }
};

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

/// Published λ ladder of line `uid` (2L+1 entries, breakpoint order).
[[nodiscard]] auto lambda_ladder(const LinearInterface& li,
                                 std::span<const double> sol,
                                 int uid) -> std::array<double, (2 * kL) + 1>
{
  constexpr int lambda_count = (2 * kL) + 1;
  const auto prefix = std::string {"line_flow_lambda_"} + std::to_string(uid)
      + std::string {"_"};
  std::array<double, lambda_count> lam {};
  int found = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (!name.contains(prefix)) {
      continue;
    }
    const auto last_underscore = name.find_last_of('_');
    if (last_underscore == std::string_view::npos) {
      continue;
    }
    const auto seg_str = name.substr(last_underscore + 1);
    int seg_idx {};
    const auto* first = seg_str.data();
    const auto* last = first + seg_str.size();  // NOLINT
    if (std::from_chars(first, last, seg_idx).ec != std::errc {}) {
      continue;
    }
    if (seg_idx >= 0 && seg_idx < lambda_count) {
      lam[static_cast<std::size_t>(seg_idx)] =
          sol[static_cast<std::size_t>(value_of(idx))];
      ++found;
    }
  }
  REQUIRE(found == lambda_count);
  return lam;
}

/// SOS2 invariant (Beale & Tomlin 1970): at most TWO non-zero entries,
/// and when two, they must be ADJACENT.
void check_sos2_adjacency(std::span<const double> lam)
{
  std::vector<int> nonzero;
  for (int l = 0; l < static_cast<int>(lam.size()); ++l) {
    if (lam[static_cast<std::size_t>(l)] > 1e-6) {
      nonzero.push_back(l);
    }
  }
  CHECK(nonzero.size() <= 2);
  if (nonzero.size() == 2) {
    CHECK(nonzero[1] == nonzero[0] + 1);
  }
}

}  // namespace
}  // namespace test_fix_integers_sos2_duals_ns

using test_fix_integers_sos2_duals_ns::check_sos2_adjacency;
using test_fix_integers_sos2_duals_ns::find_col;
using test_fix_integers_sos2_duals_ns::find_row;
using test_fix_integers_sos2_duals_ns::kCLoss;
using test_fix_integers_sos2_duals_ns::kL;
using test_fix_integers_sos2_duals_ns::kTmax;
using test_fix_integers_sos2_duals_ns::lambda_ladder;
using test_fix_integers_sos2_duals_ns::RingSos2DualsFixture;
using test_fix_integers_sos2_duals_ns::sos2_available;

// ═══════════════════════════════════════════════════════════════════════════
// 1. Per-solver unit MIP: the fixed LP must NOT free the SOS2 λ ladder
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "fix_integers_and_resolve pins SOS2 members — published primal is the "
    "incumbent, not the arbitrage vertex")  // NOLINT
{
  // Minimal λ-form arbitrage lure:
  //
  //   cols: x ∈ [1,1]             (flow, pinned at breakpoint b=1)
  //         λ0,λ1,λ2 ∈ [0,1]      (SOS2, breakpoints b = {0, 1, 2})
  //         y ∈ [0,10]  cost −1   (loss — wants to inflate)
  //         u ∈ {0,1}   cost 0.5  (binary, gates y on)
  //   rows: conv : λ0+λ1+λ2            = 1
  //         flow : 0λ0+1λ1+2λ2 − x     = 0
  //         chord: 0λ0+1λ1+4λ2 − y     ≥ 0     (y ≤ Σ b² λ)
  //         gate : y − 10u             ≤ 0
  //
  //   MIP optimum (unique): adjacency ⇒ λ = (0,1,0) ⇒ y = 1, u = 1,
  //   obj = −0.5 (u = 0 forces y = 0, obj = 0 — strictly worse).
  //   If dual recovery frees the λs: λ0 = λ2 = 0.5 keeps flow = 1 but
  //   lifts the chord ceiling to 2 ⇒ y = 2 (SOS2-INFEASIBLE vertex,
  //   obj −1.5) — exactly the production loss-inflation signature.
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  bool tested_any = false;
  for (const auto& name : reg.available_solvers()) {
    if (!reg.supports_mip(name)) {
      continue;
    }
    CAPTURE(name);

    LinearProblem lp("sos2_fix_duals");
    const auto x = lp.add_col(SparseCol {
        .lowb = 1.0,
        .uppb = 1.0,
    });
    const auto l0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
    });
    const auto l1 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
    });
    const auto l2 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
    });
    const auto y = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = -1.0,
    });
    const auto u = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 0.5,
        .is_integer = true,
    });

    auto conv = SparseRow {};
    conv[l0] = 1.0;
    conv[l1] = 1.0;
    conv[l2] = 1.0;
    conv.equal(1.0);
    std::ignore = lp.add_row(std::move(conv));

    auto flow = SparseRow {};
    flow[l1] = 1.0;
    flow[l2] = 2.0;
    flow[x] = -1.0;
    flow.equal(0.0);
    std::ignore = lp.add_row(std::move(flow));

    auto chord = SparseRow {};
    chord[l1] = 1.0;
    chord[l2] = 4.0;
    chord[y] = -1.0;
    chord.greater_equal(0.0);
    const auto chord_row = lp.add_row(std::move(chord));

    auto gate = SparseRow {};
    gate[y] = 1.0;
    gate[u] = -10.0;
    gate.less_equal(0.0);
    std::ignore = lp.add_row(std::move(gate));

    lp.add_sos2({l0, l1, l2});

    const auto flat = lp.flatten({
        .equilibration_method = LpEquilibrationMethod::none,
    });

    // Backends without native SOS2 throw at load time — skip those.
    std::optional<LinearInterface> li;
    try {
      li.emplace(name, flat);
    } catch (const std::exception&) {
      MESSAGE("solver ", name, " has no SOS2 support — skipped");
      continue;
    }
    tested_any = true;

    REQUIRE(li->initial_solve({}).has_value());
    REQUIRE(li->is_optimal());
    REQUIRE(li->sos2_set_count() == 1);
    REQUIRE(li->sos2_member_cols().size() == 3);

    // MIP incumbent: SOS2-feasible, y = 1.
    {
      const auto sol = li->get_col_sol();
      CHECK(sol[y] == doctest::Approx(1.0).epsilon(1e-6));
      CHECK(sol[l1] == doctest::Approx(1.0).epsilon(1e-6));
      CHECK(sol[u] == doctest::Approx(1.0).epsilon(1e-6));
    }

    // ── The real output path: dual recovery ──
    auto fix = li->fix_integers_and_resolve({});
    REQUIRE(fix.has_value());
    // 1 binary + 3 SOS2 members pinned.
    CHECK(fix->fixed_columns == 4);
    REQUIRE(fix->status.has_value());
    CHECK(*fix->status == 0);
    REQUIRE(li->is_optimal());

    // PUBLISHED primal must still be the incumbent: y = 1 and the λ
    // ladder SOS2-feasible.  y = 2 with λ = (0.5, 0, 0.5) is the
    // regression (fixed LP freed the SOS members).
    {
      const auto sol = li->get_col_sol();
      CHECK(sol[y] == doctest::Approx(1.0).epsilon(1e-6));
      const std::array<double, 3> lam {
          sol[l0],
          sol[l1],
          sol[l2],
      };
      check_sos2_adjacency(lam);
      CHECK(sol[l1] == doctest::Approx(1.0).epsilon(1e-6));
    }

    // Duals are well-defined on the fixed LP.
    const auto duals = li->get_row_dual();
    REQUIRE(duals.size() == 4);
    CHECK(std::isfinite(duals[chord_row]));
  }

  if (!tested_any) {
    MESSAGE("no SOS2-capable MIP plugin loaded — skipping");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Production publish path: ring + UC binary + SOS2 through SystemLP
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "SystemLP::resolve + duals: published SOS2 loss stays ≤ c·f² + tol on "
    "the negative-LMP corridor (UC binary present)")  // NOLINT
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 regression — no SOS2-capable backend loaded");
    return;
  }

  RingSos2DualsFixture fix(/*with_commitment=*/true);
  auto& li = fix.lp();
  REQUIRE(li.sos2_set_count() == 4);  // one per (line, block)
  REQUIRE(li.has_integer_cols());  // the UC binary — production shape

  // The REAL path: MIP solve + fix-integers dual-recovery pass (gated on
  // by the `write_out` dual request in the fixture options).
  const auto result = fix.sys_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  REQUIRE(li.is_optimal());

  // Dual recovery ran: integrality is relaxed on the fixed LP.
  CHECK(!li.has_integer_cols());

  const auto sol = li.get_col_sol_raw();

  // Published λ ladder on the arbitrage corridor l14 (uid 4) is still
  // SOS2-feasible — the incumbent's, not the freed-λ arbitrage vertex.
  const auto lam = lambda_ladder(li, sol, /*uid=*/4);
  check_sos2_adjacency(lam);

  // Published loss on l14 obeys the SOS2 secant bracket: with the λs
  // pinned, ℓ can reach at most the secant of the incumbent's segment —
  // c·f² + c·w²/4 — never the c·env² = 50 MW envelope ceiling of the
  // production defect.
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(loss14);
  const double w = kTmax / static_cast<double>(kL);
  const double bracket_slack = kCLoss * w * w / 4.0;
  CHECK(std::abs(f14) > 10.0);
  CHECK(loss14 <= (kCLoss * f14 * f14) + bracket_slack + 1e-4);
  CHECK(loss14 < 1.0);

  // The recovered duals show the M2 pattern that fuels the arbitrage:
  // π1 > 0 pinned by g1, π4 with the OPPOSITE sign (negative LMP), so
  // the l14 pair-sum is negative — the exact regime under which the
  // unpinned fixed LP used to inflate the loss.
  const auto duals = li.get_row_dual_raw();
  const double pi1 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_1_")))];
  const double pi4 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_4_")))];
  CAPTURE(pi1);
  CAPTURE(pi4);
  REQUIRE(std::abs(pi1) > 1e-6);
  CHECK(pi4 * pi1 < 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Gate extension: SOS2 sets alone (no integer column) trigger recovery
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "SystemLP::resolve + duals: SOS2-only MIP (no integer cols) also runs "
    "dual recovery and publishes duals")  // NOLINT
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 gate test — no SOS2-capable backend loaded");
    return;
  }

  RingSos2DualsFixture fix(/*with_commitment=*/false);
  auto& li = fix.lp();
  REQUIRE(li.sos2_set_count() == 4);
  REQUIRE(!li.has_integer_cols());  // SOS2 is the ONLY discrete structure

  const auto result = fix.sys_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  REQUIRE(li.is_optimal());

  const auto sol = li.get_col_sol_raw();

  // Published primal keeps the SOS2 guarantees.
  const auto lam = lambda_ladder(li, sol, /*uid=*/4);
  check_sos2_adjacency(lam);

  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(loss14);
  const double w = kTmax / static_cast<double>(kL);
  const double bracket_slack = kCLoss * w * w / 4.0;
  CHECK(std::abs(f14) > 10.0);
  CHECK(loss14 <= (kCLoss * f14 * f14) + bracket_slack + 1e-4);

  // The headline of the gate extension: a pure SOS2 MIP used to skip
  // dual recovery entirely (`has_integer_cols()` is false) and publish
  // NO duals.  Now the balance duals must be populated and show the M2
  // sign pattern.
  const auto duals = li.get_row_dual_raw();
  REQUIRE(duals.size() == static_cast<std::size_t>(li.get_numrows()));
  const double pi1 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_1_")))];
  const double pi4 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_4_")))];
  CAPTURE(pi1);
  CAPTURE(pi4);
  REQUIRE(std::abs(pi1) > 1e-6);
  CHECK(pi4 * pi1 < 0.0);
}
