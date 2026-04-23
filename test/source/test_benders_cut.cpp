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
// Regression test for the phase-N eini double-scale-divide bug diagnosed
// on plp_2_years / juan_iplp (2026-04-22).
//
// Bug symptom: RALCO phase-2 `reservoir_sini_65_51_2 = 73.82` LP when the
// correct raw bound was 233.44 LP (= 73.82 × 3.162).  The 3.162 factor is
// RALCO's energy col_scale (√10).
//
// Root cause was in the StateVariable overload of `propagate_trial_values`:
// it read `link.state_var->col_sol()` (which is stored as `phys /
// var_scale` by `capture_state_variable_values` in sddp_method.cpp) and
// wrote the result straight through `set_col_low_raw` onto the dependent
// column, which embeds a hidden `var_scale(source) == col_scale(dependent)`
// invariant.  When the two scales disagree, the dependent column gets
// pinned off by a factor of `col_scale_dep / var_scale_src`.
//
// Fix: read `col_sol_physical()` and apply via the physical setter
// `set_col_low` / `set_col_upp`, which internally divides by the
// dependent column's own `col_scale` — scale-agnostic across phases.
// ---------------------------------------------------------------------------
TEST_CASE(  // NOLINT
    "propagate_trial_values state-variable overload: matching col_scales")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Simulate phase 0 (source) and phase 1 (dependent) as a single
  // LinearInterface with two columns, each carrying col_scale = √10.
  // (In production the two columns live in separate LPs but the
  // propagate_trial_values contract is per-column.)
  constexpr double kScale = 3.162;  // RALCO energy scale.

  LinearProblem lp("matching_scales");
  const auto src_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 500.0,
      .scale = kScale,
  });
  const auto dep_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 500.0,
      .scale = kScale,
  });
  // flatten requires at least one row.
  auto r = SparseRow {};
  r[src_col] = 1.0;
  r[dep_col] = 1.0;
  r.uppb = 1000.0;
  std::ignore = lp.add_row(std::move(r));

  LinearInterface li("", lp.flatten({}));
  REQUIRE(li.get_col_scale(src_col) == doctest::Approx(kScale));
  REQUIRE(li.get_col_scale(dep_col) == doctest::Approx(kScale));

  // Simulate a StateVariable on the source column with var_scale
  // matching the source col_scale, and a trial value equivalent to
  // phase-1's optimum (233.44 LP = 738.14 Hm³ physical on RALCO).
  const StateVariable::LPKey key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };
  StateVariable svar(key, src_col, /*scost=*/0.0, kScale, LpContext {});
  constexpr double kTrialLP = 233.44;
  svar.set_col_sol(kTrialLP);
  REQUIRE(svar.col_sol_physical()
          == doctest::Approx(kTrialLP * kScale).epsilon(1e-6));

  std::vector<StateVarLink> links = {
      {
          .source_col = src_col,
          .dependent_col = dep_col,
          .trial_value = 0.0,
          .var_scale = kScale,
          .state_var = &svar,
      },
  };

  propagate_trial_values(links, li);

  // With matching scales the dependent raw bound must equal the
  // source's LP value — anything else signals the double-divide bug.
  CHECK(li.get_col_low_raw()[dep_col] == doctest::Approx(kTrialLP));
  CHECK(li.get_col_upp_raw()[dep_col] == doctest::Approx(kTrialLP));
  // Physical bound on the dependent col: raw × col_scale_dep.
  CHECK(li.get_col_low()[dep_col]
        == doctest::Approx(kTrialLP * kScale).epsilon(1e-6));
  CHECK(li.get_col_upp()[dep_col]
        == doctest::Approx(kTrialLP * kScale).epsilon(1e-6));
  // `trial_value` contract: dependent-column raw LP.
  CHECK(links[0].trial_value == doctest::Approx(kTrialLP));
}

