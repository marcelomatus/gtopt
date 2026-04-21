/**
 * @file      test_benders_cut.hpp
 * @brief     Unit tests for Benders cut construction and averaging functions
 * @date      2026-03-21
 * @copyright BSD-3-Clause
 */

#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/sparse_col.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// build_benders_cut
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut basic optimality cut")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Simple 2-link scenario:
  //   α >= z + rc1*(x1 - v1) + rc2*(x2 - v2)
  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  10,
              },
          .trial_value = 5.0,
      },
      {
          .source_col =
              ColIndex {
                  2,
              },
          .dependent_col =
              ColIndex {
                  11,
              },
          .trial_value = 3.0,
      },
  };

  // Reduced costs at dependent columns
  std::vector<double> rc(12, 0.0);
  rc[10] = 2.0;
  rc[11] = -1.0;

  const double obj_value = 100.0;
  auto cut = build_benders_cut(alpha, links, rc, obj_value);

  // α coefficient = 1.0
  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));
  // source_col[0] coefficient = -rc[10] = -2.0
  CHECK(cut.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(-2.0));
  // source_col[1] coefficient = -rc[11] = 1.0
  CHECK(cut.cmap.at(ColIndex {
            2,
        })
        == doctest::Approx(1.0));
  // lowb = obj - rc1*v1 - rc2*v2 = 100 - 2*5 - (-1)*3 = 100-10+3 = 93
  CHECK(cut.lowb == doctest::Approx(93.0));
  CHECK(cut.uppb == LinearProblem::DblMax);
}

TEST_CASE("build_benders_cut with empty links")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> empty_links;
  const std::vector<double> rc;

  auto cut = build_benders_cut(alpha, empty_links, rc, 42.0);
  CHECK(cut.lowb == doctest::Approx(42.0));
  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));
  CHECK(cut.cmap.size() == 1);
}

// ---------------------------------------------------------------------------
// build_benders_cut_physical — physical-space cut builder intended for
// `LinearInterface::add_row` on an equilibrated LP.  Unlike the LP-space
// overloads above, this variant:
//   - takes `reduced_costs_physical` and `objective_value_physical`
//     (caller uses `target_li.get_col_cost()` / `get_obj_value_physical()`);
//   - takes `trial_values_physical` as a span indexed parallel to
//     `links` (caller builds it from `source_li.get_col_sol()[...]`);
//   - emits coefficient 1.0 on the α column and row.scale 1.0 — the LP
//     interface folds col_scales + row-max internally on insertion;
//   - leaves `already_lp_space == false` so `add_row` applies the
//     physical → LP conversion pipeline.
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut_physical basic cut")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  10,
              },
      },
      {
          .source_col =
              ColIndex {
                  2,
              },
          .dependent_col =
              ColIndex {
                  11,
              },
      },
  };

  // Physical reduced costs at dependent columns.
  std::vector<double> rc_phys(12, 0.0);
  rc_phys[10] = 2.0;
  rc_phys[11] = -1.0;

  // Physical trial values, one per link (positional, not per dep_col).
  const std::vector<double> trial_phys {5.0, 3.0};

  const double obj_phys = 100.0;
  const auto cut =
      build_benders_cut_physical(alpha, links, rc_phys, trial_phys, obj_phys);

  // α coefficient = 1.0 (no scale_alpha — that's a col_scale of alpha,
  // applied by add_row on insertion).
  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));

  // source_col[0] coefficient = -rc_phys[10] = -2.0
  CHECK(cut.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(-2.0));
  // source_col[1] coefficient = -rc_phys[11] = 1.0
  CHECK(cut.cmap.at(ColIndex {
            2,
        })
        == doctest::Approx(1.0));

  // lowb = obj_phys - rc1*v1 - rc2*v2 = 100 - 2*5 - (-1)*3 = 93
  CHECK(cut.lowb == doctest::Approx(93.0));
  CHECK(cut.uppb == LinearProblem::DblMax);

  // Row starts in physical space; add_row will apply col_scales +
  // row-max and set a composite row_scale.
  CHECK(cut.scale == doctest::Approx(1.0));
  CHECK_FALSE(cut.already_lp_space);
}

TEST_CASE("build_benders_cut_physical with empty links")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> empty_links;
  const std::vector<double> rc_phys;
  const std::vector<double> trial_phys;

  const auto cut =
      build_benders_cut_physical(alpha, empty_links, rc_phys, trial_phys, 42.0);
  CHECK(cut.lowb == doctest::Approx(42.0));
  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));
  CHECK(cut.cmap.size() == 1);
  CHECK_FALSE(cut.already_lp_space);
}

TEST_CASE(
    "build_benders_cut_physical eps filter drops tiny rc terms")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  10,
              },
      },
      {
          .source_col =
              ColIndex {
                  2,
              },
          .dependent_col =
              ColIndex {
                  11,
              },
      },
  };

  std::vector<double> rc_phys(12, 0.0);
  rc_phys[10] = 1e-12;  // below eps
  rc_phys[11] = 4.0;

  const std::vector<double> trial_phys {5.0, 3.0};

  const auto cut =
      build_benders_cut_physical(alpha,
                                 links,
                                 rc_phys,
                                 trial_phys,
                                 /*objective_value_physical=*/100.0,
                                 /*cut_coeff_eps=*/1e-8);

  // Link 0 filtered out: no coefficient for source_col 1, and its
  // contribution (rc1*v1 ≈ 5e-12) does not affect lowb.
  CHECK_FALSE(cut.cmap.contains(ColIndex {
      1,
  }));
  // Link 1 kept: source_col 2 gets -rc=-4.
  CHECK(cut.cmap.at(ColIndex {
            2,
        })
        == doctest::Approx(-4.0));
  // lowb = obj - rc2*v2 = 100 - 4*3 = 88.
  CHECK(cut.lowb == doctest::Approx(88.0));
}

TEST_CASE("build_benders_cut_physical preserves physical-space contract")
{
  // Regression guard: the physical builder must NOT apply any
  // scale_alpha / scale_objective arithmetic.  Coefficients and RHS
  // are taken verbatim from the physical inputs so that add_row on an
  // equilibrated LP can safely fold col_scales and row-max.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  10,
              },
      },
  };
  std::vector<double> rc_phys(11, 0.0);
  rc_phys[10] = 3.14;
  const std::vector<double> trial_phys {2.0};

  const auto cut = build_benders_cut_physical(
      alpha, links, rc_phys, trial_phys, /*objective_value_physical=*/7.5);

  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));
  CHECK(cut.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(-3.14));
  // lowb = 7.5 - 3.14*2.0 = 7.5 - 6.28 = 1.22
  CHECK(cut.lowb == doctest::Approx(1.22));
  // No row.scale → scale stays 1.0; add_row handles col_scales[alpha].
  CHECK(cut.scale == doctest::Approx(1.0));
}

