
#include <doctest/doctest.h>
#include <fmt/ranges.h>
#include <gtopt/linear_problem.hpp>

// using gtopt::SparseCol;

struct SparseCol
{
  std::string name {};
  double lowb {};
  double uppb {gtopt::CoinDblMax};
  double cost {};
  bool integer {};
};

TEST_CASE("Linear problem test 0")
{
  SparseCol col({.name = "hola", .cost = 1});
  REQUIRE(col.name == "hola");
  REQUIRE(col.cost == doctest::Approx(1));
}

TEST_CASE("Linear problem test 1")
{
  gtopt::SparseRow row({.name = "r1"});
  REQUIRE(row.name == "r1");
  REQUIRE(row.size() == 0);

  row[3] = 1;
  row[5] = 2;

  REQUIRE(row.size() == 2);
  REQUIRE(row.get_coeff(0) == doctest::Approx(0));
  REQUIRE(row.get_coeff(3) == doctest::Approx(1));
  REQUIRE(row.get_coeff(5) == doctest::Approx(2));
  REQUIRE(row.get_coeff(7) == doctest::Approx(0));

  REQUIRE(row.size() == 2);
}

TEST_CASE("Linear problem test 2")
{
  {
    gtopt::SparseCol col({.name = "c1"});
    REQUIRE(col.name == "c1");

    REQUIRE(col.lowb == doctest::Approx(0));
    REQUIRE(col.uppb == doctest::Approx(gtopt::CoinDblMax));
  }

  {
    gtopt::SparseCol col({.name = "c1", .lowb = 25, .uppb = 25});
    REQUIRE(col.name == "c1");

    REQUIRE(col.lowb == doctest::Approx(25));
    REQUIRE(col.uppb == doctest::Approx(25));
    REQUIRE(col.cost == doctest::Approx(0));
  }

  {
    SparseCol col("c1", -25, 25, 10);
    REQUIRE(col.name == "c1");

    REQUIRE(col.lowb == doctest::Approx(-25));
    REQUIRE(col.uppb == doctest::Approx(25));
    REQUIRE(col.cost == doctest::Approx(10));
  }
}

