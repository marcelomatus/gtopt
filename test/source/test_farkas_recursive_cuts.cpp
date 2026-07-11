// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_farkas_recursive_cuts.cpp
 * @brief     Correctness pins for `elastic_filter_mode = farkas_recursive`
 *            (Füllner & Rebennack, SIAM Review 2023, §17.2–17.3):
 *            `build_farkas_recursive_cut` + the `+z` elasticization of
 *            installed feasibility-cut rows in `elastic_filter_solve`,
 *            plus the plp → state_repair rename regression.
 *
 * The crown-jewel fixture reproduces the empirically-photographed kill
 * chain (/tmp/elastic_campaign, 2y case i1 p51/p52): a previously
 * installed feasibility-cut row that is unsatisfiable at the trial sits
 * HARD in the elastic clone → "relaxed clone infeasible".  With the
 * §17.2 `+ I z` term the clone solves and the §17.3 aggregated cut's
 * RHS folds the poisoned row's intercept through the clone optimum V.
 *
 * Fixture pattern follows test_plp_feasibility_cuts.cpp: build a
 * labelled LP, `initial_solve` while feasible, install the poisoned cut
 * row, pin the dependent column to the trial value, run
 * `elastic_filter_solve` with the unit cost policy, then extract cuts.
 */

#include <cmath>
#include <string_view>
#include <vector>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

namespace
{

/// The farkas_recursive cost policy: flat unit slack costs, no tilt.
constexpr ElasticCostPolicy kFrkUnitPolicy {
    .model = ElasticCostPolicy::Model::unit,
};

constexpr double kFrkFactEps = 1e-8;

/// Kill-chain fixture (the anti-"relaxed clone infeasible" pin):
///   dep in [0, 1000], pinned at trial = 500 by the forward pass;
///   vf  in [0, 2000];
///   balance: vf − 0.5·dep = 50   (gain-0.5 turbine + inflow)
///   POISONED installed fcut row:  vf ≥ 700.
/// Source box [0, 530] ⇒ clone dep ≤ 530 ⇒ vf ≤ 315 < 700: without
/// the +z term the relaxed clone is INFEASIBLE (the photographed kill
/// chain).  With z the unique optimum keeps dep at the trial (raising
/// dep costs 1/unit but only recovers 0.5/unit of z) so
///   z* = 700 − 300 = 400,  V = 400,  σ = 0.5,  ω = 1.
/// The §17.3 cut: 0.5·x ≥ 0.5·500 + 400 = 650 — the +400 folding term
/// is exactly ω·z* (the poisoned row's contribution), absent from any
/// σ-only formula.
struct FrkKillChainFixture
{
  LinearInterface li;
  ColIndex dep {};
  ColIndex vf {};
  RowIndex poisoned_row {};
  std::vector<StateVarLink> links;

  static constexpr double kTrial = 500.0;
  static constexpr double kInflow = 50.0;
  static constexpr double kGain = 0.5;
  static constexpr double kPoisonedRhs = 700.0;
  static constexpr double kSourceUpp = 530.0;
  static constexpr ColIndex kSourceCol {7};

  FrkKillChainFixture()
  {
    dep = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "eini",
        .variable_uid = 51,
    });
    vf = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 2000.0,
        .class_name = "Reservoir",
        .variable_name = "efin",
        .variable_uid = 51,
    });

    SparseRow balance {
        .lowb = kInflow,
        .uppb = kInflow,
        .class_name = "Reservoir",
        .constraint_name = "balance",
        .variable_uid = 51,
    };
    balance[vf] = 1.0;
    balance[dep] = -kGain;
    std::ignore = li.add_row(balance);

    // Solve once while feasible, then install the poisoned cut row
    // (as the forward pass would have after a downstream fcut event)
    // and pin the dependent column at the infeasible trial.
    std::ignore = li.initial_solve();
    REQUIRE(li.is_optimal());

    SparseRow poisoned {
        .lowb = kPoisonedRhs,
        .uppb = LinearProblem::DblMax,
        .class_name = sddp_alpha_class_name,
        .constraint_name = sddp_fcut_constraint_name,
        .variable_uid = 51,
    };
    poisoned[vf] = 1.0;
    poisoned_row = li.add_row(poisoned);

    li.set_col(dep, kTrial);

    links = {
        {
            .source_col = kSourceCol,
            .dependent_col = dep,
            .target_phase_index = PhaseIndex {1},
            .trial_value = kTrial,
            .source_low = 0.0,
            .source_upp = kSourceUpp,
            .uid = Uid {51},
        },
    };
  }
};

}  // namespace