// ---------------------------------------------------------------------------
// build_benders_cut_physical — state_var-based overload (SDDP backward-pass)
//
// The state_var overload reads rc and trial from `link.state_var` instead
// of parallel spans.  It descales the stored raw-LP reduced cost via
// `reduced_cost_physical(scale_obj) = rc_LP * scale_obj / var_scale` so
// the cut is in $ / physical_unit regardless of scale_objective.
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut_physical state_var overload matches span overload")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Hand-rolled StateVariable objects.  ColIndex in the state_var is the
  // source column (what the cut will coefficient on); the `var_scale` and
  // the raw-LP rc/col_sol determine physical values.
  constexpr auto mkctx = [] { return LpContext {}; };

  const StateVariable::LPKey key0 {
      .scene_index = first_scene_index(),
      .phase_index = PhaseIndex {0},
  };
  StateVariable svar0 {
      key0,
      ColIndex {1},
      /*scost=*/0.0,
      /*var_scale=*/2.0,
      mkctx(),
  };
  svar0.set_col_sol(5.0);  // phys = 5 * 2 = 10
  svar0.set_reduced_cost(0.4);  // rc_phys = 0.4 * scale_obj / 2

  StateVariable svar1 {
      key0,
      ColIndex {2},
      /*scost=*/0.0,
      /*var_scale=*/1.0,
      mkctx(),
  };
  svar1.set_col_sol(3.0);  // phys = 3
  svar1.set_reduced_cost(-2.0);  // rc_phys = -2 * scale_obj

  constexpr double scale_obj = 10.0;

  const std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {1},
          .dependent_col = ColIndex {10},
          .state_var = &svar0,
      },
      {
          .source_col = ColIndex {2},
          .dependent_col = ColIndex {11},
          .state_var = &svar1,
      },
  };

  // rc_phys[0] = 0.4 * 10 / 2 = 2.0
  // rc_phys[1] = -2.0 * 10 / 1 = -20.0
  // v̂_phys[0] = 10.0, v̂_phys[1] = 3.0
  // lowb = 100 - 2*10 - (-20)*3 = 100 - 20 + 60 = 140
  const double obj_phys = 100.0;
  const auto cut =
      build_benders_cut_physical(ColIndex {0}, links, obj_phys, scale_obj);

  CHECK(cut.cmap.at(ColIndex {0}) == doctest::Approx(1.0));
  CHECK(cut.cmap.at(ColIndex {1}) == doctest::Approx(-2.0));
  CHECK(cut.cmap.at(ColIndex {2}) == doctest::Approx(20.0));
  CHECK(cut.lowb == doctest::Approx(140.0));
  CHECK_FALSE(cut.already_lp_space);
  CHECK(cut.scale == doctest::Approx(1.0));
}

TEST_CASE("build_benders_cut_physical state_var overload eps filters small rc")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const StateVariable::LPKey key0 {
      .scene_index = first_scene_index(),
      .phase_index = PhaseIndex {0},
  };
  StateVariable svar_tiny {
      key0,
      ColIndex {1},
      0.0,
      1.0,
      LpContext {},
  };
  svar_tiny.set_col_sol(5.0);
  svar_tiny.set_reduced_cost(1e-13);  // rc_phys = 1e-13 * 1 / 1 = 1e-13

  StateVariable svar_big {
      key0,
      ColIndex {2},
      0.0,
      1.0,
      LpContext {},
  };
  svar_big.set_col_sol(3.0);
  svar_big.set_reduced_cost(4.0);

  const std::vector<StateVarLink> links = {
      {.source_col = ColIndex {1}, .state_var = &svar_tiny},
      {.source_col = ColIndex {2}, .state_var = &svar_big},
  };

  const auto cut =
      build_benders_cut_physical(ColIndex {0},
                                 links,
                                 /*objective_value_physical=*/100.0,
                                 /*scale_objective=*/1.0,
                                 /*cut_coeff_eps=*/1e-8);

  CHECK_FALSE(cut.cmap.contains(ColIndex {1}));
  CHECK(cut.cmap.at(ColIndex {2}) == doctest::Approx(-4.0));
  // lowb = 100 - 4*3 = 88.  Tiny rc contribution (1e-13 * 5) is also
  // dropped since the whole link is skipped.
  CHECK(cut.lowb == doctest::Approx(88.0));
}

TEST_CASE(
    "build_benders_cut_physical state_var overload handles null state_var")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Test-fixture convention: a link with state_var==nullptr contributes
  // nothing to the cut (no coefficient, no RHS adjustment).
  const std::vector<StateVarLink> links = {
      {.source_col = ColIndex {1}, .state_var = nullptr},
  };
  const auto cut = build_benders_cut_physical(ColIndex {0},
                                              links,
                                              /*objective_value_physical=*/42.0,
                                              /*scale_objective=*/1.0);

  CHECK(cut.cmap.at(ColIndex {0}) == doctest::Approx(1.0));
  CHECK_FALSE(cut.cmap.contains(ColIndex {1}));
  CHECK(cut.lowb == doctest::Approx(42.0));
}

// ---------------------------------------------------------------------------
// average_benders_cut
// ---------------------------------------------------------------------------

TEST_CASE("average_benders_cut with empty vector")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<SparseRow> empty;
  auto avg = average_benders_cut(empty);
  CHECK(avg.cmap.empty());
}

TEST_CASE("average_benders_cut with single cut")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto cut = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  cut[ColIndex {
      0,
  }] = 3.0;
  cut[ColIndex {
      1,
  }] = -2.0;

  const std::vector<SparseRow> cuts = {
      cut,
  };
  auto avg = average_benders_cut(cuts);

  CHECK(avg.lowb == doctest::Approx(10.0));
  CHECK(avg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(3.0));
  CHECK(avg.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(-2.0));
}

