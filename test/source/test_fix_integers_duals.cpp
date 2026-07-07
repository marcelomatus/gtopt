/**
 * @file      test_fix_integers_duals.cpp
 * @brief     Tests for the MIP fix-integers dual-recovery pass
 * @date      2026-05-25
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * A MIP solution has no LP duals (shadow prices / reduced costs).  The
 * fix-integers pass (`LinearInterface::fix_integers_and_resolve`) pins
 * every integer column to its rounded MIP-optimal value, relaxes
 * integrality, and re-solves the resulting pure LP — recovering the
 * row duals (bus LMPs) and column reduced costs given the committed
 * integer solution.  These tests build small MIPs, solve them, and
 * verify the recovered duals are populated and economically sensible.
 *
 * The integrated path (`SystemLP::resolve` running the pass when the
 * write-out flags request duals) is also exercised on the commitment
 * MIP fixture so the end-to-end gating is covered.
 */

#include <cmath>
#include <ranges>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/bus_lp.hpp>
#include <gtopt/commitment.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-throwing-static-initialization,
// bugprone-unchecked-optional-access,cert-err58-cpp)

namespace
{

/// First loaded solver that supports MIP, or empty when none.
[[nodiscard]] std::string first_mip_solver()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      return name;
    }
  }
  return {};
}

/// True when @p solver recovers valid LP duals from the fix-integers pass on
/// these fixtures.  Excludes the two backends that don't:
///  - `cbc`: OsiCbc returns the LP-relaxation value instead of the MIP optimum
///    here (`u` gates `gen` only via a ≤ row), so the "fixed" LP is not at the
///    integer optimum and its duals are meaningless.
///  - `gurobi`: an UNLICENSED gurobi plugin can slip past plugin validation in
///    some build/plugin-set combinations and then solves to a degenerate,
///    zero-dual result.  Licensed gurobi is not a practical gtopt MIP target
///    (CPLEX / HiGHS / SCIP are), so skip rather than assert on it.
/// The practical, tested MIP backends (cplex, highs, scip) all pass.
[[nodiscard]] bool mip_solver_recovers_duals(const std::string& solver)
{
  return solver != "cbc" && solver != "gurobi";
}

const Array<Bus> single_bus = {
    {
        .uid = Uid {1},
        .name = "b1",
    },
};