TEST_CASE(
    "farkas_recursive: poisoned fcut row is relaxed, not fatal")  // NOLINT
{
  FrkKillChainFixture fx;

  // (a) The kill chain photographed on the 2y case: WITHOUT the +z
  // term the state-relaxed clone is itself infeasible and the builder
  // reports FAIL — "relaxed clone infeasible".
  {
    auto elastic = elastic_filter_solve(
        fx.li, fx.links, 1.0, SolverOptions {}, kFrkUnitPolicy);
    REQUIRE(elastic.has_value());
    CHECK(!elastic->solved);
    auto res = build_farkas_recursive_cut(
        elastic.value(), fx.links, LpContext {}, kFrkFactEps);
    CHECK(res.status == PlpCutStatus::fail);
    CHECK(res.cuts.empty());
  }

  // (b) WITH the §17.2 `+ I z` term the clone SOLVES and one
  // aggregated cut is emitted.
  const std::vector<RowIndex> fcut_rows {
      fx.poisoned_row,
  };
  auto elastic = elastic_filter_solve(
      fx.li, fx.links, 1.0, SolverOptions {}, kFrkUnitPolicy, fcut_rows);
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  REQUIRE(elastic->fcut_infos.size() == 1);
  CHECK(elastic->fcut_infos.front().row == fx.poisoned_row);

  auto res = build_farkas_recursive_cut(
      elastic.value(), fx.links, LpContext {}, kFrkFactEps);
  CHECK(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 1);

  const auto& cut = res.cuts.front();
  REQUIRE(cut.cmap.contains(FrkKillChainFixture::kSourceCol));
  const double sigma = cut.cmap.at(FrkKillChainFixture::kSourceCol);

  // σ = kGain: raising the trial recovers only 0.5 units of z per
  // unit of state (interior optimum, unique vertex).
  CHECK(sigma == doctest::Approx(FrkKillChainFixture::kGain).epsilon(1e-6));

  // The RHS folds the poisoned row's intercept: read ω and z* off the
  // clone and verify rhs = σ·v̂ + ω·z* (+ the fact_eps margin).  Here
  // ω·z* = 1 × 400 — a KNOWN +400 vs the σ-only term σ·v̂ = 250.
  const auto& fi = elastic->fcut_infos.front();
  auto duals = elastic->clone.get_row_dual();
  const double omega = duals[fi.row];
  const double z_star = elastic->clone.get_col_sol_raw()[fi.z_col];
  CHECK(omega == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(z_star == doctest::Approx(400.0).epsilon(1e-6));

  const double sigma_only = sigma * FrkKillChainFixture::kTrial;
  const double expected = (sigma_only + (omega * z_star)) * (1.0 + kFrkFactEps);
  CHECK(cut.lowb == doctest::Approx(expected).epsilon(1e-9));
  CHECK(cut.lowb == doctest::Approx(650.0).epsilon(1e-6));
  CHECK(cut.lowb - sigma_only > 399.0);  // the folding term is present

  // Contrast pin: the state_repair builder on the SAME clone zeroes
  // the link via its dx filter (the repair went entirely through z)
  // and reports HOLGURAS — only farkas_recursive extracts a cut here.
  auto plp = build_plp_feasibility_cuts(
      elastic.value(), fx.links, LpContext {}, kFrkFactEps);
  CHECK(plp.status == PlpCutStatus::holguras);
}

TEST_CASE(
    "farkas_recursive: sigma-only equivalence without fcut rows")  // NOLINT
{
  // CANUTILLAR-pattern fixture (same arithmetic as the state_repair
  // pin): dep in [0, 1000] pinned at 528.705, vf ≥ 596.152,
  // balance vf − dep = 63.605.  Minimal repair +3.842 through sdn.
  LinearInterface li;
  const auto dep = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1000.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 64,
  });
  const auto vf = li.add_col(SparseCol {
      .lowb = 596.152,
      .uppb = 1000.0,
      .class_name = "Reservoir",
      .variable_name = "efin",
      .variable_uid = 64,
  });
  SparseRow balance {
      .lowb = 63.605,
      .uppb = 63.605,
      .class_name = "Reservoir",
      .constraint_name = "balance",
      .variable_uid = 64,
  };
  balance[vf] = 1.0;
  balance[dep] = -1.0;
  std::ignore = li.add_row(balance);

  std::ignore = li.initial_solve();
  REQUIRE(li.is_optimal());
  li.set_col(dep, 528.705);

  // A live StateVariable so BOTH builders read the same
  // v̂_phys = col_sol_physical() (build_feasibility_cut_physical's
  // null-state_var fallback is 0.0, unlike the farkas/plp builders').
  const StateVariable sv {LPKey {}, dep, 0.0, 1.0, LpContext {}};
  sv.set_col_sol(528.705);

  const ColIndex src {7};
  const std::vector<StateVarLink> links = {
      {
          .source_col = src,
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 528.705,
          .source_low = 0.0,
          .source_upp = 1000.0,
          .state_var = &sv,
          .uid = Uid {64},
      },
  };

  auto elastic =
      elastic_filter_solve(li, links, 1.0, SolverOptions {}, kFrkUnitPolicy);
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  // Unit policy pins the slack OBJECTIVE coefficients at exactly 1.0
  // on the clone (`get_obj_coeff` — the post-solve `get_col_cost*`
  // views return reduced costs, not objective coefficients).
  const auto& info = elastic->link_infos.front();
  REQUIRE(info.relaxed);
  CHECK(elastic->clone.get_obj_coeff()[info.sup_col] == doctest::Approx(1.0));
  CHECK(elastic->clone.get_obj_coeff()[info.sdn_col] == doctest::Approx(1.0));

  auto res = build_farkas_recursive_cut(
      elastic.value(), links, LpContext {}, kFrkFactEps);
  REQUIRE(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 1);
  const auto& frk = res.cuts.front();
  REQUIRE(frk.cmap.contains(src));

  // With NO installed fcut rows the aggregated §17.3 cut collapses to
  // the σ-only cut: same coefficient and RHS as the aggregated
  // build_feasibility_cut_physical row (up to the fact_eps margin).
  auto agg = build_feasibility_cut_physical(
      links, elastic->link_infos, elastic->clone, kFrkFactEps, 1);
  REQUIRE(agg.cmap.contains(src));
  CHECK(frk.cmap.at(src) == doctest::Approx(agg.cmap.at(src)).epsilon(1e-9));
  CHECK(frk.lowb
        == doctest::Approx(agg.lowb * (1.0 + kFrkFactEps)).epsilon(1e-6));

  // Analytic pin: x ≥ 532.547 — the same mid-box floor state_repair
  // emits on this fixture.
  const double implied = frk.lowb / frk.cmap.at(src);
  CHECK(implied == doctest::Approx(532.547).epsilon(1e-6));
}