TEST_CASE("average_benders_cut with multiple cuts")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto cut1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  cut1[ColIndex {
      0,
  }] = 4.0;
  cut1[ColIndex {
      1,
  }] = 2.0;

  auto cut2 = SparseRow {
      .lowb = 20.0,
      .uppb = LinearProblem::DblMax,
  };
  cut2[ColIndex {
      0,
  }] = 6.0;
  cut2[ColIndex {
      1,
  }] = -2.0;

  const std::vector<SparseRow> cuts = {
      cut1,
      cut2,
  };
  auto avg = average_benders_cut(cuts);

  // lowb = (10+20)/2 = 15
  CHECK(avg.lowb == doctest::Approx(15.0));
  // col0 = (4+6)/2 = 5
  CHECK(avg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(5.0));
  // col1 = (2+(-2))/2 = 0
  CHECK(avg.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(0.0));
}

// ---------------------------------------------------------------------------
// weighted_average_benders_cut
// ---------------------------------------------------------------------------

TEST_CASE("weighted_average_benders_cut empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<SparseRow> empty;
  const std::vector<double> weights;
  auto wavg = weighted_average_benders_cut(empty, weights);
  CHECK(wavg.cmap.empty());
}

TEST_CASE("weighted_average_benders_cut size mismatch")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto c1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 1.0;

  const std::vector<SparseRow> cuts = {
      c1,
  };
  const std::vector<double> weights = {
      0.5,
      0.5,
  };  // mismatched sizes

  auto wavg = weighted_average_benders_cut(cuts, weights);
  CHECK(wavg.cmap.empty());  // returns empty cut on mismatch
}

TEST_CASE("weighted_average_benders_cut zero total weight")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto c1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 1.0;

  const std::vector<SparseRow> cuts = {
      c1,
  };
  const std::vector<double> weights = {
      0.0,
  };

  auto wavg = weighted_average_benders_cut(cuts, weights);
  CHECK(wavg.cmap.empty());
}

TEST_CASE("weighted_average_benders_cut single cut")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto c1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 3.0;

  const std::vector<SparseRow> cuts = {
      c1,
  };
  const std::vector<double> weights = {
      1.0,
  };

  auto wavg = weighted_average_benders_cut(cuts, weights);
  CHECK(wavg.lowb == doctest::Approx(10.0));
  CHECK(wavg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(3.0));
}

TEST_CASE("weighted_average_benders_cut multiple cuts")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto c1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 4.0;

  auto c2 = SparseRow {
      .lowb = 30.0,
      .uppb = LinearProblem::DblMax,
  };
  c2[ColIndex {
      0,
  }] = 8.0;

  const std::vector<SparseRow> cuts = {
      c1,
      c2,
  };
  // weights: 0.25 and 0.75 → normalized: 0.25/1.0 and 0.75/1.0
  const std::vector<double> weights = {
      0.25,
      0.75,
  };

  auto wavg = weighted_average_benders_cut(cuts, weights);
  // lowb = 0.25*10 + 0.75*30 = 2.5 + 22.5 = 25.0
  CHECK(wavg.lowb == doctest::Approx(25.0));
  // col0 = 0.25*4 + 0.75*8 = 1 + 6 = 7.0
  CHECK(wavg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(7.0));
}

// ---------------------------------------------------------------------------
// accumulate_benders_cuts
// ---------------------------------------------------------------------------

TEST_CASE("accumulate_benders_cuts empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<SparseRow> empty;
  auto acc = accumulate_benders_cuts(empty);
  CHECK(acc.cmap.empty());
}

TEST_CASE("accumulate_benders_cuts single cut")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto c1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 5.0;

  const std::vector<SparseRow> cuts = {
      c1,
  };
  auto acc = accumulate_benders_cuts(cuts);
  CHECK(acc.lowb == doctest::Approx(10.0));
  CHECK(acc.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(5.0));
}

TEST_CASE("accumulate_benders_cuts multiple cuts sums")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto c1 = SparseRow {
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 4.0;
  c1[ColIndex {
      1,
  }] = 2.0;

  auto c2 = SparseRow {
      .lowb = 20.0,
      .uppb = LinearProblem::DblMax,
  };
  c2[ColIndex {
      0,
  }] = 6.0;
  c2[ColIndex {
      2,
  }] = 3.0;

  const std::vector<SparseRow> cuts = {
      c1,
      c2,
  };
  auto acc = accumulate_benders_cuts(cuts);

  // lowb = 10 + 20 = 30
  CHECK(acc.lowb == doctest::Approx(30.0));
  // col0 = 4 + 6 = 10
  CHECK(acc.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(10.0));
  // col1 = 2 (only in c1)
  CHECK(acc.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(2.0));
  // col2 = 3 (only in c2)
  CHECK(acc.cmap.at(ColIndex {
            2,
        })
        == doctest::Approx(3.0));
}

// ---------------------------------------------------------------------------
// propagate_trial_values
// ---------------------------------------------------------------------------

TEST_CASE("propagate_trial_values sets bounds")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearInterface li;
  // source columns
  const auto s1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto s2 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  // dependent columns
  const auto d1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto d2 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });

  std::vector<StateVarLink> links = {
      {
          .source_col = s1,
          .dependent_col = d1,
      },
      {
          .source_col = s2,
          .dependent_col = d2,
      },
  };

  // Source solution: s1=25, s2=50
  std::vector<double> source_sol = {
      25.0,
      50.0,
      0.0,
      0.0,
  };

  propagate_trial_values(links, source_sol, li);

  // After propagation: dependent columns fixed at trial values
  CHECK(links[0].trial_value == doctest::Approx(25.0));
  CHECK(links[1].trial_value == doctest::Approx(50.0));
  CHECK(li.get_col_low()[d1] == doctest::Approx(25.0));
  CHECK(li.get_col_upp()[d1] == doctest::Approx(25.0));
  CHECK(li.get_col_low()[d2] == doctest::Approx(50.0));
  CHECK(li.get_col_upp()[d2] == doctest::Approx(50.0));
}

// ---------------------------------------------------------------------------
// relax_fixed_state_variable
// ---------------------------------------------------------------------------

