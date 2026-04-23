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
  // each link so the cuts come out with realistic metadata.  The
  // `source_low` / `source_upp` fields must enclose the dep_val so
  // the new bound-clamp logic in build_multi_cuts does not suppress
  // the cut as redundant with the source column's own bounds.
  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  100,
              },
          .dependent_col = dep0,
          .trial_value = 30.0,
          .source_low = 0.0,
          .source_upp = 1000.0,
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
          .source_low = 0.0,
          .source_upp = 1000.0,
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
// Regression test for the phase-28 mcut-exceeds-emax bug diagnosed on
// plp_2_years (2026-04-22).  The elastic filter emitted
//     reservoir_mcut_lb_65_*: reservoir_energy_65_* >= 917.47 LP
// against a column capped at the physical emax, causing the following
// forward-pass attempt on phase N-1 to be strictly infeasible.
//
// Fix: clamp each bound cut's RHS to the source column's physical
// [source_low, source_upp] box and drop cuts that would be trivially
// satisfied by the column's own bounds.
// ---------------------------------------------------------------------------
TEST_CASE(  // NOLINT
    "build_multi_cuts clamps cut RHS to source column physical bounds")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Tiny clone: 2 dep cols + 1 sup + 1 sdn per link.  We DON'T solve
  // the clone — instead we set its col_sol manually via set_col_sol
  // so the clone's `get_col_sol()` returns deterministic dep_val
  // values.  The relevant behaviour under test is the clamp logic on
  // those values against `StateVarLink::source_{low,upp}`.
  LinearInterface cloned_li;

  // link 0: dep_val = 917 physical, source box = [158, 371].
  // Under sdn-active: lb_cut.lowb = min(917, 371) = 371.  But 371 ==
  // source_upp so the cut is trivially non-binding → skipped.
  const auto dep0 = cloned_li.add_col(SparseCol {.uppb = 2000.0});
  const auto sup0 = cloned_li.add_col(SparseCol {.uppb = 100.0});
  const auto sdn0 = cloned_li.add_col(SparseCol {.uppb = 100.0});

  // link 1: dep_val = 100 physical, source box = [0, 1000].
  // Under sdn-active: lb_cut.lowb = min(100, 1000) = 100 > 0 → emit.
  const auto dep1 = cloned_li.add_col(SparseCol {.uppb = 2000.0});
  const auto sup1 = cloned_li.add_col(SparseCol {.uppb = 100.0});
  const auto sdn1 = cloned_li.add_col(SparseCol {.uppb = 100.0});

  // link 2: dep_val = -10 (phys) below source_low=5; sup-active path.
  // ub_cut.uppb = max(-10, 5) = 5 == source_low → cut collapses to
  // source_low, which is redundant with the column's lower bound; in
  // the strict-inequality logic (`ub < src_upp_phys`) this still emits
  // because 5 < 100, but the resulting bound is loose vs. column
  // lower.  That's fine — the rule is "don't embed infeasibility,"
  // not "strictly improve every time."
  const auto dep2 = cloned_li.add_col(SparseCol {.uppb = 2000.0});
  const auto sup2 = cloned_li.add_col(SparseCol {.uppb = 100.0});
  const auto sdn2 = cloned_li.add_col(SparseCol {.uppb = 100.0});

  // Populate the raw solution so get_col_sol_raw/get_col_sol return
  // deterministic values.  col_scale defaults to 1.0 so raw == phys.
  std::vector<double> sol(cloned_li.get_numcols(), 0.0);
  sol[dep0] = 917.0;  // above source_upp for link 0 → clamp to 371
  sol[dep1] = 100.0;  // inside link 1's source box → emit as-is
  sol[dep2] = -10.0;  // below source_low for link 2
  sol[sup0] = 0.0;
  sol[sdn0] = 10.0;  // sdn-active → lb_cut candidate on link 0
  sol[sup1] = 0.0;
  sol[sdn1] = 10.0;  // sdn-active → lb_cut candidate on link 1
  sol[sup2] = 10.0;  // sup-active → ub_cut candidate on link 2
  sol[sdn2] = 0.0;
  cloned_li.set_col_sol(sol);

  ElasticSolveResult elastic;
  elastic.clone = std::move(cloned_li);
  elastic.link_infos = {
      {.relaxed = true, .sup_col = sup0, .sdn_col = sdn0},
      {.relaxed = true, .sup_col = sup1, .sdn_col = sdn1},
      {.relaxed = true, .sup_col = sup2, .sdn_col = sdn2},
  };
  const std::vector<StateVarLink> links = {
      // RALCO-like: dep_val 917 above source_upp 371 → no cut
      {
          .source_col = ColIndex {100},
          .dependent_col = dep0,
          .source_low = 158.35,
          .source_upp = 371.03,
          .class_name = "Reservoir",
          .col_name = "efin",
          .uid = Uid {65},
      },
      // Healthy: dep_val 100 inside [0, 1000] → emit lb_cut @ 100
      {
          .source_col = ColIndex {101},
          .dependent_col = dep1,
          .source_low = 0.0,
          .source_upp = 1000.0,
          .class_name = "Reservoir",
          .col_name = "efin",
          .uid = Uid {6},
      },
      // Below source_low: dep_val -10 → ub_cut.uppb = max(-10,5) = 5,
      // still < source_upp=100 so emit (at the lower bound).
      {
          .source_col = ColIndex {102},
          .dependent_col = dep2,
          .source_low = 5.0,
          .source_upp = 100.0,
          .class_name = "Reservoir",
          .col_name = "efin",
          .uid = Uid {21},
      },
  };

  const auto cuts = build_multi_cuts(elastic, links);

  SUBCASE("no cut exceeds source_upp or falls below source_low")
  {
    for (const auto& cut : cuts) {
      // Find which link this cut targets (by the single source_col in
      // the cmap).
      REQUIRE(cut.cmap.size() == 1);
      const auto src_col = cut.cmap.begin()->first;
      // Match to the originating link.
      const auto it = std::ranges::find_if(links,
                                           [src_col](const StateVarLink& l)
                                           { return l.source_col == src_col; });
      REQUIRE(it != links.end());
      if (cut.constraint_name == "mcut_lb") {
        // source ≥ lowb: lowb must not exceed source_upp.
        CHECK(cut.lowb <= it->source_upp);
      } else if (cut.constraint_name == "mcut_ub") {
        // source ≤ uppb: uppb must not be below source_low.
        CHECK(cut.uppb >= it->source_low);
      }
    }
  }

  SUBCASE("RALCO-like link (dep_val 917 > source_upp 371) clamps to source_upp")
  {
    // Under sdn-active, the raw filter asked for `source ≥ 917` which
    // is infeasible against the physical emax of 371.  The clamp
    // emits `source ≥ 371` (= source_upp), which combined with the
    // column's own `≤ 371` forces `source = 371` — the tightest
    // FEASIBLE representation of "phase N needs maximal state".
    const auto it = std::ranges::find_if(
        cuts, [](const SparseRow& c) { return c.variable_uid == Uid {65}; });
    REQUIRE(it != cuts.end());
    CHECK(it->constraint_name == "mcut_lb");
    CHECK(it->lowb == doctest::Approx(371.03));
    // The critical invariant that prevents the phase-27 infeasibility:
    // lowb must NEVER exceed source_upp.
    CHECK(it->lowb <= 371.03);
  }

  SUBCASE("healthy link (dep_val within source box) emits exactly one lb_cut")
  {
    const auto it = std::ranges::find_if(
        cuts, [](const SparseRow& c) { return c.variable_uid == Uid {6}; });
    REQUIRE(it != cuts.end());
    CHECK(it->constraint_name == "mcut_lb");
    CHECK(it->lowb == doctest::Approx(100.0));
  }
}