TEST_CASE("Linear problem test 3")
{
  using flat_lp_t = gtopt::FlatLinearProblem;

  {
    gtopt::LinearProblem lp;
    auto&& flat_lp = lp.to_flat();

    REQUIRE(flat_lp.ncols == 0);
    REQUIRE(flat_lp.nrows == 0);

    const flat_lp_t flat_lp2 = {};

    REQUIRE(flat_lp.ncols == flat_lp2.ncols);
    REQUIRE(flat_lp.nrows == flat_lp2.nrows);
    REQUIRE(flat_lp.matbeg == flat_lp2.matbeg);
    REQUIRE(flat_lp.matind == flat_lp2.matind);
    REQUIRE(flat_lp.matval == flat_lp2.matval);
    REQUIRE(flat_lp.collb == flat_lp2.collb);
    REQUIRE(flat_lp.colub == flat_lp2.colub);
    REQUIRE(flat_lp.objval == flat_lp2.objval);
    REQUIRE(flat_lp.rowlb == flat_lp2.rowlb);
    REQUIRE(flat_lp.rowub == flat_lp2.rowub);
    REQUIRE(flat_lp.colint == flat_lp2.colint);
    REQUIRE(flat_lp.colnm == flat_lp2.colnm);
    REQUIRE(flat_lp.rownm == flat_lp2.rownm);
    REQUIRE(flat_lp.colmp == flat_lp2.colmp);
    REQUIRE(flat_lp.rowmp == flat_lp2.rowmp);
    REQUIRE(flat_lp.name == flat_lp2.name);
  }

  gtopt::LinearProblem lp("SEN");

  REQUIRE(lp.get_numrows() == 0);
  REQUIRE(lp.get_numcols() == 0);

  const auto col1 = lp.add_col({.name = "col1"});
  const auto col2 = lp.add_col(std::move(gtopt::SparseCol("col2").free()));

  REQUIRE(lp.get_numrows() == 0);
  REQUIRE(lp.get_numcols() == 2);

  REQUIRE(lp.col_at(col1).name == "col1");
  REQUIRE(lp.col_at(col1).lowb == doctest::Approx(0));
  REQUIRE(lp.col_at(col1).uppb == doctest::Approx(gtopt::CoinDblMax));

  REQUIRE(lp.col_at(col2).name == "col2");
  REQUIRE(lp.col_at(col2).lowb == doctest::Approx(-gtopt::CoinDblMax));
  REQUIRE(lp.col_at(col2).uppb == doctest::Approx(gtopt::CoinDblMax));

  lp.col_at(col1).lowb = -10;
  lp.col_at(col2).lowb = 0;

  lp.col_at(col1).uppb = 100;
  lp.col_at(col2).uppb = 200;

  const auto row1 = lp.add_row(gtopt::SparseRow("row1"));
  const auto row2 = lp.add_row(gtopt::SparseRow("row2"));

  lp.row_at(row1).uppb = 25;
  lp.row_at(row2).uppb = 35;

  REQUIRE(lp.get_numrows() == 2);
  REQUIRE(lp.get_numcols() == 2);

  REQUIRE(lp.row_at(row1).name == "row1");
  REQUIRE(lp.row_at(row1).lowb == doctest::Approx(0));
  REQUIRE(lp.row_at(row1).uppb == doctest::Approx(25));

  REQUIRE(lp.row_at(row2).name == "row2");
  REQUIRE(lp.row_at(row2).lowb == doctest::Approx(0));
  REQUIRE(lp.row_at(row2).uppb == doctest::Approx(35));

  REQUIRE(lp.get_coeff(row1, col1) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row1, col2) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row2, col1) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row2, col2) == doctest::Approx(0));

  lp.set_coeff(row1, col1, 1);

  REQUIRE(lp.get_coeff(row1, col1) == doctest::Approx(1));
  REQUIRE(lp.get_coeff(row1, col2) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row2, col1) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row2, col2) == doctest::Approx(0));

  lp.set_coeff(row2, col2, -1);

  REQUIRE(lp.get_coeff(row1, col1) == doctest::Approx(1));
  REQUIRE(lp.get_coeff(row1, col2) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row2, col1) == doctest::Approx(0));
  REQUIRE(lp.get_coeff(row2, col2) == doctest::Approx(-1));

  lp.col_at(col2).integer();
  REQUIRE(lp.col_at(col2).is_integer == true);
  REQUIRE(lp.col_at(col1).is_integer == false);

  lp.col_at(col1).cost = 10;
  lp.col_at(col2).cost = 20;

  {
    const auto flat_lp = lp.to_flat({.col_with_names = true,
                                     .row_with_names = true,
                                     .col_with_name_map = true,
                                     .row_with_name_map = true});

    REQUIRE(flat_lp.ncols == 2);
    REQUIRE(flat_lp.nrows == 2);

    const flat_lp_t flat_lp2 = {.ncols = 2,
                                .nrows = 2,
                                .matbeg = {0, 1, 2},
                                .matind = {0, 1},
                                .matval = {1, -1},
                                .collb = {-10, 0},
                                .colub = {100, 200},
                                .objval = {10, 20},
                                .rowlb = {0, 0},
                                .rowub = {25, 35},
                                .colint = {1},
                                .colnm = {"col1", "col2"},
                                .rownm = {"row1", "row2"},
                                .colmp = {{"col1", 0}, {"col2", 1}},
                                .rowmp = {{"row1", 0}, {"row2", 1}},
                                .name = "SEN"};

    REQUIRE(flat_lp.ncols == flat_lp2.ncols);
    REQUIRE(flat_lp.nrows == flat_lp2.nrows);
    REQUIRE(flat_lp.matbeg == flat_lp2.matbeg);
    REQUIRE(flat_lp.matind == flat_lp2.matind);
    REQUIRE(flat_lp.matval == flat_lp2.matval);
    REQUIRE(flat_lp.collb == flat_lp2.collb);
    REQUIRE(flat_lp.colub == flat_lp2.colub);
    REQUIRE(flat_lp.objval == flat_lp2.objval);
    REQUIRE(flat_lp.rowlb == flat_lp2.rowlb);
    REQUIRE(flat_lp.rowub == flat_lp2.rowub);
    REQUIRE(flat_lp.colint == flat_lp2.colint);
    REQUIRE(flat_lp.colnm == flat_lp2.colnm);
    REQUIRE(flat_lp.rownm == flat_lp2.rownm);
    REQUIRE(flat_lp.colmp == flat_lp2.colmp);
    REQUIRE(flat_lp.rowmp == flat_lp2.rowmp);
    REQUIRE(flat_lp.name == flat_lp2.name);
  }

  {
    const auto flat_lp = lp.to_flat({.col_with_names = false});

    REQUIRE(flat_lp.ncols == 2);
    REQUIRE(flat_lp.nrows == 2);

    const flat_lp_t flat_lp2 = {.ncols = 2,
                                .nrows = 2,
                                .matbeg = {0, 1, 2},
                                .matind = {0, 1},
                                .matval = {1, -1},
                                .collb = {-10, 0},
                                .colub = {100, 200},
                                .objval = {10, 20},
                                .rowlb = {0, 0},
                                .rowub = {25, 35},
                                .colint = {1},
                                .colnm = {},
                                .rownm = {},
                                .colmp = {},
                                .rowmp = {},
                                .name = "SEN"};

    REQUIRE(flat_lp.ncols == flat_lp2.ncols);
    REQUIRE(flat_lp.nrows == flat_lp2.nrows);
    REQUIRE(flat_lp.matbeg == flat_lp2.matbeg);
    REQUIRE(flat_lp.matind == flat_lp2.matind);
    REQUIRE(flat_lp.matval == flat_lp2.matval);
    REQUIRE(flat_lp.collb == flat_lp2.collb);
    REQUIRE(flat_lp.colub == flat_lp2.colub);
    REQUIRE(flat_lp.objval == flat_lp2.objval);
    REQUIRE(flat_lp.rowlb == flat_lp2.rowlb);
    REQUIRE(flat_lp.rowub == flat_lp2.rowub);
    REQUIRE(flat_lp.colint == flat_lp2.colint);
    REQUIRE(flat_lp.colnm == flat_lp2.colnm);
    REQUIRE(flat_lp.rownm == flat_lp2.rownm);
    REQUIRE(flat_lp.colmp == flat_lp2.colmp);
    REQUIRE(flat_lp.rowmp == flat_lp2.rowmp);
    REQUIRE(flat_lp.name == flat_lp2.name);
  }
}