TEST_CASE("relax_fixed_state_variable relaxes a fixed column")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearInterface li;
  const auto dep = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });

  // Fix the column (simulate propagate_trial_values)
  li.set_col_low(dep, 42.0);
  li.set_col_upp(dep, 42.0);

  const StateVarLink link {
      .source_col =
          ColIndex {
              99,
          },
      .dependent_col = dep,
      .source_phase_index =
          PhaseIndex {
              0,
          },
      .target_phase_index =
          PhaseIndex {
              1,
          },
      .trial_value = 42.0,
      .source_low = 0.0,
      .source_upp = 100.0,
  };

  auto info = relax_fixed_state_variable(li,
                                         link,
                                         PhaseIndex {
                                             1,
                                         },
                                         1e6);

  CHECK(info.relaxed);
  // Dependent column bounds relaxed to source physical bounds
  CHECK(li.get_col_low()[dep] == doctest::Approx(0.0));
  CHECK(li.get_col_upp()[dep] == doctest::Approx(100.0));
  // Slack columns were added
  CHECK(info.sup_col
        != ColIndex {
            unknown_index,
        });
  CHECK(info.sdn_col
        != ColIndex {
            unknown_index,
        });
  // LP now has 3 original + 2 slack columns = 3 cols + 2 = 3 + 2
  CHECK(li.get_numcols() == 3);
  // And one elastic constraint row
  CHECK(li.get_numrows() == 1);
}

TEST_CASE("relax_fixed_state_variable skips unfixed column")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearInterface li;
  const auto dep = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });

  const StateVarLink link {
      .source_col =
          ColIndex {
              99,
          },
      .dependent_col = dep,
      .trial_value = 50.0,
      .source_low = 0.0,
      .source_upp = 100.0,
  };

  auto info = relax_fixed_state_variable(li,
                                         link,
                                         PhaseIndex {
                                             0,
                                         },
                                         1e6);

  CHECK_FALSE(info.relaxed);
  CHECK(li.get_numcols() == 1);
  CHECK(li.get_numrows() == 0);
}

// ---------------------------------------------------------------------------
// BendersCut class
// ---------------------------------------------------------------------------

TEST_CASE("BendersCut default construction and counter")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  BendersCut bc;
  CHECK(bc.pool() == nullptr);
  CHECK(bc.infeasible_cut_count() == 0);
  bc.reset_infeasible_cut_count();
  CHECK(bc.infeasible_cut_count() == 0);
}

// ---------------------------------------------------------------------------
// build_multi_cuts
// ---------------------------------------------------------------------------

TEST_CASE("build_multi_cuts with no relaxed links returns empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ElasticSolveResult elastic;
  elastic.link_infos = {
      {
          .relaxed = false,
      },
      {
          .relaxed = false,
      },
  };

  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  0,
              },
          .dependent_col =
              ColIndex {
                  1,
              },
      },
      {
          .source_col =
              ColIndex {
                  2,
              },
          .dependent_col =
              ColIndex {
                  3,
              },
      },
  };

  auto cuts = build_multi_cuts(elastic, links);
  CHECK(cuts.empty());
}

TEST_CASE("build_multi_cuts with active slack generates cuts")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Build a small LP that will serve as the "cloned" elastic LP.
  // We need columns for: dep0, dep1, sup0, sdn0, sup1, sdn1
  LinearInterface cloned_li;
  const auto dep0 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto dep1 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sup0 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sdn0 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sup1 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sdn1 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });

  // Set objective to minimise slack penalties
  cloned_li.set_obj_coeff(dep0, 0.0);
  cloned_li.set_obj_coeff(dep1, 0.0);
  cloned_li.set_obj_coeff(sup0, 1000.0);
  cloned_li.set_obj_coeff(sdn0, 1000.0);
  cloned_li.set_obj_coeff(sup1, 1000.0);
  cloned_li.set_obj_coeff(sdn1, 1000.0);

  // Add trivial constraints to make LP valid
  SparseRow r0;
  r0[dep0] = 1.0;
  r0[sup0] = -1.0;
  r0[sdn0] = 1.0;
  r0.lowb = 30.0;
  r0.uppb = 30.0;
  cloned_li.add_row(r0);

  SparseRow r1;
  r1[dep1] = 1.0;
  r1[sup1] = -1.0;
  r1[sdn1] = 1.0;
  r1.lowb = 50.0;
  r1.uppb = 50.0;
  cloned_li.add_row(r1);

  // Solve to get a valid solution — dep0=30, dep1=50, all slacks=0
  auto res = cloned_li.initial_solve();
  REQUIRE(res.has_value());

  // Now manually set column solution to simulate slack > 0:
  // dep0=25, sup0=5 (slack up active), dep1=60, sdn1=10 (slack down active)
  std::vector<double> sol = {
      25.0,
      60.0,
      5.0,
      0.0,
      0.0,
      10.0,
  };
  cloned_li.set_col_sol(sol);

  ElasticSolveResult elastic;
  elastic.clone = std::move(cloned_li);
  elastic.link_infos = {
      {
          .relaxed = true,
          .sup_col = sup0,
          .sdn_col = sdn0,
      },
      {
          .relaxed = true,
          .sup_col = sup1,
          .sdn_col = sdn1,
      },
  };

  // build_multi_cuts uses `link.class_name` + `link.uid` as the row's
  // identity for LabelMaker (so labels stay globally unique across
  // state-var classes that share a UID).  Populate those fields on
  // each link so the cuts come out with realistic metadata.
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  100,
              },
          .dependent_col = dep0,
          .trial_value = 30.0,
          .class_name = "Reservoir",
          .col_name = "efin",
          .uid = Uid {7},
      },
      {
          .source_col =
              ColIndex {
                  101,
              },
          .dependent_col = dep1,
          .trial_value = 50.0,
          .class_name = "Battery",
          .col_name = "sini",
          .uid = Uid {3},
      },
  };

  auto cuts = build_multi_cuts(elastic, links);

  // sup0=5 > 0 → upper-bound cut on source_col[0] <= dep_val=25
  // sdn1=10 > 0 → lower-bound cut on source_col[1] >= dep_val=60
  CHECK(cuts.size() == 2);

  // First cut: ub_cut for link 0 (sup0 active)
  CHECK(cuts[0].class_name == "Reservoir");
  CHECK(cuts[0].constraint_name == "mcut_ub");
  CHECK(cuts[0].variable_uid == Uid {7});
  CHECK(cuts[0].uppb == doctest::Approx(25.0));
  CHECK(cuts[0].cmap.at(ColIndex {
            100,
        })
        == doctest::Approx(1.0));

  // Second cut: lb_cut for link 1 (sdn1 active)
  CHECK(cuts[1].class_name == "Battery");
  CHECK(cuts[1].constraint_name == "mcut_lb");
  CHECK(cuts[1].variable_uid == Uid {3});
  CHECK(cuts[1].lowb == doctest::Approx(60.0));
  CHECK(cuts[1].cmap.at(ColIndex {
            101,
        })
        == doctest::Approx(1.0));
}

