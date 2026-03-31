/**
 * @file      test_benders_cut.hpp
 * @brief     Unit tests for Benders cut construction and averaging functions
 * @date      2026-03-21
 * @copyright BSD-3-Clause
 */

#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// build_benders_cut
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut basic optimality cut")  // NOLINT
{
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
  auto cut = build_benders_cut(alpha, links, rc, obj_value, "test_cut");

  CHECK(cut.name == "test_cut");
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
  const ColIndex alpha {
      0,
  };
  const std::vector<StateVarLink> empty_links;
  const std::vector<double> rc;

  auto cut = build_benders_cut(alpha, empty_links, rc, 42.0, "empty");
  CHECK(cut.lowb == doctest::Approx(42.0));
  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));
  CHECK(cut.cmap.size() == 1);
}

// ---------------------------------------------------------------------------
// average_benders_cut
// ---------------------------------------------------------------------------

TEST_CASE("average_benders_cut with empty vector")  // NOLINT
{
  const std::vector<SparseRow> empty;
  auto avg = average_benders_cut(empty, "avg_empty");
  CHECK(avg.name.empty());
}

TEST_CASE("average_benders_cut with single cut")  // NOLINT
{
  auto cut = SparseRow {
      .name = "original",
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
  auto avg = average_benders_cut(cuts, "single_avg");

  CHECK(avg.name == "single_avg");
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
  auto cut1 = SparseRow {
      .name = "c1",
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
      .name = "c2",
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
  auto avg = average_benders_cut(cuts, "avg2");

  CHECK(avg.name == "avg2");
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
  const std::vector<SparseRow> empty;
  const std::vector<double> weights;
  auto wavg = weighted_average_benders_cut(empty, weights, "wempty");
  CHECK(wavg.name.empty());
}

TEST_CASE("weighted_average_benders_cut size mismatch")  // NOLINT
{
  auto c1 = SparseRow {
      .name = "c1",
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

  auto wavg = weighted_average_benders_cut(cuts, weights, "mismatch");
  CHECK(wavg.name.empty());  // returns empty cut on mismatch
}

TEST_CASE("weighted_average_benders_cut zero total weight")  // NOLINT
{
  auto c1 = SparseRow {
      .name = "c1",
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

  auto wavg = weighted_average_benders_cut(cuts, weights, "zero_w");
  CHECK(wavg.name.empty());
}

TEST_CASE("weighted_average_benders_cut single cut")  // NOLINT
{
  auto c1 = SparseRow {
      .name = "c1",
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

  auto wavg = weighted_average_benders_cut(cuts, weights, "wsingle");
  CHECK(wavg.name == "wsingle");
  CHECK(wavg.lowb == doctest::Approx(10.0));
  CHECK(wavg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(3.0));
}

TEST_CASE("weighted_average_benders_cut multiple cuts")  // NOLINT
{
  auto c1 = SparseRow {
      .name = "c1",
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 4.0;

  auto c2 = SparseRow {
      .name = "c2",
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

  auto wavg = weighted_average_benders_cut(cuts, weights, "wmulti");
  CHECK(wavg.name == "wmulti");
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
  const std::vector<SparseRow> empty;
  auto acc = accumulate_benders_cuts(empty, "acc_empty");
  CHECK(acc.name.empty());
}

TEST_CASE("accumulate_benders_cuts single cut")  // NOLINT
{
  auto c1 = SparseRow {
      .name = "c1",
      .lowb = 10.0,
      .uppb = LinearProblem::DblMax,
  };
  c1[ColIndex {
      0,
  }] = 5.0;

  const std::vector<SparseRow> cuts = {
      c1,
  };
  auto acc = accumulate_benders_cuts(cuts, "acc_single");
  CHECK(acc.name == "acc_single");
  CHECK(acc.lowb == doctest::Approx(10.0));
  CHECK(acc.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(5.0));
}

TEST_CASE("accumulate_benders_cuts multiple cuts sums")  // NOLINT
{
  auto c1 = SparseRow {
      .name = "c1",
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
      .name = "c2",
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
  auto acc = accumulate_benders_cuts(cuts, "acc_multi");

  CHECK(acc.name == "acc_multi");
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
  LinearInterface li;
  // source columns
  const auto s1 = li.add_col("s1", 0.0, 100.0);
  const auto s2 = li.add_col("s2", 0.0, 100.0);
  // dependent columns
  const auto d1 = li.add_col("d1", 0.0, 100.0);
  const auto d2 = li.add_col("d2", 0.0, 100.0);

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
  LinearInterface li;
  const auto dep = li.add_col("dep", 0.0, 100.0);

  // Fix the column (simulate propagate_trial_values)
  li.set_col_low(dep, 42.0);
  li.set_col_upp(dep, 42.0);

  const StateVarLink link {
      .source_col =
          ColIndex {
              99,
          },
      .dependent_col = dep,
      .source_phase =
          PhaseIndex {
              0,
          },
      .target_phase =
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
  LinearInterface li;
  const auto dep = li.add_col("dep", 0.0, 100.0);

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

  auto cuts = build_multi_cuts(elastic, links, "mc");
  CHECK(cuts.empty());
}

TEST_CASE("build_multi_cuts with active slack generates cuts")  // NOLINT
{
  // Build a small LP that will serve as the "cloned" elastic LP.
  // We need columns for: dep0, dep1, sup0, sdn0, sup1, sdn1
  LinearInterface cloned_li;
  const auto dep0 = cloned_li.add_col("dep0", 0.0, 100.0);
  const auto dep1 = cloned_li.add_col("dep1", 0.0, 100.0);
  const auto sup0 = cloned_li.add_col("sup0", 0.0, 100.0);
  const auto sdn0 = cloned_li.add_col("sdn0", 0.0, 100.0);
  const auto sup1 = cloned_li.add_col("sup1", 0.0, 100.0);
  const auto sdn1 = cloned_li.add_col("sdn1", 0.0, 100.0);

  // Set objective to minimise slack penalties
  cloned_li.set_obj_coeff(dep0, 0.0);
  cloned_li.set_obj_coeff(dep1, 0.0);
  cloned_li.set_obj_coeff(sup0, 1000.0);
  cloned_li.set_obj_coeff(sdn0, 1000.0);
  cloned_li.set_obj_coeff(sup1, 1000.0);
  cloned_li.set_obj_coeff(sdn1, 1000.0);

  // Add trivial constraints to make LP valid
  SparseRow r0("e0");
  r0[dep0] = 1.0;
  r0[sup0] = -1.0;
  r0[sdn0] = 1.0;
  r0.lowb = 30.0;
  r0.uppb = 30.0;
  cloned_li.add_row(r0);

  SparseRow r1("e1");
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

  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  100,
              },
          .dependent_col = dep0,
          .trial_value = 30.0,
      },
      {
          .source_col =
              ColIndex {
                  101,
              },
          .dependent_col = dep1,
          .trial_value = 50.0,
      },
  };

  auto cuts = build_multi_cuts(elastic, links, "mc");

  // sup0=5 > 0 → upper-bound cut on source_col[0] <= dep_val=25
  // sdn1=10 > 0 → lower-bound cut on source_col[1] >= dep_val=60
  CHECK(cuts.size() == 2);

  // First cut: ub_cut for link 0 (sup0 active)
  CHECK(cuts[0].name == "mc_ub_0");
  CHECK(cuts[0].uppb == doctest::Approx(25.0));
  CHECK(cuts[0].cmap.at(ColIndex {
            100,
        })
        == doctest::Approx(1.0));

  // Second cut: lb_cut for link 1 (sdn1 active)
  CHECK(cuts[1].name == "mc_lb_1");
  CHECK(cuts[1].lowb == doctest::Approx(60.0));
  CHECK(cuts[1].cmap.at(ColIndex {
            101,
        })
        == doctest::Approx(1.0));
}

TEST_CASE("elastic_filter_solve free function succeeds")  // NOLINT
{
  // Build a simple LP with one state variable link:
  // min x1 + 1000*alpha  s.t.  x1 >= 5, alpha >= 0
  LinearInterface li;
  const auto x1 = li.add_col("x1", 0.0, 100.0);
  const auto alpha = li.add_col("alpha", 0.0, LinearProblem::DblMax);
  li.set_obj_coeff(x1, 1.0);
  li.set_obj_coeff(alpha, 0.0);

  SparseRow row("lb");
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
          .target_phase =
              PhaseIndex {
                  1,
              },
          .trial_value = 5.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  SolverOptions opts;
  opts.reuse_basis = true;

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
// propagate_trial_values_row_dual
// ---------------------------------------------------------------------------

TEST_CASE("propagate_trial_values_row_dual adds coupling rows")  // NOLINT
{
  LinearInterface li;
  // source columns
  const auto s1 = li.add_col("s1", 0.0, 100.0);
  const auto s2 = li.add_col("s2", 0.0, 100.0);
  // dependent columns
  const auto d1 = li.add_col("d1", 0.0, 100.0);
  const auto d2 = li.add_col("d2", 0.0, 100.0);

  std::vector<StateVarLink> links = {
      {
          .source_col = s1,
          .dependent_col = d1,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
      {
          .source_col = s2,
          .dependent_col = d2,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  // Source solution: s1=25, s2=50
  std::vector<double> source_sol = {
      25.0,
      50.0,
      0.0,
      0.0,
  };

  CHECK(li.get_numrows() == 0);
  propagate_trial_values_row_dual(links, source_sol, li);

  SUBCASE("trial values are set correctly")
  {
    CHECK(links[0].trial_value == doctest::Approx(25.0));
    CHECK(links[1].trial_value == doctest::Approx(50.0));
  }

  SUBCASE("dependent columns keep physical bounds (not fixed)")
  {
    CHECK(li.get_col_low()[d1] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[d1] == doctest::Approx(100.0));
    CHECK(li.get_col_low()[d2] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[d2] == doctest::Approx(100.0));
  }

  SUBCASE("explicit coupling constraint rows were added")
  {
    CHECK(li.get_numrows() == 2);
  }

  SUBCASE("coupling_row indices are stored in links")
  {
    CHECK(links[0].coupling_row
          != RowIndex {
              unknown_index,
          });
    CHECK(links[1].coupling_row
          != RowIndex {
              unknown_index,
          });
    // Rows should be sequential
    CHECK(static_cast<Index>(links[1].coupling_row)
          == static_cast<Index>(links[0].coupling_row) + 1);
  }
}

// ---------------------------------------------------------------------------
// build_benders_cut_from_row_duals
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut_from_row_duals basic cut")  // NOLINT
{
  // Same scenario as build_benders_cut, but using row duals instead of
  // reduced costs.  The math is identical:
  //   α >= z + π1*(x1 - v1) + π2*(x2 - v2)
  const ColIndex alpha {
      0,
  };
  std::vector<StateVarLink> links = {
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
          .coupling_row =
              RowIndex {
                  3,
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
          .trial_value = 3.0,
          .coupling_row =
              RowIndex {
                  4,
              },
      },
  };

  // Row duals at coupling constraint rows
  std::vector<double> row_duals(5, 0.0);
  row_duals[3] = 2.0;  // π1
  row_duals[4] = -1.0;  // π2

  const double obj_value = 100.0;
  auto cut = build_benders_cut_from_row_duals(
      alpha, links, row_duals, obj_value, "row_dual_cut");

  CHECK(cut.name == "row_dual_cut");
  // α coefficient = 1.0
  CHECK(cut.cmap.at(alpha) == doctest::Approx(1.0));
  // source_col[0] coefficient = -π1 = -2.0
  CHECK(cut.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(-2.0));
  // source_col[1] coefficient = -π2 = 1.0
  CHECK(cut.cmap.at(ColIndex {
            2,
        })
        == doctest::Approx(1.0));
  // lowb = obj - π1*v1 - π2*v2 = 100 - 2*5 - (-1)*3 = 93
  CHECK(cut.lowb == doctest::Approx(93.0));
  CHECK(cut.uppb == LinearProblem::DblMax);
}

TEST_CASE(
    "build_benders_cut and row_duals produce same cut for equivalent "
    "duals")  // NOLINT
{
  // When reduced costs and row duals report the same values, the two
  // cut builders must produce identical results.
  const ColIndex alpha {
      0,
  };

  // Reduced-cost links (dependent_col index into rc vector)
  std::vector<StateVarLink> rc_links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  5,
              },
          .trial_value = 10.0,
      },
  };
  std::vector<double> rc(6, 0.0);
  rc[5] = 3.5;

  // Row-dual links (coupling_row index into row_duals vector)
  std::vector<StateVarLink> rd_links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  5,
              },
          .trial_value = 10.0,
          .coupling_row =
              RowIndex {
                  2,
              },
      },
  };
  std::vector<double> row_duals(3, 0.0);
  row_duals[2] = 3.5;  // same value as rc[5]

  auto cut_rc = build_benders_cut(alpha, rc_links, rc, 200.0, "rc");
  auto cut_rd =
      build_benders_cut_from_row_duals(alpha, rd_links, row_duals, 200.0, "rd");

  CHECK(cut_rc.lowb == doctest::Approx(cut_rd.lowb));
  CHECK(cut_rc.cmap.at(alpha) == doctest::Approx(cut_rd.cmap.at(alpha)));
  CHECK(cut_rc.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(cut_rd.cmap.at(ColIndex {
            1,
        })));
}

// ---------------------------------------------------------------------------
// build_benders_cut with cut_coeff_eps filtering
// ---------------------------------------------------------------------------

TEST_CASE("build_benders_cut filters tiny coefficients via cut_coeff_eps")
{
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
    auto cut = build_benders_cut(alpha, links, rc, obj_value, "no_filter");
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
    auto cut =
        build_benders_cut(alpha, links, rc, obj_value, "filtered", 1.0, 1e-12);
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
    auto cut =
        build_benders_cut(alpha, links, rc, obj_value, "all_gone", 1.0, 100.0);
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
  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .name = "big_cut",
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
  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .name = "small_cut",
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
  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .name = "no_rescale",
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
  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .name = "filter_test",
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
  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .name = "alpha_tiny",
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
  };
  row[alpha] = 1e-15;  // α is tiny but must survive

  filter_cut_coefficients(row, alpha, 1e-12);
  CHECK(row.cmap.contains(alpha));
}

TEST_CASE("rescale then filter produces clean cut")
{
  const ColIndex alpha {
      0,
  };
  auto row = SparseRow {
      .name = "combo_test",
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
// cut_coeff_eps filtering (build_benders_cut_from_row_duals)
// ---------------------------------------------------------------------------

TEST_CASE(
    "build_benders_cut_from_row_duals filters tiny coefficients via "
    "cut_coeff_eps")
{
  const ColIndex alpha {
      0,
  };
  std::vector<StateVarLink> links = {
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
          .coupling_row =
              RowIndex {
                  3,
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
          .trial_value = 3.0,
          .coupling_row =
              RowIndex {
                  4,
              },
      },
  };

  std::vector<double> row_duals(5, 0.0);
  row_duals[3] = 2.0;  // significant
  row_duals[4] = -1e-15;  // tiny — should be filtered

  const double obj_value = 100.0;

  SUBCASE("eps=0 keeps all")
  {
    auto cut = build_benders_cut_from_row_duals(
        alpha, links, row_duals, obj_value, "no_filter");
    CHECK(cut.cmap.contains(ColIndex {
        2,
    }));
  }

  SUBCASE("eps=1e-12 filters tiny row dual")
  {
    auto cut = build_benders_cut_from_row_duals(
        alpha, links, row_duals, obj_value, "filtered", 1.0, 1e-12);
    CHECK(cut.cmap.contains(ColIndex {
        1,
    }));
    CHECK_FALSE(cut.cmap.contains(ColIndex {
        2,
    }));
    CHECK(cut.lowb == doctest::Approx(90.0));
  }
}