// ---------------------------------------------------------------------------
// Multi-cut physical-space regression guard: post-migration,
// `build_multi_cuts` emits rows with physical-space dep_val bounds
// (via `get_col_sol()`) so `add_row` can fold col_scales + row-max.
// ---------------------------------------------------------------------------

TEST_CASE(
    "build_multi_cuts emits physical-space cuts (no already_lp_space flag)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Reuse the scaffold from "build_multi_cuts with active slack
  // generates cuts" — identical LP setup, identical slack activation.
  // Post-migration, multi-cut rows carry physical-space dep_val
  // bounds (via `get_col_sol()`) so `add_row` on the source LP folds
  // col_scales + per-row row-max like freshly-built Benders cuts;
  // this test guards against regressing back to LP-space bounds.
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
          .source_low = 0.0,
          .source_upp = 1000.0,
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
          .source_low = 0.0,
          .source_upp = 1000.0,
          .class_name = "Battery",
          .col_name = "sini",
          .uid = Uid {3},
      },
  };

  const auto cuts = build_multi_cuts(elastic, links);
  REQUIRE(cuts.size() == 2);
  for (const auto& cut : cuts) {
    CHECK(cut.scale == doctest::Approx(1.0));
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
// Elastic filter scaling with large energy_scale reservoir
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic filter slack costs are unit under Chinneck Phase-1")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Chinneck Phase-1 convention for SDDP feasibility cuts (see
  // `elastic_filter_solve` in source/benders_cut.cpp, PLP `plp-bc.f`,
  // and Chinneck (2008) "Feasibility and Infeasibility in
  // Optimization" § 4):
  //   - Every original objective coefficient is zeroed on the clone.
  //   - Slack variables added by `relax_fixed_state_variable` get
  //     cost = 1.0 regardless of link.scost or global penalty.
  //   - The relaxed LP's optimum = Σ(s⁺ + s⁻) = pure feasibility
  //     gap — independent of dispatch cost structure.
  //
  // Keeping original obj or scost-scaled slack cost would leak
  // state-dependent opex into the Benders feasibility-cut RHS and
  // drive α to diverge under SDDP iteration (observed on
  // juan/gtopt_iplp).  `link.scost` and the global `penalty`
  // argument are retained only for signature stability.

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

  SUBCASE("relax_fixed_state_variable adds unit-cost slacks")
  {
    auto clone = li.clone();
    auto info = relax_fixed_state_variable(clone,
                                           links[0],
                                           PhaseIndex {
                                               1,
                                           },
                                           1e-4);  // global fallback (unused)

    REQUIRE(info.relaxed);

    // Chinneck Phase-1: unit slack cost regardless of link.scost.
    const auto obj = clone.get_obj_coeff();
    CHECK(obj[info.sup_col] == doctest::Approx(1.0));
    CHECK(obj[info.sdn_col] == doctest::Approx(1.0));

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

  SUBCASE("slack cost remains unit when scost is zero")
  {
    // Under Chinneck Phase-1, neither scost nor the global penalty
    // parameter is consulted — slack cost is always 1.0.  Verify
    // with scost = 0 and a non-zero global penalty argument.
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

    const auto obj = clone.get_obj_coeff();
    CHECK(obj[info.sup_col] == doctest::Approx(1.0));
    CHECK(obj[info.sdn_col] == doctest::Approx(1.0));
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
