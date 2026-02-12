
#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>

using namespace gtopt;

TEST_CASE("Linear problem test 0")
{
  const SparseCol col({.name = "hola", .cost = 1});
  REQUIRE(col.name == "hola");
  REQUIRE(col.cost == doctest::Approx(1));
}

TEST_CASE("Linear problem test 1")
{
  gtopt::SparseRow row({.name = "r1"});
  REQUIRE(row.name == "r1");
  REQUIRE(row.size() == 0);

  row[ColIndex {3}] = 1;
  row[ColIndex {5}] = 2;

  REQUIRE(row.size() == 2);
  REQUIRE(row.get_coeff(ColIndex {0}) == doctest::Approx(0));
  REQUIRE(row.get_coeff(ColIndex {3}) == doctest::Approx(1));
  REQUIRE(row.get_coeff(ColIndex {5}) == doctest::Approx(2));
  REQUIRE(row.get_coeff(ColIndex {7}) == doctest::Approx(0));

  REQUIRE(row.size() == 2);
}

TEST_CASE("Linear problem test 2")
{
  {
    const gtopt::SparseCol col({.name = "c1"});
    REQUIRE(col.name == "c1");

    REQUIRE(col.lowb == doctest::Approx(0));
    REQUIRE(col.uppb == doctest::Approx(gtopt::CoinDblMax));
  }

  {
    const gtopt::SparseCol col({.name = "c1", .lowb = 25, .uppb = 25});
    REQUIRE(col.name == "c1");

    REQUIRE(col.lowb == doctest::Approx(25));
    REQUIRE(col.uppb == doctest::Approx(25));
    REQUIRE(col.cost == doctest::Approx(0));
  }

  {
    const SparseCol col({.name = "c1", .lowb = -25, .uppb = 25, .cost = 10});
    REQUIRE(col.name == "c1");

    REQUIRE(col.lowb == doctest::Approx(-25));
    REQUIRE(col.uppb == doctest::Approx(25));
    REQUIRE(col.cost == doctest::Approx(10));
  }
}

TEST_CASE("Linear problem matrix operations")
{
  LinearProblem lp("matrix_test");

  // Add multiple rows/columns
  std::vector<ColIndex> col_indices;
  col_indices.reserve(5);
  for (int i = 0; i < 5; ++i) {
    col_indices.push_back(
        lp.add_col(gtopt::SparseCol {.name = std::format("col{}", i)}));
  }

  std::vector<RowIndex> row_indices;
  row_indices.reserve(3);
  for (int i = 0; i < 3; ++i) {
    row_indices.push_back(
        lp.add_row(gtopt::SparseRow {.name = std::format("row{}", i)}));
  }

  // Set up a small matrix
  lp.set_coeff(row_indices[0], col_indices[0], 1.0);
  lp.set_coeff(row_indices[0], col_indices[2], 2.0);
  lp.set_coeff(row_indices[1], col_indices[1], 3.0);
  lp.set_coeff(row_indices[2], col_indices[3], 4.0);
  lp.set_coeff(row_indices[2], col_indices[4], 5.0);

  // Test flat conversion with different options
  SUBCASE("Flat conversion options")
  {
    auto flat_full = lp.to_flat({
        .col_with_names = true,
        .row_with_names = true,
        .col_with_name_map = true,
    });

    CHECK(flat_full.ncols == 5);
    CHECK(flat_full.nrows == 3);
    CHECK(flat_full.matval.size() == 5);

    auto flat_minimal =
        lp.to_flat({.col_with_names = false, .row_with_names = false});

    CHECK(flat_minimal.colnm.empty());
    CHECK(flat_minimal.rownm.empty());
  }

  // Test bounds setting
  SUBCASE("Bounds checking")
  {
    lp.col_at(col_indices[0]).equal(5.0);
    CHECK(lp.get_col_lowb(col_indices[0]) == doctest::Approx(5.0));
    CHECK(lp.get_col_uppb(col_indices[0]) == doctest::Approx(5.0));

    lp.col_at(col_indices[1]).free();
    CHECK(lp.get_col_lowb(col_indices[1])
          == doctest::Approx(-gtopt::CoinDblMax));
  }
}