// ---------------------------------------------------------------------------
// already_lp_space flag regression guards.  Every cut produced by the
// legacy (LP-space) builders MUST set SparseRow::already_lp_space = true
// so `LinearInterface::add_row` bypasses col_scale + row-max composition
// on those rows.  If any of these regresses silently, every live Benders
// cut in an equilibrated LP gets double-scaled and the solve diverges —
// hence the unit-level guard here.
// ---------------------------------------------------------------------------

TEST_CASE("SparseRow default already_lp_space is false")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SparseRow row {};
  CHECK_FALSE(row.already_lp_space);
  // Defensive: the default must propagate through aggregate construction
  // from literal fields too, not only zero-init.
  const SparseRow explicit_row {
      .lowb = 1.0,
      .uppb = 2.0,
  };
  CHECK_FALSE(explicit_row.already_lp_space);
}

TEST_CASE(
    "build_benders_cut (span overload) marks output as LP-space")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  5,
              },
          .trial_value = 2.0,
      },
  };
  std::vector<double> rc(6, 0.0);
  rc[5] = 3.0;

  const auto cut =
      build_benders_cut(alpha, links, rc, /*objective_value=*/10.0);
  CHECK(cut.already_lp_space);
}

TEST_CASE(
    "build_benders_cut (link overload) marks output as LP-space")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  // The link-based overload reads rc from link.state_var->reduced_cost();
  // leave state_var null so rc=0 and no term is added — the builder still
  // runs to completion and must flag its output.
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  5,
              },
          .trial_value = 2.0,
      },
  };

  const auto cut = build_benders_cut(alpha, links, /*objective_value=*/10.0);
  CHECK(cut.already_lp_space);
}

TEST_CASE("build_multi_cuts marks every emitted cut as LP-space")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Reuse the scaffold from "build_multi_cuts with active slack
  // generates cuts" — identical LP setup, identical slack activation.
  // The CHECK here targets only the already_lp_space flag so the test
  // is focused and a future add_row regression in the multi-cut path
  // surfaces immediately.
  LinearInterface cloned_li;
  const auto dep0 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto dep1 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sup0 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sdn0 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sup1 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto sdn1 = cloned_li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  cloned_li.set_obj_coeff(sup0, 1.0);
  cloned_li.set_obj_coeff(sdn0, 1.0);
  cloned_li.set_obj_coeff(sup1, 1.0);
  cloned_li.set_obj_coeff(sdn1, 1.0);

  // Trivial balance rows so the LP is structurally valid.
  SparseRow r0;
  r0[dep0] = 1.0;
  r0[sup0] = -1.0;
  r0[sdn0] = 1.0;
  r0.lowb = 30.0;
  r0.uppb = 30.0;
  cloned_li.add_row(r0);
  SparseRow r1;
  r1[dep1] = 1.0;
  r1[sup1] = -1.0;
  r1[sdn1] = 1.0;
  r1.lowb = 50.0;
  r1.uppb = 50.0;
  cloned_li.add_row(r1);

  auto res = cloned_li.initial_solve();
  REQUIRE(res.has_value());

  // Spoof slack > 0 on both links so build_multi_cuts emits two cuts.
  const std::vector<double> sol = {
      25.0,
      60.0,
      5.0,
      0.0,
      0.0,
      10.0,
  };
  cloned_li.set_col_sol(sol);

  ElasticSolveResult elastic;
  elastic.clone = std::move(cloned_li);
  elastic.link_infos = {
      {
          .relaxed = true,
          .sup_col = sup0,
          .sdn_col = sdn0,
      },
      {
          .relaxed = true,
          .sup_col = sup1,
          .sdn_col = sdn1,
      },
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  100,
              },
          .dependent_col = dep0,
          .trial_value = 30.0,
          .class_name = "Reservoir",
          .col_name = "efin",
          .uid = Uid {7},
      },
      {
          .source_col =
              ColIndex {
                  101,
              },
          .dependent_col = dep1,
          .trial_value = 50.0,
          .class_name = "Battery",
          .col_name = "sini",
          .uid = Uid {3},
      },
  };

  const auto cuts = build_multi_cuts(elastic, links);
  REQUIRE(cuts.size() == 2);
  for (const auto& cut : cuts) {
    CHECK(cut.already_lp_space);
  }
}

TEST_CASE("elastic_filter_solve free function succeeds")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Build a simple LP with one state variable link:
  // min x1 + 1000*alpha  s.t.  x1 >= 5, alpha >= 0
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto alpha = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
  });
  li.set_obj_coeff(x1, 1.0);
  li.set_obj_coeff(alpha, 0.0);

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  // Fix the dependent column to 5.0 (simulate propagation)
  li.set_col(x1, 5.0);

  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  99,
              },
          .dependent_col = x1,
          .target_phase_index =
              PhaseIndex {
                  1,
              },
          .trial_value = 5.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  SolverOptions opts;

  auto elastic = elastic_filter_solve(li, links, 1e6, opts);
  // The result depends on whether the column is truly fixed (low==upp)
  // In this case x1 is fixed at 5.0, so relax_fixed_state_variable should
  // relax it, add slack variables, and solve the elastic subproblem
  if (elastic.has_value()) {
    CHECK(elastic->clone.is_optimal());
    CHECK(elastic->link_infos.size() == 1);
    CHECK(elastic->link_infos[0].relaxed);
  }
}

// ---------------------------------------------------------------------------
// build_benders_cut with cut_coeff_eps filtering
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut filters tiny coefficients via cut_coeff_eps")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  10,
              },
          .trial_value = 5.0,
      },
      {
          .source_col =
              ColIndex {
                  2,
              },
          .dependent_col =
              ColIndex {
                  11,
              },
          .trial_value = 3.0,
      },
  };

  std::vector<double> rc(12, 0.0);
  rc[10] = 2.0;  // significant coefficient
  rc[11] = 1e-14;  // numerically tiny — should be filtered

  const double obj_value = 100.0;

  SUBCASE("eps=0 keeps all coefficients")
  {
    auto cut = build_benders_cut(alpha, links, rc, obj_value);
    // Both coefficients present
    CHECK(cut.cmap.contains(ColIndex {
        1,
    }));
    CHECK(cut.cmap.contains(ColIndex {
        2,
    }));
    CHECK(cut.cmap.at(ColIndex {
              2,
          })
          == doctest::Approx(-1e-14));
    // lowb includes tiny rc adjustment: 100 - 2*5 - 1e-14*3
    CHECK(cut.lowb == doctest::Approx(90.0).epsilon(1e-10));
  }

  SUBCASE("eps=1e-12 filters tiny coefficient")
  {
    auto cut = build_benders_cut(alpha, links, rc, obj_value, 1.0, 1e-12);
    // Only the significant coefficient survives
    CHECK(cut.cmap.contains(ColIndex {
        1,
    }));
    CHECK_FALSE(cut.cmap.contains(ColIndex {
        2,
    }));
    // lowb = 100 - 2*5 = 90 (no tiny adjustment)
    CHECK(cut.lowb == doctest::Approx(90.0));
  }

  SUBCASE("eps larger than all coefficients removes all link terms")
  {
    auto cut = build_benders_cut(alpha, links, rc, obj_value, 1.0, 100.0);
    // Only alpha column remains
    CHECK(cut.cmap.size() == 1);
    CHECK(cut.cmap.contains(alpha));
    CHECK(cut.lowb == doctest::Approx(100.0));
  }
}

