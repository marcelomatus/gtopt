// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_mip_start_priorities.cpp
 * @brief     Opt-in seed-derived branching priorities
 *            (`mip_start.branch_priorities`).
 * @date      2026-07-14
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * When the MIP-start pipeline produced a candidate, `branch_priorities=true`
 * derives a per-integer-column branching priority from decisiveness —
 * `round(100 × |2·frac − 1|)` with `frac` the column's fractional part in
 * the Stage-0 LP relaxation — and installs it via the backend virtual
 * `set_branch_priorities` (CPLEX: `CPXcopyorder`; every other backend is a
 * benign no-op decline).  Strictly OPT-IN: per IBM, any priority order
 * switches CPLEX from dynamic search to traditional branch-and-cut.
 *
 * Fixture: the tiny 2-generator / 2-block UC from
 * test_mip_start_roundtrip.cpp (no coupling row) —
 *   u = (1,0,1,1), p = (50,0,50,40), obj = 3240.
 */

#include <array>
#include <span>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace mip_start_priorities_test  // NOLINT(misc-use-anonymous-namespace)
{
namespace
{

constexpr double kPrioOptObj = 3240.0;
constexpr int kPrioNumIntCols = 4;

struct PrioUc
{
  // column indices, blocks 1..2
  ColIndex ua1 {}, ub1 {}, pa1 {}, pb1 {};
  ColIndex ua2 {}, ub2 {}, pa2 {}, pb2 {};
};

/// The tiny UC: per block, u/p per generator, balance + cap + min rows.
PrioUc build_prio_uc(LinearInterface& lp)
{
  PrioUc m;

  auto add_block =
      [&lp](
          double demand, ColIndex& ua, ColIndex& ub, ColIndex& pa, ColIndex& pb)
  {
    ua = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 100.0,
    });
    ub = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 40.0,
    });
    pa = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 10.0,
    });
    pb = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 50.0,
    });
    lp.set_integer(ua);
    lp.set_integer(ub);

    SparseRow balance;
    balance[pa] = 1.0;
    balance[pb] = 1.0;
    balance.equal(demand);
    (void)lp.add_row(balance);

    SparseRow cap_a;
    cap_a[pa] = 1.0;
    cap_a[ua] = -60.0;
    cap_a.less_equal(0.0);
    (void)lp.add_row(cap_a);

    SparseRow min_a;
    min_a[pa] = 1.0;
    min_a[ua] = -20.0;
    min_a.greater_equal(0.0);
    (void)lp.add_row(min_a);

    SparseRow cap_b;
    cap_b[pb] = 1.0;
    cap_b[ub] = -100.0;
    cap_b.less_equal(0.0);
    (void)lp.add_row(cap_b);

    SparseRow min_b;
    min_b[pb] = 1.0;
    min_b[ub] = -40.0;
    min_b.greater_equal(0.0);
    (void)lp.add_row(min_b);
  };

  add_block(50.0, m.ua1, m.ub1, m.pa1, m.pb1);
  add_block(90.0, m.ua2, m.ub2, m.pa2, m.pb2);
  return m;
}

}  // namespace

TEST_CASE(  // NOLINT
    "mip_start branch_priorities - order installed and optimum unchanged per "
    "MIP plugin")
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }

  for (const auto& name : solvers) {
    CAPTURE(name);
    const bool ran = gtopt::solver_test::run_or_skip_license(
        [&]
        {
          LinearInterface lp(name);
          const auto m = build_prio_uc(lp);

          MipStartOptions ms;
          ms.enabled = true;
          ms.branch_priorities = true;
          const auto report =
              apply_mip_start(lp, SolverOptions {.log_level = 0}, ms);
          REQUIRE(report.has_value());
          CHECK(report->relaxation_solved);
          CHECK(report->relaxation_feasible);
          if (name == "cplex") {
            // The plugin RECEIVED the order (CPXcopyorder succeeded on all
            // integer columns) — the report counter is the acceptance proof.
            CHECK(report->branch_priorities_cols == kPrioNumIntCols);
          } else {
            // Every other backend declines via the default no-op virtual.
            CHECK(report->branch_priorities_cols == 0);
          }

          // With the priority order installed (CPLEX: traditional
          // branch-and-cut), the MIP still solves to the exact optimum.
          REQUIRE(lp.resolve(SolverOptions {.log_level = 0}).has_value());
          REQUIRE(lp.is_optimal());
          CHECK(lp.get_obj_value()
                == doctest::Approx(kPrioOptObj).epsilon(1e-6));
          const auto sol = lp.get_col_sol_raw();
          const auto at = [&sol](ColIndex c)
          { return sol[static_cast<std::size_t>(static_cast<int>(c))]; };
          CHECK(at(m.ua1) == doctest::Approx(1.0));
          CHECK(at(m.ub1) == doctest::Approx(0.0));
          CHECK(at(m.ua2) == doctest::Approx(1.0));
          CHECK(at(m.ub2) == doctest::Approx(1.0));
        });
    if (!ran) {
      MESSAGE("skipping ", name, " — license unavailable");
    }
  }
}

TEST_CASE(  // NOLINT
    "mip_start branch_priorities - off by default (no order installed)")
{
  const auto name = gtopt::solver_test::first_mip_solver();
  if (name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(name);

  LinearInterface lp(name);
  (void)build_prio_uc(lp);

  MipStartOptions ms;
  ms.enabled = true;  // branch_priorities left unset → default false
  const auto report = apply_mip_start(lp, SolverOptions {.log_level = 0}, ms);
  REQUIRE(report.has_value());
  CHECK(report->branch_priorities_cols == 0);
}

TEST_CASE(  // NOLINT
    "set_branch_priorities - LP-only backend (clp) declines")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("clp")) {
    MESSAGE("clp plugin not loaded — skipping LP-only decline test");
    return;
  }

  LinearInterface lp("clp");
  const auto x = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .cost = -1.0,
  });
  SparseRow row;
  row[x] = 1.0;
  row.less_equal(1.0);
  (void)lp.add_row(row);
  REQUIRE(lp.initial_solve(SolverOptions {}).has_value());

  // No branching on an LP-only backend: the default virtual declines, as
  // does the LinearInterface guard on malformed input.
  const std::array<int, 1> cols {static_cast<int>(x)};
  const std::array<int, 1> prios {100};
  CHECK_FALSE(lp.set_branch_priorities(cols, prios));
  CHECK_FALSE(lp.set_branch_priorities(cols, std::span<const int> {}));
  CHECK_FALSE(lp.set_branch_priorities(std::span<const int> {},
                                       std::span<const int> {}));
}

}  // namespace mip_start_priorities_test