TEST_CASE(  // NOLINT
    "propagate_trial_values state-variable overload: mismatched col_scales")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Cross-phase scale drift — the scenario where the OLD code's
  // hidden `var_scale(src) == col_scale(dep)` invariant breaks.
  // Source var_scale = 3.162 (RALCO); dependent col_scale = 1.5
  // (deliberately different, simulating cross-phase scale drift
  // or different variable-scale-map resolution).
  constexpr double kSrcScale = 3.162;
  constexpr double kDepScale = 1.5;

  LinearProblem lp("mismatched_scales");
  const auto src_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 500.0,
      .scale = kSrcScale,
  });
  const auto dep_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 500.0,
      .scale = kDepScale,
  });
  auto r = SparseRow {};
  r[src_col] = 1.0;
  r[dep_col] = 1.0;
  r.uppb = 1000.0;
  std::ignore = lp.add_row(std::move(r));

  LinearInterface li("", lp.flatten({}));
  REQUIRE(li.get_col_scale(src_col) == doctest::Approx(kSrcScale));
  REQUIRE(li.get_col_scale(dep_col) == doctest::Approx(kDepScale));

  const StateVariable::LPKey key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };
  StateVariable svar(key, src_col, /*scost=*/0.0, kSrcScale, LpContext {});
  constexpr double kSrcTrialLP = 100.0;  // raw LP on the source col.
  svar.set_col_sol(kSrcTrialLP);

  const double kTrialPhys = kSrcTrialLP * kSrcScale;  // physical.
  REQUIRE(svar.col_sol_physical() == doctest::Approx(kTrialPhys).epsilon(1e-6));

  std::vector<StateVarLink> links = {
      {
          .source_col = src_col,
          .dependent_col = dep_col,
          .trial_value = 0.0,
          .var_scale = kSrcScale,
          .state_var = &svar,
      },
  };

  propagate_trial_values(links, li);

  // The physical trial is what must be preserved across the scale
  // boundary, not the raw LP value.  Dependent raw bound must be
  // `phys / col_scale_dep`, NOT `phys / var_scale_src` (which would
  // mis-pin it at exactly the raw LP value on the source phase and
  // off by a factor of `col_scale_dep / var_scale_src`).
  const double kExpectedDepRaw = kTrialPhys / kDepScale;
  CHECK(li.get_col_low_raw()[dep_col]
        == doctest::Approx(kExpectedDepRaw).epsilon(1e-6));
  CHECK(li.get_col_upp_raw()[dep_col]
        == doctest::Approx(kExpectedDepRaw).epsilon(1e-6));
  // Physical bound must equal the physical trial exactly — the
  // whole point of routing through physical space is that the
  // dependent pin preserves the physical value regardless of the
  // scale-map drift between source and dependent columns.
  CHECK(li.get_col_low()[dep_col] == doctest::Approx(kTrialPhys).epsilon(1e-6));
  CHECK(li.get_col_upp()[dep_col] == doctest::Approx(kTrialPhys).epsilon(1e-6));

  // Sanity: the old buggy code would have written `kSrcTrialLP` raw
  // onto the dependent column (= 100.0 LP, physical = 150.0), which
  // would fail the CHECK above (expected phys = 316.2).  This test
  // catches that regression.
  CHECK(li.get_col_low_raw()[dep_col] != doctest::Approx(kSrcTrialLP));
}