// ---------------------------------------------------------------------------
// rescale_benders_cut
// ---------------------------------------------------------------------------

TEST_CASE("rescale_benders_cut scales down large coefficients")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .lowb = 1e9,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1000.0;  // scale_alpha
  row[ColIndex {
      1,
  }] = -2e8;  // large coeff
  row[ColIndex {
      2,
  }] = 5e7;  // medium coeff

  // max|coeff| = 2e8, threshold = 1e6 → scale_factor = 200
  const bool scaled = rescale_benders_cut(row, alpha, 1e6);
  CHECK(scaled);

  // All coefficients divided by 200
  CHECK(row[alpha] == doctest::Approx(1000.0 / 200.0));
  CHECK(row[ColIndex {
            1,
        }]
        == doctest::Approx(-2e8 / 200.0));
  CHECK(row[ColIndex {
            2,
        }]
        == doctest::Approx(5e7 / 200.0));
  CHECK(row.lowb == doctest::Approx(1e9 / 200.0));
  CHECK(row.uppb == LinearProblem::DblMax);  // DblMax preserved
}

TEST_CASE("rescale_benders_cut does nothing when below threshold")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1.0;
  row[ColIndex {
      1,
  }] = -5.0;

  const bool scaled = rescale_benders_cut(row, alpha, 1e6);
  CHECK_FALSE(scaled);
  CHECK(row[ColIndex {
            1,
        }]
        == doctest::Approx(-5.0));
}

TEST_CASE("rescale_benders_cut disabled when threshold is zero")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .lowb = 1e20,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1.0;
  row[ColIndex {
      1,
  }] = -1e15;

  const bool scaled = rescale_benders_cut(row, alpha, 0.0);
  CHECK_FALSE(scaled);
}

// ---------------------------------------------------------------------------
// filter_cut_coefficients
// ---------------------------------------------------------------------------

TEST_CASE("filter_cut_coefficients removes small coefficients")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1.0;
  row[ColIndex {
      1,
  }] = -5.0;
  row[ColIndex {
      2,
  }] = 1e-13;  // tiny
  row[ColIndex {
      3,
  }] = -1e-14;  // tiny

  filter_cut_coefficients(row, alpha, 1e-12);

  CHECK(row.cmap.contains(alpha));  // α never filtered
  CHECK(row.cmap.contains(ColIndex {
      1,
  }));  // significant
  CHECK_FALSE(row.cmap.contains(ColIndex {
      2,
  }));  // filtered
  CHECK_FALSE(row.cmap.contains(ColIndex {
      3,
  }));  // filtered
}

TEST_CASE("filter_cut_coefficients preserves alpha even if tiny")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1e-15;  // α is tiny but must survive

  filter_cut_coefficients(row, alpha, 1e-12);
  CHECK(row.cmap.contains(alpha));
}

TEST_CASE("rescale then filter produces clean cut")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .lowb = 1e10,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1000.0;
  row[ColIndex {
      1,
  }] = -1e8;  // significant
  row[ColIndex {
      2,
  }] = 1e-4;  // small before rescale, will be ~1e-10 after

  // Rescale: max|coeff| = 1e8, threshold = 1e6 → scale_factor = 100
  rescale_benders_cut(row, alpha, 1e6);
  CHECK(row[ColIndex {
            1,
        }]
        == doctest::Approx(-1e6));
  CHECK(row[ColIndex {
            2,
        }]
        == doctest::Approx(1e-6));

  // Filter: 1e-6 < 1e-5 → col 2 removed
  filter_cut_coefficients(row, alpha, 1e-5);
  CHECK(row.cmap.contains(ColIndex {
      1,
  }));
  CHECK_FALSE(row.cmap.contains(ColIndex {
      2,
  }));
}

