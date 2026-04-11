// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_benders_cut.cpp
 * @brief     Unit tests for Benders cut construction functions
 * @date      2026-04-05
 */

#include <cmath>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/validate_planning.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── Free-function unit tests ───────────────────────────────────────────────

TEST_CASE("build_benders_cut produces valid cut row")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};
  const auto dep = ColIndex {2};

  std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  // reduced costs: dep column has rc = -10.0
  const std::vector<double> rc = {0.0, 0.0, -10.0};
  const double obj = 5000.0;

  auto row = build_benders_cut(alpha, links, rc, obj);

  // α coefficient = 1.0
  CHECK(row.get_coeff(alpha) == doctest::Approx(1.0));
  // source coefficient = -rc = -(-10) = 10
  CHECK(row.get_coeff(src) == doctest::Approx(10.0));
  // rhs = obj - Σ rc_i * trial_i = 5000 - (-10)*50 = 5500
  CHECK(row.lowb == doctest::Approx(5500.0));
  CHECK(row.uppb > 1e20);
}

TEST_CASE("relax_fixed_state_variable respects source bounds")  // NOLINT
{
  LinearInterface li;

  // Create a column and fix it at 80.0
  const auto col = li.add_col(SparseCol {
      .lowb = 80.0,
      .uppb = 80.0,
      .name = "dep",
  });

  const StateVarLink link {
      .dependent_col = col,
      .source_phase_index = first_phase_index(),
      .trial_value = 80.0,
      .source_low = 0.0,
      .source_upp = 150.0,
  };

  const auto relaxed =
      relax_fixed_state_variable(li, link, PhaseIndex {1}, 1e6);
  CHECK(relaxed);

  // After relaxation, bounds should match source bounds
  CHECK(li.get_col_low()[col] == doctest::Approx(0.0));
  CHECK(li.get_col_upp()[col] == doctest::Approx(150.0));
}

TEST_CASE("relax_fixed_state_variable skips non-fixed columns")  // NOLINT
{
  LinearInterface li;
  const auto col = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "dep",
  });

  const StateVarLink link {
      .dependent_col = col,
      .trial_value = 50.0,
      .source_low = 0.0,
      .source_upp = 100.0,
  };

  CHECK_FALSE(relax_fixed_state_variable(li, link, PhaseIndex {1}, 1e6));
}

TEST_CASE("average_benders_cut computes correct average")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  // name field removed from SparseRow
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  // name field removed from SparseRow
  cut2[alpha] = 1.0;
  cut2[src] = 20.0;
  cut2.lowb = 200.0;
  cut2.uppb = LinearProblem::DblMax;

  auto avg = average_benders_cut({
      cut1,
      cut2,
  });

  CHECK(avg.get_coeff(alpha) == doctest::Approx(1.0));
  CHECK(avg.get_coeff(src) == doctest::Approx(15.0));
  CHECK(avg.lowb == doctest::Approx(150.0));
}

TEST_CASE("relax_fixed_state_variable returns slack column indices")  // NOLINT
{
  LinearInterface li;

  // Create a column and fix it at 50.0
  const auto col = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  const StateVarLink link {
      .dependent_col = col,
      .source_phase_index = first_phase_index(),
      .trial_value = 50.0,
      .source_low = 0.0,
      .source_upp = 100.0,
  };

  const auto info = relax_fixed_state_variable(li, link, PhaseIndex {1}, 1e6);

  REQUIRE(info.relaxed);
  // After relaxation, bounds should match source bounds
  CHECK(li.get_col_low()[col] == doctest::Approx(0.0));
  CHECK(li.get_col_upp()[col] == doctest::Approx(100.0));
  // slack columns must be valid
  CHECK(info.sup_col != ColIndex {unknown_index});
  CHECK(info.sdn_col != ColIndex {unknown_index});
  CHECK(info.sup_col != info.sdn_col);
}

// ─── weighted_average_benders_cut unit tests ─────────────────────────────────