TEST_CASE("Linear problem edge cases")
{
  gtopt::LinearProblem lp;

  // Test empty problem conversions
  SUBCASE("Empty problem to flat")
  {
    auto flat = lp.to_flat();
    CHECK(flat.ncols == 0);
    CHECK(flat.nrows == 0);
    CHECK(flat.matbeg.empty());
    CHECK(flat.matind.empty());
    CHECK(flat.matval.empty());
  }

  // Test reserve functionality
  SUBCASE("Reserve capacity")
  {
    const gtopt::LinearProblem lp2("reserve_test");
    CHECK(lp2.get_numrows() == 0);
    CHECK(lp2.get_numcols() == 0);
  }

  // Test coefficient edge cases
  SUBCASE("Zero and near-zero coefficients")
  {
    auto row_idx = lp.add_row(gtopt::SparseRow {.name = "zero_test"});
    auto col_idx = lp.add_col(gtopt::SparseCol {.name = "zero_col"});

    // Exactly zero
    lp.set_coeff(row_idx, col_idx, 0.0);
    CHECK(lp.get_coeff(row_idx, col_idx) == doctest::Approx(0.0));

    // Near zero (within epsilon)
    lp.set_coeff(row_idx, col_idx, 1e-10);
    CHECK(lp.get_coeff(row_idx, col_idx) == doctest::Approx(1e-10));
  }
}

TEST_CASE("Linear problem advanced operations")
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
    const auto flat_lp = lp.to_flat({
        .col_with_names = true,
        .row_with_names = true,
        .col_with_name_map = true,
        .row_with_name_map = true,
    });

    REQUIRE(flat_lp.ncols == 2);
    REQUIRE(flat_lp.nrows == 2);

    const flat_lp_t flat_lp2 = {
        .ncols = 2,
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
        .name = "SEN",
    };

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

    const flat_lp_t flat_lp2 = {
        .ncols = 2,
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
        .name = "SEN",
    };

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

TEST_CASE("Linear problem to_flat row ordering")
{
  // Verify that the two-pass algorithm produces correctly sorted row
  // indices within each column, matching the expected column-major format.
  gtopt::LinearProblem lp("ordering_test");

  const auto c0 = lp.add_col(gtopt::SparseCol {.name = "c0"});
  const auto c1 = lp.add_col(gtopt::SparseCol {.name = "c1"});

  auto r0 = lp.add_row(gtopt::SparseRow {.name = "r0"});
  auto r1 = lp.add_row(gtopt::SparseRow {.name = "r1"});
  auto r2 = lp.add_row(gtopt::SparseRow {.name = "r2"});

  // Column 0 has entries in rows 0 and 2
  // Column 1 has entries in rows 0, 1, and 2
  lp.set_coeff(r0, c0, 1.0);
  lp.set_coeff(r2, c0, 3.0);
  lp.set_coeff(r0, c1, 4.0);
  lp.set_coeff(r1, c1, 5.0);
  lp.set_coeff(r2, c1, 6.0);

  const auto flat = lp.to_flat();

  REQUIRE(flat.ncols == 2);
  REQUIRE(flat.nrows == 3);

  // matbeg: col0 starts at 0, col1 starts at 2, end at 5
  REQUIRE(flat.matbeg.size() == 3);
  CHECK(flat.matbeg[0] == 0);
  CHECK(flat.matbeg[1] == 2);
  CHECK(flat.matbeg[2] == 5);

  // Column 0: rows 0 and 2 (sorted)
  CHECK(flat.matind[0] == 0);
  CHECK(flat.matval[0] == 1.0);
  CHECK(flat.matind[1] == 2);
  CHECK(flat.matval[1] == 3.0);

  // Column 1: rows 0, 1, and 2 (sorted)
  CHECK(flat.matind[2] == 0);
  CHECK(flat.matval[2] == 4.0);
  CHECK(flat.matind[3] == 1);
  CHECK(flat.matval[3] == 5.0);
  CHECK(flat.matind[4] == 2);
  CHECK(flat.matval[4] == 6.0);
}