TEST_CASE(
    "farkas_recursive: cut separates trial from feasible states")  // NOLINT
{
  // Two coupled reservoirs, `dep_a + dep_b ≥ 70`, trials (30, 30).
  // Any optimal split of the +10 repair yields σ_a = σ_b = 1 (the
  // fixing-row duals are split-independent), so the aggregated cut is
  // the exact joint requirement x_a + x_b ≥ 70 — solver-robust even
  // though the primal optimum is degenerate.
  LinearInterface li;
  const auto dep_a = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 1,
  });
  const auto dep_b = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 2,
  });
  SparseRow demand {
      .lowb = 70.0,
      .uppb = LinearProblem::DblMax,
      .class_name = "Demand",
      .constraint_name = "min",
      .variable_uid = 1,
  };
  demand[dep_a] = 1.0;
  demand[dep_b] = 1.0;
  std::ignore = li.add_row(demand);

  std::ignore = li.initial_solve();
  REQUIRE(li.is_optimal());
  li.set_col(dep_a, 30.0);
  li.set_col(dep_b, 30.0);

  const ColIndex src_a {10};
  const ColIndex src_b {11};
  const std::vector<StateVarLink> links = {
      {
          .source_col = src_a,
          .dependent_col = dep_a,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 30.0,
          .source_low = 0.0,
          .source_upp = 100.0,
          .uid = Uid {1},
      },
      {
          .source_col = src_b,
          .dependent_col = dep_b,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 30.0,
          .source_low = 0.0,
          .source_upp = 100.0,
          .uid = Uid {2},
      },
  };

  auto elastic =
      elastic_filter_solve(li, links, 1.0, SolverOptions {}, kFrkUnitPolicy);
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  auto res = build_farkas_recursive_cut(
      elastic.value(), links, LpContext {}, kFrkFactEps);
  REQUIRE(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 1);
  const auto& cut = res.cuts.front();
  REQUIRE(cut.cmap.contains(src_a));
  REQUIRE(cut.cmap.contains(src_b));
  const double ca = cut.cmap.at(src_a);
  const double cb = cut.cmap.at(src_b);
  CHECK(ca == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(cb == doctest::Approx(1.0).epsilon(1e-6));

  // (i) the infeasible trial point is cut off: 30 + 30 = 60 < rhs.
  CHECK(ca * 30.0 + cb * 30.0 < cut.lowb - 1e-6);
  // (ii) a known-feasible state point is NOT cut off: (50, 40).
  CHECK(ca * 50.0 + cb * 40.0 >= cut.lowb - 1e-9);
  // (iii) the cut is exactly the joint requirement (+ margin).
  CHECK(cut.lowb == doctest::Approx(70.0 * (1.0 + kFrkFactEps)).epsilon(1e-9));
}