TEST_CASE("weighted_average_benders_cut - empty input")  // NOLINT
{
  const auto result = weighted_average_benders_cut({}, {});
  CHECK(result.cmap.empty());
}

TEST_CASE("weighted_average_benders_cut - single cut")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  // name field removed from SparseRow
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  const auto result = weighted_average_benders_cut({cut1}, {0.7});
  // single cut → returned as-is (weight normalised to 1)
  CHECK(result.get_coeff(alpha) == doctest::Approx(1.0));
  CHECK(result.get_coeff(src) == doctest::Approx(10.0));
  CHECK(result.lowb == doctest::Approx(100.0));
}

TEST_CASE(
    "weighted_average_benders_cut - equal weights same as average")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[alpha] = 1.0;
  cut2[src] = 20.0;
  cut2.lowb = 200.0;
  cut2.uppb = LinearProblem::DblMax;

  // Equal weights → same as unweighted average
  const auto wavg = weighted_average_benders_cut({cut1, cut2}, {0.5, 0.5});
  const auto avg = average_benders_cut({cut1, cut2});

  CHECK(wavg.get_coeff(alpha) == doctest::Approx(avg.get_coeff(alpha)));
  CHECK(wavg.get_coeff(src) == doctest::Approx(avg.get_coeff(src)));
  CHECK(wavg.lowb == doctest::Approx(avg.lowb));
}

TEST_CASE(
    "weighted_average_benders_cut - probability weights applied")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[alpha] = 1.0;
  cut2[src] = 30.0;
  cut2.lowb = 300.0;
  cut2.uppb = LinearProblem::DblMax;

  // 75% weight on cut1, 25% weight on cut2
  const auto result = weighted_average_benders_cut({cut1, cut2}, {0.75, 0.25});

  CHECK(result.get_coeff(alpha) == doctest::Approx(1.0));
  // expected: 0.75 * 10 + 0.25 * 30 = 7.5 + 7.5 = 15.0
  CHECK(result.get_coeff(src) == doctest::Approx(15.0));
  // expected: 0.75 * 100 + 0.25 * 300 = 75 + 75 = 150
  CHECK(result.lowb == doctest::Approx(150.0));
}

TEST_CASE("weighted_average_benders_cut - unnormalised weights")  // NOLINT
{
  const auto src = ColIndex {0};

  SparseRow cut1;
  cut1[src] = 4.0;
  cut1.lowb = 40.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[src] = 8.0;
  cut2.lowb = 80.0;
  cut2.uppb = LinearProblem::DblMax;

  // weights {3, 1} → normalised: {0.75, 0.25}
  const auto result = weighted_average_benders_cut({cut1, cut2}, {3.0, 1.0});

  // expected src: 0.75 * 4 + 0.25 * 8 = 3 + 2 = 5
  CHECK(result.get_coeff(src) == doctest::Approx(5.0));
  // expected rhs: 0.75 * 40 + 0.25 * 80 = 30 + 20 = 50
  CHECK(result.lowb == doctest::Approx(50.0));
}

TEST_CASE(
    "weighted_average_benders_cut - zero weight scene excluded")  // NOLINT
{
  const auto src = ColIndex {0};

  SparseRow cut1;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[src] = 20.0;
  cut2.lowb = 200.0;
  cut2.uppb = LinearProblem::DblMax;

  // Zero weight on cut2 → only cut1 contributes
  const auto result = weighted_average_benders_cut({cut1, cut2}, {1.0, 0.0});

  CHECK(result.get_coeff(src) == doctest::Approx(10.0));
  CHECK(result.lowb == doctest::Approx(100.0));
}

TEST_CASE(
    "weighted_average_benders_cut - all zero weights returns empty")  // NOLINT
{
  const auto src = ColIndex {0};

  SparseRow cut1;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  // All zero weights → empty result
  const auto result = weighted_average_benders_cut({cut1}, {0.0});
  CHECK(result.cmap.empty());
  CHECK(result.cmap.empty());
  CHECK(result.lowb == doctest::Approx(0.0));
}