const Simulation three_block_simulation = {
    .block_array =
        {
            {
                .uid = Uid {0},
                .duration = 1.0,
            },
            {
                .uid = Uid {1},
                .duration = 1.0,
            },
            {
                .uid = Uid {2},
                .duration = 1.0,
            },
        },
    .stage_array =
        {
            {
                .uid = Uid {0},
                .first_block = 0,
                .count_block = 3,
                .chronological = true,
            },
        },
    .scenario_array =
        {
            {
                .uid = Uid {0},
            },
        },
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1. Direct LinearInterface MIP: fix-integers recovers a sensible balance dual
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("fix_integers_and_resolve recovers duals on a small commitment MIP")
{
  const auto solver = first_mip_solver();
  if (solver.empty()) {
    MESSAGE("No MIP-capable solver available — skipping");
    return;
  }
  CAPTURE(solver);
  if (!mip_solver_recovers_duals(solver)) {
    MESSAGE(
        "Skipping — this backend does not recover fix-integers duals on this "
        "fixture (cbc LP-relaxation quirk / unlicensed gurobi); the practical "
        "gtopt MIP backends are CPLEX / HiGHS / SCIP");
    return;
  }

  // Tiny unit-commitment MIP:
  //   load  ∈ [0, 60]            cost 0        (must serve 60)
  //   gen   ∈ [0, 100]           cost 50       (only if committed)
  //   u     ∈ {0, 1}             cost 100      (binary commit + fixed cost)
  //   fail  ∈ [0, inf]           cost 1000     (unserved-energy penalty)
  //
  //   cap_row     : load + fail        = 60      (demand)
  //   balance_row : gen - load         = 0       (bus balance)
  //   commit_row  : gen - 100*u       <= 0       (gen needs commit)
  //
  // MIP optimum: u = 1, gen = 60, load = 60, fail = 0.
  // After fixing u = 1 and relaxing, the LP balance dual (LMP) is the
  // marginal cost of serving one more unit = gen's gcost = 50.
  constexpr double lmax = 60.0;
  constexpr double gcost = 50.0;

  LinearProblem lp("commit_mip");

  const auto load_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = lmax,
      .cost = 0.0,
  });
  const auto gen_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = gcost,
  });
  const auto fail_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = 1000.0,
  });
  const auto commit_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .cost = 100.0,
      .is_integer = true,
  });

  auto cap_row = SparseRow {};
  cap_row[load_col] = 1.0;
  cap_row[fail_col] = 1.0;
  cap_row.equal(lmax);
  std::ignore = lp.add_row(std::move(cap_row));

  auto balance = SparseRow {};
  balance[gen_col] = 1.0;
  balance[load_col] = -1.0;
  balance.equal(0.0);
  const auto balance_row = lp.add_row(std::move(balance));

  auto commit = SparseRow {};
  commit[gen_col] = 1.0;
  commit[commit_col] = -100.0;
  commit.less_equal(0.0);
  std::ignore = lp.add_row(std::move(commit));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::none,
  });
  LinearInterface li(solver, flat);

  // ── MIP solve ──
  auto mip = li.initial_solve({});
  REQUIRE(mip.has_value());
  REQUIRE(li.is_optimal());

  CHECK(li.has_integer_cols());
  CHECK(li.is_integer(commit_col));

  // The MIP commits the generator and serves all demand.
  {
    const auto sol = li.get_col_sol();
    CHECK(sol[commit_col] == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(sol[gen_col] == doctest::Approx(60.0).epsilon(1e-6));
    CHECK(sol[fail_col] == doctest::Approx(0.0).epsilon(1e-6));
  }

  // ── Fix-integers dual-recovery pass ──
  auto fix = li.fix_integers_and_resolve({});
  REQUIRE(fix.has_value());
  CHECK(fix->fixed_columns == 1);
  REQUIRE(fix->status.has_value());
  CHECK(*fix->status == 0);
  REQUIRE(li.is_optimal());

  // After fixing, the integer column is continuous.
  CHECK(!li.has_integer_cols());
  CHECK(!li.is_integer(commit_col));

  // Primal is unchanged (integers pinned to their MIP values).
  {
    const auto sol = li.get_col_sol();
    CHECK(sol[commit_col] == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(sol[gen_col] == doctest::Approx(60.0).epsilon(1e-6));
  }

  // Duals are now well-defined.  The bus-balance LMP equals the marginal
  // generator's cost (50): serving one more MW of load costs gcost.
  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 3);
  CHECK(duals[balance_row] != doctest::Approx(0.0));
  CHECK(std::abs(duals[balance_row]) == doctest::Approx(gcost).epsilon(1e-4));
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. No-op on a pure LP (no integer columns)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("fix_integers_and_resolve is a no-op on a pure LP")
{
  LinearProblem lp("pure_lp");
  const auto x = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
  });
  auto row = SparseRow {};
  row[x] = 1.0;
  row.greater_equal(3.0);
  std::ignore = lp.add_row(std::move(row));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::none,
  });
  // Empty solver name → auto-detected default backend (LP-capable).
  LinearInterface li("", flat);
  REQUIRE(li.initial_solve({}).has_value());
  REQUIRE(li.is_optimal());

  CHECK(!li.has_integer_cols());

  auto fix = li.fix_integers_and_resolve({});
  REQUIRE(fix.has_value());
  CHECK(fix->fixed_columns == 0);
  CHECK(!fix->status.has_value());

  // Pure-LP duals are untouched and still valid.
  REQUIRE(li.is_optimal());
  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 1);
  CHECK(duals[RowIndex {0}] == doctest::Approx(1.0).epsilon(1e-6));
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Integrated SystemLP path: resolve() recovers duals when write-out asks
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SystemLP::resolve runs fix-integers pass and populates bus balance duals")
{
  const auto solver = first_mip_solver();
  if (solver.empty()) {
    MESSAGE("No MIP-capable solver available — skipping");
    return;
  }
  CAPTURE(solver);
  if (!mip_solver_recovers_duals(solver)) {
    MESSAGE(
        "Skipping — this backend does not recover fix-integers duals on this "
        "fixture (cbc / unlicensed gurobi); use CPLEX / HiGHS / SCIP");
    return;
  }

  System system;
  system.name = "FixIntDuals_" + solver;
  system.bus_array = single_bus;
  system.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = TBRealFieldSched {std::vector<std::vector<Real>> {
              {
                  0.0,
                  60.0,
                  60.0,
              },
          }},
          .capacity = 100.0,
      },
  };
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "cmt1",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .shutdown_cost = 50.0,
          .pmin = 30.0,
          .initial_status = 0.0,
      },
  };

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  // Request duals in the write-out selection so the fix-integers pass is
  // gated ON inside SystemLP::resolve.
  poptions.write_out =
      OutputSelection {OutputFlags::solution | OutputFlags::dual};
  poptions.lp_matrix_options.solver_name = solver;
  PlanningOptionsLP options(std::move(poptions));

  SimulationLP simulation_lp(three_block_simulation, options);

  LpMatrixOptions flat_opts;
  flat_opts.solver_name = solver;
  SystemLP system_lp(system, simulation_lp, flat_opts);

  auto& lp = system_lp.linear_interface();
  REQUIRE(lp.has_integer_cols());

  // SystemLP::resolve: MIP solve + fix-integers dual recovery pass.
  auto result = system_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  REQUIRE(lp.is_optimal());

  // Integrality has been relaxed by the dual-recovery pass.
  CHECK(!lp.has_integer_cols());

  // The bus balance rows for the committed blocks (1 and 2, demand 60)
  // must carry a nonzero LMP equal to the generator's marginal cost (50).
  const auto& bus_lps = system_lp.elements<BusLP>();
  REQUIRE(bus_lps.size() == 1);
  const auto& bus_lp = bus_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& balance_rows = bus_lp.balance_rows_at(scenario_lp, stage_lp);

  const auto duals = lp.get_row_dual();

  // Block 1 (uid 1) and block 2 (uid 2) have demand 60 → committed,
  // marginal cost = gcost = 50.  Block 0 (uid 0) has zero demand.
  bool any_balance_dual_nonzero = false;
  for (const auto& row : balance_rows | std::views::values) {
    const double d = duals[row];
    if (std::abs(d) > 1e-6) {
      any_balance_dual_nonzero = true;
      // LMP magnitude must equal the marginal generator cost.
      CHECK(std::abs(d) == doctest::Approx(50.0).epsilon(1e-3));
    }
  }
  CHECK(any_balance_dual_nonzero);
}

// NOLINTEND(bugprone-throwing-static-initialization,
// bugprone-unchecked-optional-access,cert-err58-cpp)