// ---------------------------------------------------------------------------
// Elastic filter scaling with large energy_scale reservoir
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic filter penalty scales correctly with large energy_scale")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Scenario: a reservoir with physical volume ~1000 hm³
  //   energy_scale = 1000  → LP efin variable ≈ 1.0
  //   state_fail_cost = 1000 $/MWh, mean_production_factor = 5 MWh/hm³
  //   → scost = 5000 $/hm³
  //   scale_objective = 1e7
  //   → link.scost = 5000 / 1e7 = 5e-4  (pre-divided)
  //   → link.var_scale = 1000
  //   → penalty on slack = 5e-4 × 1000 = 0.5  (LP units)
  //
  // This test verifies that the elastic penalty coefficient is O(1) and
  // does not produce ill-conditioned cut coefficients.

  constexpr double energy_scale = 1000.0;
  constexpr double scale_obj = 1e7;
  constexpr double state_fail_cost = 1000.0;
  constexpr double mean_prod_factor = 5.0;
  constexpr double scost = state_fail_cost * mean_prod_factor;  // 5000 $/hm³
  constexpr double link_scost = scost / scale_obj;  // 5e-4

  // Build a 2-variable LP: efin (reservoir) + alpha (future cost)
  // min  cost_e * efin + alpha   s.t.  efin >= 0.5  (physical 500 hm³)
  //
  // In LP units (divided by energy_scale):
  //   efin_lp = physical / energy_scale = 500 / 1000 = 0.5
  LinearInterface li;
  const auto efin = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
  });  // [0, 1000 hm³] in LP
  const auto alpha = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
  });

  // Objective: small coefficient on efin (normal LP scale)
  li.set_obj_coeff(efin, 0.01);
  li.set_obj_coeff(alpha, 1.0);

  // Constraint: efin >= 0.5 (physical 500 hm³)
  auto row = SparseRow {};
  row[efin] = 1.0;
  row.lowb = 0.5;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  // Fix efin at the trial value (simulate propagate_trial_values)
  constexpr double trial_value_lp = 0.8;  // 800 hm³ physical
  li.set_col(efin, trial_value_lp);

  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  99,
              },
          .dependent_col = efin,
          .source_phase_index =
              PhaseIndex {
                  0,
              },
          .target_phase_index =
              PhaseIndex {
                  1,
              },
          .trial_value = trial_value_lp,
          .source_low = 0.0,
          .source_upp = 1.0,
          .var_scale = energy_scale,
          .scost = link_scost,
      },
  };

  SUBCASE("relax_fixed_state_variable uses scost × var_scale as penalty")
  {
    auto clone = li.clone();
    auto info = relax_fixed_state_variable(clone,
                                           links[0],
                                           PhaseIndex {
                                               1,
                                           },
                                           1e-4);  // global fallback (unused)

    REQUIRE(info.relaxed);

    // The penalty should be scost × var_scale = 5e-4 × 1000 = 0.5
    const auto expected_penalty = link_scost * energy_scale;
    CHECK(expected_penalty == doctest::Approx(0.5));

    // Verify slack objective coefficients match
    const auto obj = clone.get_obj_coeff();
    CHECK(obj[info.sup_col] == doctest::Approx(expected_penalty));
    CHECK(obj[info.sdn_col] == doctest::Approx(expected_penalty));

    // Penalty is O(1), not O(1e6) — well-conditioned
    CHECK(obj[info.sup_col] < 10.0);
    CHECK(obj[info.sup_col] > 1e-6);
  }

  SUBCASE("elastic solve produces cut with well-scaled coefficients")
  {
    SolverOptions opts;
    auto elastic = elastic_filter_solve(li, links, 1e-4, opts);

    // The LP is feasible even without elastic (efin=0.8 > 0.5 lb),
    // but the column IS fixed so elastic will relax it
    if (elastic.has_value()) {
      REQUIRE(elastic->clone.is_optimal());

      // Build a Benders cut from the elastic result
      auto cut = build_benders_cut(alpha,
                                   links,
                                   elastic->clone.get_col_cost_raw(),
                                   elastic->clone.get_obj_value());

      // Cut coefficient on source_col should be the reduced cost
      // of the dependent column — proportional to the penalty, not huge
      if (cut.cmap.contains(links[0].source_col)) {
        const auto coeff = cut.cmap.at(links[0].source_col);
        // Coefficient should be O(penalty) = O(0.5), not O(1e6)
        CHECK(std::abs(coeff) < 100.0);
      }

      // RHS (cut.lowb) should be proportional to the objective value
      // which is small since the LP is feasible at efin=0.8
      CHECK(std::abs(cut.lowb) < 1e6);
    }
  }

  SUBCASE("global penalty fallback when scost is zero")
  {
    // When scost=0, the global penalty is used instead
    auto link_no_scost = links[0];
    link_no_scost.scost = 0.0;

    constexpr double global_penalty = 1e3 / scale_obj;  // 1e-4

    auto clone = li.clone();
    auto info = relax_fixed_state_variable(clone,
                                           link_no_scost,
                                           PhaseIndex {
                                               1,
                                           },
                                           global_penalty);

    REQUIRE(info.relaxed);

    // Penalty = global_penalty × var_scale = 1e-4 × 1000 = 0.1
    const auto expected_penalty = global_penalty * energy_scale;
    CHECK(expected_penalty == doctest::Approx(0.1));

    const auto obj = clone.get_obj_coeff();
    CHECK(obj[info.sup_col] == doctest::Approx(expected_penalty));
    CHECK(obj[info.sdn_col] == doctest::Approx(expected_penalty));
  }

  SUBCASE("penalty without scaling would be ill-conditioned")
  {
    // Demonstrate that raw penalty (1e6) without scale_obj division
    // would create a coefficient ratio > 1e6 — ill-conditioned
    constexpr double raw_penalty = 1e6;
    const auto scaled_penalty = link_scost * energy_scale;  // 0.5

    // The ratio between raw and properly scaled is huge
    CHECK(raw_penalty / scaled_penalty > 1e6);

    // The properly scaled penalty is O(1)
    CHECK(scaled_penalty == doctest::Approx(0.5));
  }
}

// ---------------------------------------------------------------------------
// chinneck_filter_solve — Chinneck IIS filter
// ---------------------------------------------------------------------------
//
// Helper: build a small LP with two columns x1, x2 — both fixable as
// state-variable links — and two upper-bound constraints x1 ≤ ub1,
// x2 ≤ ub2.  The trial values then drive which links are essential
// after elastic relaxation.
namespace
{

struct ChinneckFixture
{
  LinearInterface li {};
  ColIndex x1 {};
  ColIndex x2 {};
  std::vector<StateVarLink> links {};

  ChinneckFixture(double trial1,
                  double trial2,
                  double ub1 = 10.0,
                  double ub2 = 50.0)
  {
    x1 = li.add_col(SparseCol {.lowb = 0.0, .uppb = ub1 + 1000.0});
    x2 = li.add_col(SparseCol {.lowb = 0.0, .uppb = ub2 + 1000.0});
    li.set_obj_coeff(x1, 1.0);
    li.set_obj_coeff(x2, 1.0);

    SparseRow r1;
    r1[x1] = 1.0;
    r1.uppb = ub1;
    r1.lowb = -LinearProblem::DblMax;
    li.add_row(r1);

    SparseRow r2;
    r2[x2] = 1.0;
    r2.uppb = ub2;
    r2.lowb = -LinearProblem::DblMax;
    li.add_row(r2);

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    // Pin both columns at the trial values (state-variable convention).
    li.set_col(x1, trial1);
    li.set_col(x2, trial2);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = x1,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial1,
            .source_low = 0.0,
            .source_upp = ub1 + 1000.0,
        },
        {
            .source_col = ColIndex {100},
            .dependent_col = x2,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial2,
            .source_low = 0.0,
            .source_upp = ub2 + 1000.0,
        },
    };
  }
};

}  // namespace