// ─── Modular Benders cut tests (benders_cut.hpp) ────────────────────────────
//
// These tests exercise the cut-creation functions against actual LP solves
// using simple 2-variable LP problems.  They do not depend on SDDPMethod.

TEST_CASE(  // NOLINT
    "build_benders_cut - optimality cut from LP solve")
{
  // Build a simple LP:
  //   min  10*x0 + 20*x1 + alpha
  //   s.t. x0 + x1 + dep >= 100   (demand)
  //        0 <= x0 <= 80
  //        0 <= x1 <= 80
  //        dep fixed at 50

  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 80.0,
      .name = "x0",
  });
  const auto x1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 80.0,
      .name = "x1",
  });
  li.set_obj_coeff(x0, 10.0);
  li.set_obj_coeff(x1, 20.0);

  // Alpha (future cost)
  const auto alpha_col = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1e12,
      .name = "alpha",
  });
  li.set_obj_coeff(alpha_col, 1.0);

  // Dependent (state variable from previous phase)
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  // demand: x0 + x1 + dep >= 100
  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[x1] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);

  auto r = li.resolve({});
  REQUIRE(r.has_value());
  REQUIRE(li.is_optimal());

  const auto src = ColIndex {20};  // arbitrary source col index

  std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  auto cut = build_benders_cut(
      alpha_col, links, li.get_col_cost_raw(), li.get_obj_value());
  CHECK(cut.get_coeff(alpha_col) == doctest::Approx(1.0));
  CHECK(cut.lowb > -1e20);
  CHECK(cut.uppb > 1e20);
  // Source coefficient from reduced costs
  CHECK(std::abs(cut.get_coeff(src)) > 1e-10);
}

TEST_CASE(  // NOLINT
    "elastic_filter_solve - relaxes fixed column and solves clone")
{
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);

  auto r0 = li.resolve({});
  REQUIRE(r0.has_value());
  REQUIRE(li.is_optimal());

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto result = elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->clone.is_optimal());
    REQUIRE(result->link_infos.size() == 1);
    CHECK(result->link_infos[0].relaxed);
  }

  // Original LP untouched
  CHECK(li.get_col_low()[dep] == doctest::Approx(50.0));
  CHECK(li.get_col_upp()[dep] == doctest::Approx(50.0));
}

TEST_CASE(  // NOLINT
    "elastic_filter_solve - returns nullopt for non-fixed column")
{
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "dep",
  });  // NOT fixed

  auto demand = SparseRow {
      .lowb = 50.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  auto result = elastic_filter_solve(li, links, 1e6, {});
  CHECK_FALSE(result.has_value());
}

TEST_CASE(  // NOLINT
    "build_feasibility_cut - produces valid cut from elastic solve")
{
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const auto alpha_col = ColIndex {10};
  const auto src = ColIndex {11};

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto result = build_feasibility_cut(
      li, alpha_col, links, 1e6, SolverOptions {});  // NOLINT
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->cut.get_coeff(alpha_col) == doctest::Approx(1.0));
    CHECK(result->cut.lowb > -1e20);
    CHECK(result->elastic.clone.is_optimal());
    REQUIRE(result->elastic.link_infos.size() == 1);
    CHECK(result->elastic.link_infos[0].relaxed);
  }
}

TEST_CASE(  // NOLINT
    "build_feasibility_cut - returns nullopt for non-fixed column")
{
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "dep",
  });  // NOT fixed

  auto demand = SparseRow {
      .lowb = 50.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  auto result = build_feasibility_cut(
      li, ColIndex {10}, links, 1e6, SolverOptions {});  // NOLINT
  CHECK_FALSE(result.has_value());
}