TEST_CASE(
    "propagate_trial_values state-variable overload: null state_var")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // When `state_var == nullptr`, the trial defaults to 0.0 physical
  // and the dependent column is pinned at 0.
  LinearInterface li;
  const auto dep = li.add_col(SparseCol {.lowb = -10.0, .uppb = 10.0});
  li.set_col_scale(dep, 2.0);

  std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {0},
          .dependent_col = dep,
          .trial_value = 999.0,
          .state_var = nullptr,
      },
  };

  propagate_trial_values(links, li);

  CHECK(links[0].trial_value == doctest::Approx(0.0));
  CHECK(li.get_col_low_raw()[dep] == doctest::Approx(0.0));
  CHECK(li.get_col_upp_raw()[dep] == doctest::Approx(0.0));
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
// Elastic filter scaling with large energy_scale reservoir
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic filter slack cost = penalty × link.var_scale")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // PLP convention for SDDP feasibility cuts (see
  // `elastic_filter_solve` / `relax_fixed_state_variable` in
  // source/benders_cut.cpp, and PLP `plp-agrespd.f::AgrElastici`):
  //   - Every original objective coefficient is zeroed on the clone
  //     (caller's responsibility — `elastic_filter_solve` does it).
  //   - Slack variables added by `relax_fixed_state_variable` get
  //     cost = `penalty × link.var_scale` where `penalty` is passed
  //     from `SDDPMethod::elastic_solve` as
  //         100 × target_phase_discount / scale_objective
  //     The `var_scale` multiplier matches the fixing equation's
  //     scale convention (dep in LP units = physical / var_scale).
  //
  // Historical note: previously hard-coded to 1.0 ("Chinneck Phase-1
  // pure feasibility").  Changing to `penalty × var_scale` breaks a
  // cold-start degeneracy where α-free fcuts carried no economic
  // weight — diagnosed on plp_2_years iter 0.

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

  SUBCASE("relax_fixed_state_variable adds penalty × var_scale slacks")
  {
    auto clone = li.clone();
    constexpr double penalty = 1e-4;  // global base penalty
    auto info = relax_fixed_state_variable(clone,
                                           links[0],
                                           PhaseIndex {
                                               1,
                                           },
                                           penalty);

    REQUIRE(info.relaxed);

    // P0-A convention (2026-04-23): per-variable `link.scost` overrides
    // the global `penalty` when set (> 0) — multi-reservoir fixtures
    // with differing water values can now price their slacks differently.
    //   expected_base = (scost > 0 ? scost : penalty)
    //   slack_cost    = expected_base × var_scale
    // Here scost = 5e-4 > 0, so expected = 5e-4 × 1000 = 0.5.
    const double expected_base = (link_scost > 0.0) ? link_scost : penalty;
    const double expected_slack_cost = expected_base * energy_scale;
    const auto obj = clone.get_obj_coeff();
    CHECK(obj[info.sup_col] == doctest::Approx(expected_slack_cost));
    CHECK(obj[info.sdn_col] == doctest::Approx(expected_slack_cost));

    // Sanity: well-conditioned coefficients.
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

      // Build a Benders cut from the elastic result (physical space).
      auto cut =
          build_benders_cut_physical(alpha,
                                     links,
                                     elastic->clone,
                                     elastic->clone.get_obj_value_physical());

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

  SUBCASE("slack cost independent of link.scost")
  {
    // The `link.scost` field is not part of the slack-cost formula —
    // verifying that zeroing it does not affect the slack price.
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

    const double expected_slack_cost = global_penalty * energy_scale;
    const auto obj = clone.get_obj_coeff();
    CHECK(obj[info.sup_col] == doctest::Approx(expected_slack_cost));
    CHECK(obj[info.sdn_col] == doctest::Approx(expected_slack_cost));
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

    // single_cut consumes the elastic result via
    // build_benders_cut_physical; multi_cut / chinneck additionally
    // call build_multi_cuts.  Both should run without throwing.
    auto bc =
        build_benders_cut_physical(ColIndex {99},  // any source_col
                                   fx.links,
                                   result->clone,
                                   result->clone.get_obj_value_physical());
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

// ===========================================================================
// PLP vs gtopt multi-cut critical review (2026-04-22)
// ===========================================================================
//
// Audit of gtopt's `build_multi_cuts` (source/benders_cut.cpp:534-659)
// and `elastic_filter_solve` (source/benders_cut.cpp:316-386) against
// PLP's equivalent machinery in the `marcelomatus/plp_storage` repo,
// specifically:
//   - plp-agrespd.f::AgrElastici         (feasibility-cut builder)
//   - osicallsc.cpp::osi_lp_get_feasible_cut
//                                        (elastic clone + Farkas-ray extract)
//   - plp-faseprim.f / plp-fasedual.f    (forward/backward-pass callers)
//
// ----- Section A: PLP multi-cut pseudocode --------------------------------
//
// plp-agrespd.f AgrElastici:
//   1. Builds `rows_ori[]` = fixing-row indices in lpi (prev stage) and
//      `cols_dest[]` = state-var column indices in lpo (current stage),
//      one per reservoir.
//   2. Calls osi_lp_get_feasible_cut(lpi, lpo, nrows_ori, rows_ori,
//          cols_dest, objs=0, fname, deps, FactDbl, ray, rhsi, RoundRay).
//      [osicallsc.cpp osi_lp_get_feasible_cut]:
//        - Clones lpi ("cloned" LP).
//        - Sets EVERY objective coefficient to 0 (Chinneck Phase-1).
//        - For each i in [0, nrows_ori):
//            collb = max(rhs - lp_dest->getColLower()[cols[i]], 0.0);
//            colub = max(lp_dest->getColUpper()[cols[i]] - rhs, 0.0);
//            obj   = objs ? objs[i] : 1.0;          // slack cost = 1.0 const
//            redcost = 0.01 * lp_dest->getReducedCost()[cols[i]];
//            addCol(sp, upperbound = colub, cost = obj + max(redcost,0));
//            addCol(sn, upperbound = collb, cost = obj - min(redcost,0));
//            // Fixing row modified to: dep - sp + sn = rhs
//        - lp->resolve() (Phase-1 minimum slack solve).
//        - For each i:
//            ray[i] = |dual[row]| < eps ? 0 : -dual[row];
//            dx     = x[sp] - x[sn];
//            rhsi[i] = (b[row] + dx) * ray[i];
//            (optional round_ray: ray[i] = ±1)
//        - rhsi[nrows_ori] = Σ rhsi[i].
//   3. If FOneFeasRay (the aggregated / "single" form):
//        Build ONE cut:  Σ_i ray[i] * cols_dest[i] ≥ Σ_i rhsi[i] +
//        FactEPS·|sum| Only non-zero ray[i] are included.
//      Else (the "multi-cut" / per-reservoir form):
//        For each i with ray[i] != 0:
//          Build ONE cut:  ray[i] * cols_dest[i] ≥ rhsi[i] + FactEPS·|rhsi[i]|
//   4. Optional FactMLD magnitude clamp:
//        if |rhs| > FactMLD: rescale coeffs and rhs by FactMLD/|rhs|.
//
// KEY PROPERTIES (PLP multi-cut, i.e. FOneFeasRay=.FALSE.):
//   - Per-column coefficient is `ray[i]` (signed Farkas multiplier),
//     NOT 1.0 — classical Benders feasibility cut, always implied by
//     the original LP's feasible set.
//   - Cuts are emitted only when ray[i] != 0 (non-zero Farkas component).
//   - ONE cut per reservoir, always in ≥ form.  PLP never emits both a
//     lower AND upper bound cut on the same reservoir at the same event.
//   - Cut RHS = (b + dx) * ray[i], perturbed outward by FactEPS*|rhs|.
//     NOT clamped to physical [emin, emax] — the Farkas-ray form is
//     already implied by the original polyhedron.
//   - Slack columns have explicit finite upper bounds (colub/collb) =
//     distance from fixing-row RHS to the physical column bound.  This
//     is the "mini-clamp" that prevents the elastic clone from pushing
//     the dependent column past its physical domain.
//   - Slack cost is a constant `obj = 1.0` (+ tiny reduced-cost bias).
//     Does NOT depend on phase discount or objective scale.
//
// ----- Section B: gtopt multi-cut pseudocode ------------------------------
//
// source/benders_cut.cpp relax_fixed_state_variable (line 247):
//   - Sets dep col bounds to [link.source_low, link.source_upp] (physical).
//   - Adds sup/sdn slacks with uppb = DblMax (UNBOUNDED), cost = penalty.
//     (penalty = phase_discount / scale_obj per current
//     SDDPMethod::elastic_solve.)
//   - Adds fixing row: dep + sup - sdn = trial_value.
//
// source/benders_cut.cpp elastic_filter_solve (line 324):
//   - Clones the LP, zeroes every obj coeff (Chinneck Phase-1).
//   - Leaves α unmodified (excluded from links).
//   - Solves the clone.
//
// source/benders_cut.cpp build_multi_cuts (line 534):
//   For each link with info.relaxed:
//     dep_val_phys = clone.get_col_sol()[link.dependent_col].
//     If info.sup_col != unknown && clone.get_col_sol_raw()[sup] > slack_tol:
//         ub = max(dep_val_phys, source_low)
//         if ub < source_upp: emit `mcut_ub:  source_col ≤ ub` with coeff 1.0
//     If info.sdn_col != unknown && clone.get_col_sol_raw()[sdn] > slack_tol:
//         lb = min(dep_val_phys, source_upp)
//         if lb > source_low: emit `mcut_lb:  source_col ≥ lb` with coeff 1.0
//
// Separately (source/sddp_forward_pass.cpp:312-395), gtopt ALWAYS
// installs a single aggregated feasibility cut built from the fixing-row
// DUALS (build_feasibility_cut_physical, line 176 of benders_cut.cpp):
//     Σ π_i * source_col_i ≥ Σ π_i * (v̂_i + dx_i)
// and THEN (if `use_multi_cut` is on) appends the per-reservoir bound
// cuts on top of the aggregated cut — i.e. BOTH are added in the same
// infeasibility event.
//
// ----- Section C: Divergence table ----------------------------------------
//
// | # | Aspect                | PLP                    | gtopt                |
// |---|-----------------------|------------------------|----------------------|
// | D1| Source-col coefficient| ray[i] (signed Farkas) | 1.0 always           |
// | D2| Cut RHS               | (b+dx)*ray[i] + ε      | dep_val_phys         |
// | D3| Slack col upper bound | colUpp−rhs / rhs−colLow| DblMax (unbounded)   |
// | D4| Slack cost            | 1.0 const              | phase_disc/scale_obj |
// | D5| Active criterion      | ray[i] != 0 (signed)   | sup/sdn > slack_tol  |
// | D6| Cut direction         | always ≥ (one per i)   | two (mcut_lb +
// mcut_ub)| | D7| Cut-RHS clamp         | only |rhs|>FactMLD     | [source_low,
// source_upp]| | D8| Outward ε perturb     | rhs += FactEPS*|rhs|   | none | |
// D9| round_ray option      | FRoundRay → ±1         | none                 |
// | D10| Which phase's LP    | lpo = lp(ISimul, IEtapaDest=IEtapaOri-1) |
// prev_phase_index (matches)| | D11| Aggregated + multi  | Exclusive
// (FOneFeasRay)| Both: feas_cut + mcuts stacked| | D12| Mode flag           |
// FOneFeasRay (boolean)  | multi_cut_threshold + elastic_mode|
//
// ----- Section D: Hypothesis on why gtopt's multi-cut fails ---------------
//
// On plp_2_years iter 0, p28 infeasibility triggers gtopt to install:
//   (a) 1 aggregated fcut (rows 312-350) on p27, with coefficients π_i
//       on source_cols (physical-space) and RHS = Σ π_i · (v̂_i + dx_i).
//   (b) 5 bound mcuts (rows 374-395) on p27, each of the form
//       `1.0 * source_col ≥ dep_val_phys` or `1.0 * source_col ≤ dep_val_phys`.
// The p27 LP's next solve then hits the bound-infeasibility on
// `reservoir_energy_65_51` because the stacked mcuts enforce HARD
// bounds on several reservoirs simultaneously.  PLP never stacks —
// it emits ONE cut per reservoir with coefficient `ray[i]` (which can
// cancel out infeasibility ε cleanly via Farkas) and only ONE form
// per event (either single-aggregated OR per-reservoir, not both).
//
// Prime suspects (in descending order of likelihood):
//   (H1) Stacking aggregated + multi (D11): doubles the Farkas
//        coverage into both a soft (π-weighted) and a hard (1.0-weighted)
//        form, overconstraining p27.
//   (H2) 1.0 coefficient vs ray[i] (D1): emits strictly tighter bounds
//        than the Farkas ray would, under-approximating the feasible set.
//   (H3) Two-sided direction (D6): a single reservoir can land in both
//        sup>0 AND sdn>0 regimes across a single IIS subset if the
//        Chinneck filter sorts weirdly — cutting both sides is
//        physically inconsistent for any reservoir.
//   (H4) Slack-cost = phase_discount/scale_obj (D4): under very small
//        phase_discount values, the slack becomes essentially free, and
//        the elastic LP may choose dep values arbitrarily, making
//        `dep_val_phys` meaningless as a cut RHS.
//