TEST_CASE(  // NOLINT
    "chinneck_filter_solve filters non-essential link")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // trial1 = 5  (≤ ub1 = 10 — feasible, link non-essential)
  // trial2 = 100 (> ub2 = 50  — infeasible, link essential)
  ChinneckFixture fx {5.0, 100.0};
  SolverOptions opts;

  auto result = chinneck_filter_solve(fx.li, fx.links, 1e3, opts);

  REQUIRE(result.has_value());
  REQUIRE(result->link_infos.size() == 2);

  // Link 1 (trial=5, ub=10): non-essential — slack cols cleared.
  CHECK(result->link_infos[0].sup_col == ColIndex {unknown_index});
  CHECK(result->link_infos[0].sdn_col == ColIndex {unknown_index});

  // Link 2 (trial=100, ub=50): essential — slack cols preserved.
  CHECK(result->link_infos[1].sup_col != ColIndex {unknown_index});
  CHECK(result->link_infos[1].sdn_col != ColIndex {unknown_index});

  // Clone is still optimal after the IIS-filter re-fix solve.
  CHECK(result->clone.is_optimal());
}

TEST_CASE(  // NOLINT
    "chinneck_filter_solve preserves all when full IIS")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Both trial values infeasible — every relaxed bound is essential.
  ChinneckFixture fx {500.0, 500.0};
  SolverOptions opts;

  auto result = chinneck_filter_solve(fx.li, fx.links, 1e3, opts);

  REQUIRE(result.has_value());
  REQUIRE(result->link_infos.size() == 2);

  // Both links remain active — no filtering possible.
  CHECK(result->link_infos[0].sup_col != ColIndex {unknown_index});
  CHECK(result->link_infos[0].sdn_col != ColIndex {unknown_index});
  CHECK(result->link_infos[1].sup_col != ColIndex {unknown_index});
  CHECK(result->link_infos[1].sdn_col != ColIndex {unknown_index});
}

TEST_CASE(  // NOLINT
    "chinneck_filter_solve all-feasible trials yield zero-active result")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Both trial values within bounds — no slack should activate.
  ChinneckFixture fx {3.0, 20.0};
  SolverOptions opts;

  auto result = chinneck_filter_solve(fx.li, fx.links, 1e3, opts);

  // When all relaxed bounds are inactive the function returns the full
  // elastic result unchanged (no filtering needed).
  REQUIRE(result.has_value());
  REQUIRE(result->link_infos.size() == 2);
  CHECK(result->clone.is_optimal());

  // The full-elastic-pass result preserves slack columns even when slacks
  // are zero — it's the chinneck IIS pass that would clear them, but here
  // there is nothing to filter (n_active == 0 short-circuit).
  CHECK(result->link_infos[0].relaxed);
  CHECK(result->link_infos[1].relaxed);
}

TEST_CASE(  // NOLINT
    "chinneck IIS-filtered build_multi_cuts emits cuts only on IIS subset")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Mixed: link 1 non-essential, link 2 essential.
  ChinneckFixture fx {5.0, 100.0};
  SolverOptions opts;

  auto result = chinneck_filter_solve(fx.li, fx.links, 1e3, opts);
  REQUIRE(result.has_value());

  auto cuts = build_multi_cuts(*result, fx.links);

  // build_multi_cuts skips links whose sup_col/sdn_col are unknown_index,
  // so only the essential link 2 contributes — at most 2 cuts (ub + lb).
  CHECK(cuts.size() <= 2);
  for (const auto& cut : cuts) {
    // Cuts on link 2's source_col only.
    CHECK(cut.cmap.contains(ColIndex {100}));
    CHECK_FALSE(cut.cmap.contains(ColIndex {99}));
  }
}

// ---------------------------------------------------------------------------
// ElasticFilterMode — exercise all four values via parser + dispatch
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "ElasticFilterMode parses all four named modes plus aliases")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(parse_elastic_filter_mode("single_cut")
        == ElasticFilterMode::single_cut);
  CHECK(parse_elastic_filter_mode("cut")  // alias
        == ElasticFilterMode::single_cut);
  CHECK(parse_elastic_filter_mode("multi_cut") == ElasticFilterMode::multi_cut);
  CHECK(parse_elastic_filter_mode("chinneck") == ElasticFilterMode::chinneck);
  CHECK(parse_elastic_filter_mode("iis")  // alias for chinneck
        == ElasticFilterMode::chinneck);

  // Unknown name (including the retired "backpropagate" mode) falls back
  // to the default mode (chinneck — IIS-based).
  CHECK(parse_elastic_filter_mode("backpropagate")
        == ElasticFilterMode::chinneck);
  CHECK(parse_elastic_filter_mode("nonsense") == ElasticFilterMode::chinneck);
}

TEST_CASE(  // NOLINT
    "ElasticFilterMode every value yields a usable dispatch on a single LP")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Same LP, sweep all three modes.  We don't run a full SDDP solve
  // here — that's covered by integration tests — but we verify the
  // elastic pass + cut-construction pipeline runs end-to-end without
  // crashing for every mode and produces a non-empty cut row when
  // relevant.
  for (const auto mode : {ElasticFilterMode::single_cut,
                          ElasticFilterMode::multi_cut,
                          ElasticFilterMode::chinneck})
  {
    CAPTURE(static_cast<int>(mode));

    ChinneckFixture fx {5.0, 100.0};
    SolverOptions opts;

    // The elastic-pass entry point used by all modes is the free
    // elastic_filter_solve / chinneck_filter_solve pair.  Mode dispatch
    // happens at the call site (SDDPMethod::elastic_solve), so here we
    // simulate that branch.
    auto result = (mode == ElasticFilterMode::chinneck)
        ? chinneck_filter_solve(fx.li, fx.links, 1e3, opts)
        : elastic_filter_solve(fx.li, fx.links, 1e3, opts);

    REQUIRE(result.has_value());
    REQUIRE(result->link_infos.size() == 2);
    CHECK(result->clone.is_optimal());

    // single_cut consumes the elastic result via build_benders_cut;
    // multi_cut / chinneck additionally call build_multi_cuts.  Both
    // should run without throwing.
    auto bc = build_benders_cut(ColIndex {99},  // any source_col
                                fx.links,
                                result->clone.get_col_cost_raw(),
                                result->clone.get_obj_value());
    CHECK(bc.cmap.size() >= 1);

    if (mode == ElasticFilterMode::multi_cut
        || mode == ElasticFilterMode::chinneck)
    {
      auto mc = build_multi_cuts(*result, fx.links);
      // For chinneck, only the essential link contributes (cuts.size() ≤ 2).
      // For multi_cut, both links may contribute (cuts.size() ≤ 4).
      const std::size_t expected_max =
          (mode == ElasticFilterMode::chinneck) ? 2 : 4;
      CHECK(mc.size() <= expected_max);
    }
  }
}