TEST_CASE(  // NOLINT
    "build_multi_cuts - generates bound cuts from elastic slack")
{
  // LP infeasible when dep fixed at 50: x0+dep>=200, x0<=80
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 80.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 200.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);

  [[maybe_unused]] auto r0 = li.resolve({});
  CHECK_FALSE(li.is_optimal());

  const auto src = ColIndex {10};

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto elastic = elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(elastic.has_value());
  if (elastic) {
    CHECK(elastic->clone.is_optimal());

    auto multi = build_multi_cuts(*elastic, links);
    CHECK_FALSE(multi.empty());

    // sdn active (dep went from 50 to ~120) → lower-bound cut
    bool found_lb = false;
    for (const auto& mc : multi) {
      if (mc.lowb > -1e20) {
        found_lb = true;
        CHECK(mc.get_coeff(src) == doctest::Approx(1.0));
        CHECK(mc.lowb > 50.0);
      }
    }
    CHECK(found_lb);
  }
}

TEST_CASE(  // NOLINT
    "build_multi_cuts - returns empty when no slack is active")
{
  // Feasible LP with dep fixed
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});
  REQUIRE(li.is_optimal());

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto elastic = elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(elastic.has_value());
  if (elastic) {
    auto multi = build_multi_cuts(*elastic, links);
    CHECK(multi.empty());
  }
}

TEST_CASE(  // NOLINT
    "Benders cut tightens lower bound in two-phase LP")
{
  // Simulate a minimal 2-phase decomposition manually:
  //
  // Phase 0: min 10*x0 + alpha, s.t. x0 >= 20, x0 in [0,100]
  // Phase 1: min 50*x1, s.t. x1 + dep >= 80, dep fixed at x0, x1 in [0,100]
  //
  // Full: min 10*x0+50*x1, x0>=20, x1+x0>=80 → x0=80,x1=0 obj=800

  // Phase 0
  LinearInterface phase0;
  const auto x0 = phase0.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "x0",
  });
  phase0.set_obj_coeff(x0, 10.0);
  const auto alpha_col = phase0.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1e12,
      .name = "alpha",
  });
  phase0.set_obj_coeff(alpha_col, 1.0);

  auto constr0 = SparseRow {
      .lowb = 20.0,
      .uppb = LinearProblem::DblMax,
  };
  constr0[x0] = 1.0;
  phase0.add_row(constr0);

  // Phase 1
  LinearInterface phase1;
  const auto x1 = phase1.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "x1",
  });
  phase1.set_obj_coeff(x1, 50.0);
  const auto dep = phase1.add_col(SparseCol {
      .lowb = 20.0,
      .uppb = 20.0,
      .name = "dep",
  });

  auto constr1 = SparseRow {
      .lowb = 80.0,
      .uppb = LinearProblem::DblMax,
  };
  constr1[x1] = 1.0;
  constr1[dep] = 1.0;
  phase1.add_row(constr1);

  // Forward pass iteration 1: solve phase 0
  auto r0 = phase0.resolve({});
  REQUIRE(r0.has_value());
  REQUIRE(phase0.is_optimal());
  const double lb_before = phase0.get_obj_value();

  // Propagate x0 → dep
  std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = x0,
          .dependent_col = dep,
          .trial_value = 20.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };
  propagate_trial_values(links, phase0.get_col_sol_raw(), phase1);
  CHECK(links[0].trial_value == doctest::Approx(20.0));

  // Solve phase 1
  auto r1 = phase1.resolve({});
  REQUIRE(r1.has_value());
  REQUIRE(phase1.is_optimal());
  CHECK(phase1.get_obj_value() == doctest::Approx(3000.0));  // 60*50

  // Backward: build optimality cut and add to phase 0
  auto cut = build_benders_cut(
      alpha_col, links, phase1.get_col_cost_raw(), phase1.get_obj_value());
  phase0.add_row(cut);

  // Re-solve phase 0 with cut
  auto r0b = phase0.resolve({});
  REQUIRE(r0b.has_value());
  REQUIRE(phase0.is_optimal());
  const double lb_after = phase0.get_obj_value();

  // Lower bound must increase (cut tightens approximation)
  CHECK(lb_after > lb_before);
  // Phase 0 should now choose larger x0
  CHECK(phase0.get_col_sol_raw()[x0] > 20.0 + 1e-6);
}