TEST_CASE("farkas_recursive: holguras and fail paths")  // NOLINT
{
  SUBCASE("holguras: repair has no state sensitivity")
  {
    // The infeasibility lives entirely on an installed fcut row over a
    // NON-state column (w ≤ 10 vs w ≥ 20): z absorbs it, every fixing
    // row dual is 0 → no σ survives the zero-guard → HOLGURAS.
    LinearInterface li;
    const auto dep = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "eini",
        .variable_uid = 3,
    });
    const auto w = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .class_name = "Flow",
        .variable_name = "w",
        .variable_uid = 3,
    });

    std::ignore = li.initial_solve();
    REQUIRE(li.is_optimal());

    SparseRow poisoned {
        .lowb = 20.0,
        .uppb = LinearProblem::DblMax,
        .class_name = sddp_alpha_class_name,
        .constraint_name = sddp_fcut_constraint_name,
        .variable_uid = 3,
    };
    poisoned[w] = 1.0;
    const auto poisoned_row = li.add_row(poisoned);
    li.set_col(dep, 500.0);

    const std::vector<StateVarLink> links = {
        {
            .source_col = ColIndex {5},
            .dependent_col = dep,
            .target_phase_index = PhaseIndex {1},
            .trial_value = 500.0,
            .source_low = 0.0,
            .source_upp = 1000.0,
            .uid = Uid {3},
        },
    };
    const std::vector<RowIndex> fcut_rows {
        poisoned_row,
    };

    auto elastic = elastic_filter_solve(
        li, links, 1.0, SolverOptions {}, kFrkUnitPolicy, fcut_rows);
    REQUIRE(elastic.has_value());
    REQUIRE(elastic->solved);

    auto res = build_farkas_recursive_cut(
        elastic.value(), links, LpContext {}, kFrkFactEps);
    CHECK(res.status == PlpCutStatus::holguras);
    CHECK(res.cuts.empty());
  }

  SUBCASE("fail: hard non-cut row stays unrelaxable")
  {
    // Same shape, but the w ≥ 20 row is a HARD row (NOT passed in
    // elastic_fcut_rows): the clone stays infeasible → FAIL.
    LinearInterface li;
    const auto dep = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "eini",
        .variable_uid = 4,
    });
    const auto w = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .class_name = "Flow",
        .variable_name = "w",
        .variable_uid = 4,
    });

    std::ignore = li.initial_solve();
    REQUIRE(li.is_optimal());

    SparseRow hard {
        .lowb = 20.0,
        .uppb = LinearProblem::DblMax,
        .class_name = "Flow",
        .constraint_name = "hard_min",
        .variable_uid = 4,
    };
    hard[w] = 1.0;
    std::ignore = li.add_row(hard);
    li.set_col(dep, 500.0);

    const std::vector<StateVarLink> links = {
        {
            .source_col = ColIndex {5},
            .dependent_col = dep,
            .target_phase_index = PhaseIndex {1},
            .trial_value = 500.0,
            .source_low = 0.0,
            .source_upp = 1000.0,
            .uid = Uid {4},
        },
    };

    auto elastic =
        elastic_filter_solve(li, links, 1.0, SolverOptions {}, kFrkUnitPolicy);
    REQUIRE(elastic.has_value());
    CHECK(!elastic->solved);

    auto res = build_farkas_recursive_cut(
        elastic.value(), links, LpContext {}, kFrkFactEps);
    CHECK(res.status == PlpCutStatus::fail);
    CHECK(res.cuts.empty());
  }
}