TEST_CASE("Linear problem to_flat column and row names")
{
  gtopt::LinearProblem lp("names_test");

  const auto c0 = lp.add_col(gtopt::SparseCol {.name = "alpha"});
  const auto c1 = lp.add_col(gtopt::SparseCol {.name = "beta"});
  const auto c2 = lp.add_col(gtopt::SparseCol {.name = "gamma"});

  auto r0 = lp.add_row(gtopt::SparseRow {.name = "con1"});
  auto r1 = lp.add_row(gtopt::SparseRow {.name = "con2"});

  lp.set_coeff(r0, c0, 1.0);
  lp.set_coeff(r0, c2, 2.0);
  lp.set_coeff(r1, c1, 3.0);

  SUBCASE("col_with_names only")
  {
    const auto flat = lp.to_flat({.col_with_names = true});

    REQUIRE(flat.colnm.size() == 3);
    CHECK(flat.colnm[0] == "alpha");
    CHECK(flat.colnm[1] == "beta");
    CHECK(flat.colnm[2] == "gamma");

    CHECK(flat.rownm.empty());
    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("row_with_names only")
  {
    const auto flat = lp.to_flat({.row_with_names = true});

    CHECK(flat.colnm.empty());

    REQUIRE(flat.rownm.size() == 2);
    CHECK(flat.rownm[0] == "con1");
    CHECK(flat.rownm[1] == "con2");

    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("both names enabled")
  {
    const auto flat = lp.to_flat({
        .col_with_names = true,
        .row_with_names = true,
    });

    REQUIRE(flat.colnm.size() == 3);
    CHECK(flat.colnm[0] == "alpha");
    CHECK(flat.colnm[1] == "beta");
    CHECK(flat.colnm[2] == "gamma");

    REQUIRE(flat.rownm.size() == 2);
    CHECK(flat.rownm[0] == "con1");
    CHECK(flat.rownm[1] == "con2");

    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("col_with_name_map implies colnm populated")
  {
    const auto flat = lp.to_flat({.col_with_name_map = true});

    REQUIRE(flat.colnm.size() == 3);
    CHECK(flat.colnm[0] == "alpha");
    CHECK(flat.colnm[1] == "beta");
    CHECK(flat.colnm[2] == "gamma");

    REQUIRE(flat.colmp.size() == 3);
    CHECK(flat.colmp.at("alpha") == 0);
    CHECK(flat.colmp.at("beta") == 1);
    CHECK(flat.colmp.at("gamma") == 2);

    CHECK(flat.rownm.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("row_with_name_map implies rownm populated")
  {
    const auto flat = lp.to_flat({.row_with_name_map = true});

    CHECK(flat.colnm.empty());
    CHECK(flat.colmp.empty());

    REQUIRE(flat.rownm.size() == 2);
    CHECK(flat.rownm[0] == "con1");
    CHECK(flat.rownm[1] == "con2");

    REQUIRE(flat.rowmp.size() == 2);
    CHECK(flat.rowmp.at("con1") == 0);
    CHECK(flat.rowmp.at("con2") == 1);
  }

  SUBCASE("all names and maps enabled")
  {
    const auto flat = lp.to_flat({
        .col_with_names = true,
        .row_with_names = true,
        .col_with_name_map = true,
        .row_with_name_map = true,
    });

    REQUIRE(flat.colnm.size() == 3);
    CHECK(flat.colnm[0] == "alpha");
    CHECK(flat.colnm[1] == "beta");
    CHECK(flat.colnm[2] == "gamma");

    REQUIRE(flat.rownm.size() == 2);
    CHECK(flat.rownm[0] == "con1");
    CHECK(flat.rownm[1] == "con2");

    REQUIRE(flat.colmp.size() == 3);
    CHECK(flat.colmp.at("alpha") == 0);
    CHECK(flat.colmp.at("beta") == 1);
    CHECK(flat.colmp.at("gamma") == 2);

    REQUIRE(flat.rowmp.size() == 2);
    CHECK(flat.rowmp.at("con1") == 0);
    CHECK(flat.rowmp.at("con2") == 1);
  }

  SUBCASE("no names requested")
  {
    const auto flat = lp.to_flat();

    CHECK(flat.colnm.empty());
    CHECK(flat.rownm.empty());
    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }
}

TEST_CASE("Linear problem to_flat with epsilon filtering")
{
  gtopt::LinearProblem lp("eps_test");

  const auto c0 = lp.add_col(gtopt::SparseCol {.name = "c0"});
  const auto c1 = lp.add_col(gtopt::SparseCol {.name = "c1"});

  auto r0 = lp.add_row(gtopt::SparseRow {.name = "r0"});
  auto r1 = lp.add_row(gtopt::SparseRow {.name = "r1"});

  lp.set_coeff(r0, c0, 1.0);
  lp.set_coeff(r0, c1, 0.001);  // Small value
  lp.set_coeff(r1, c0, -2.0);
  lp.set_coeff(r1, c1, 0.0005);  // Even smaller

  // With eps=0.01, both c1 entries should be filtered out
  const auto flat = lp.to_flat({.eps = 0.01});

  REQUIRE(flat.ncols == 2);
  CHECK(flat.matbeg[0] == 0);  // col0 starts at 0
  CHECK(flat.matbeg[1] == 2);  // col1 starts at 2
  CHECK(flat.matbeg[2] == 2);  // col1 has 0 entries (end at 2)

  CHECK(flat.matind[0] == 0);
  CHECK(flat.matval[0] == 1.0);
  CHECK(flat.matind[1] == 1);
  CHECK(flat.matval[1] == -2.0);
}