// ─── BendersCut class tests ──────────────────────────────────────────────────
//
// These tests exercise BendersCut without a pool (null-pool mode) and with
// a work pool, verifying that:
//  - elastic_filter_solve() works equivalently to the free function
//  - infeasible_cut_count() is incremented on each successful elastic solve
//  - reset_infeasible_cut_count() resets the counter

TEST_CASE(
    "BendersCut - default construction and no-pool elastic_filter_solve")  // NOLINT
{
  // Build a simple LP where dep is fixed at 50 and x0 <= 80; demand >= 100.
  // With dep=50 the LP is feasible; the elastic filter should relax dep.
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto r0 = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  BendersCut bc;  // null pool
  CHECK(bc.pool() == nullptr);
  CHECK(bc.infeasible_cut_count() == 0);

  auto result = bc.elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->clone.is_optimal());
  }
  CHECK(bc.infeasible_cut_count() == 1);

  // A second solve increments again
  auto result2 = bc.elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result2.has_value());
  CHECK(bc.infeasible_cut_count() == 2);

  // Reset resets the counter
  bc.reset_infeasible_cut_count();
  CHECK(bc.infeasible_cut_count() == 0);

  // Original LP untouched
  CHECK(li.get_col_low()[dep] == doctest::Approx(50.0));
  CHECK(li.get_col_upp()[dep] == doctest::Approx(50.0));
}

TEST_CASE("BendersCut - elastic_filter_solve with work pool")  // NOLINT
{
  // Same LP as above but with a live work pool.
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto r0 = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto pool = make_solver_work_pool();
  BendersCut bc(pool.get());
  CHECK(bc.pool() == pool.get());
  CHECK(bc.infeasible_cut_count() == 0);

  auto result = bc.elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->clone.is_optimal());
  }
  // Counter incremented once
  CHECK(bc.infeasible_cut_count() == 1);

  // nullopt when no fixed columns
  LinearInterface li2;
  const auto x1 = li2.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "x1",
  });
  li2.set_obj_coeff(x1, 5.0);
  const auto dep2 = li2.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .name = "dep2",
  });  // NOT fixed
  auto d2 = SparseRow {.lowb = 50.0, .uppb = LinearProblem::DblMax};
  d2[x1] = 1.0;
  d2[dep2] = 1.0;
  li2.add_row(d2);
  [[maybe_unused]] auto r2 = li2.resolve({});

  const std::vector<StateVarLink> links2 = {
      StateVarLink {
          .dependent_col = dep2,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };
  auto result2 = bc.elastic_filter_solve(li2, links2, 1e6, {});
  CHECK_FALSE(result2.has_value());
  // Counter unchanged (no fixed column, no solve)
  CHECK(bc.infeasible_cut_count() == 1);

  // Detach pool before it goes out of scope
  bc.set_pool(nullptr);
}

TEST_CASE("BendersCut - build_feasibility_cut increments counter")  // NOLINT
{
  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .name = "x0",
  });
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col(SparseCol {
      .lowb = 50.0,
      .uppb = 50.0,
      .name = "dep",
  });

  auto demand = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const auto alpha_col = ColIndex {10};
  const auto src = ColIndex {11};

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  BendersCut bc;
  CHECK(bc.infeasible_cut_count() == 0);

  auto result = bc.build_feasibility_cut(
      li, alpha_col, links, 1e6, SolverOptions {});  // NOLINT
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->cut.get_coeff(alpha_col) == doctest::Approx(1.0));
    CHECK(result->elastic.clone.is_optimal());
  }

  // build_feasibility_cut calls elastic_filter_solve internally → counter == 1
  CHECK(bc.infeasible_cut_count() == 1);

  bc.reset_infeasible_cut_count();
  CHECK(bc.infeasible_cut_count() == 0);
}

TEST_CASE("BendersCut - set_pool updates pool reference")  // NOLINT
{
  BendersCut bc;
  CHECK(bc.pool() == nullptr);

  auto pool = make_solver_work_pool();
  bc.set_pool(pool.get());
  CHECK(bc.pool() == pool.get());

  bc.set_pool(nullptr);
  CHECK(bc.pool() == nullptr);
}