TEST_CASE("elastic mode rename: plp == state_repair; JSON canonical")  // NOLINT
{
  SUBCASE("parser accepts both spellings plus the new mode")
  {
    CHECK(parse_elastic_filter_mode("state_repair")
          == ElasticFilterMode::state_repair);
    CHECK(parse_elastic_filter_mode("plp")  // legacy alias
          == ElasticFilterMode::state_repair);
    CHECK(parse_elastic_filter_mode("farkas_recursive")
          == ElasticFilterMode::farkas_recursive);
    // Canonical name wins the reverse lookup (JSON emission path).
    CHECK(enum_name(ElasticFilterMode::state_repair) == "state_repair");
    CHECK(enum_name(ElasticFilterMode::farkas_recursive) == "farkas_recursive");
  }

  SUBCASE("JSON round-trip emits state_repair, accepts plp")
  {
    SddpOptions opts;
    opts.elastic_mode = ElasticFilterMode::state_repair;
    const std::string json = daw::json::to_json(opts);
    CHECK(json.find("\"state_repair\"") != std::string::npos);
    CHECK(json.find("\"plp\"") == std::string::npos);

    const auto rt = daw::json::from_json<SddpOptions>(std::string_view {json});
    CHECK(rt.elastic_mode.value_or(ElasticFilterMode::single_cut)
          == ElasticFilterMode::state_repair);

    constexpr std::string_view legacy = R"({"elastic_mode": "plp"})";
    const auto from_alias = daw::json::from_json<SddpOptions>(legacy);
    CHECK(from_alias.elastic_mode.value_or(ElasticFilterMode::single_cut)
          == ElasticFilterMode::state_repair);

    constexpr std::string_view frk =
        R"({"elastic_mode": "farkas_recursive", "fact_eps": 1e-10})";
    const auto from_frk = daw::json::from_json<SddpOptions>(frk);
    CHECK(from_frk.elastic_mode.value_or(ElasticFilterMode::single_cut)
          == ElasticFilterMode::farkas_recursive);
    CHECK(from_frk.fact_eps.value_or(0.0) == doctest::Approx(1e-10));
  }
}
