
#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>

TEST_CASE("Linear problem test 0")
{
  using namespace gtopt;
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
    REQUIRE(col.uppb == doctest::Approx(LinearProblem::DblMax));
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
    auto flat_full = lp.lp_build({
        .col_with_names = true,
        .row_with_names = true,
        .col_with_name_map = true,
    });

    CHECK(flat_full.ncols == 5);
    CHECK(flat_full.nrows == 3);
    CHECK(flat_full.matval.size() == 5);

    auto flat_minimal = lp.lp_build({
        .col_with_names = false,
        .row_with_names = false,
        .col_with_name_map = false,
        .row_with_name_map = false,
    });

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
          == doctest::Approx(-LinearProblem::DblMax));
  }
}

TEST_CASE("Linear problem edge cases")
{
  gtopt::LinearProblem lp;

  // Test empty problem conversions
  SUBCASE("Empty problem to flat")
  {
    auto flat = lp.lp_build();
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
    auto&& flat_lp = lp.lp_build();

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
  REQUIRE(lp.col_at(col1).uppb == doctest::Approx(LinearProblem::DblMax));

  REQUIRE(lp.col_at(col2).name == "col2");
  REQUIRE(lp.col_at(col2).lowb == doctest::Approx(-LinearProblem::DblMax));
  REQUIRE(lp.col_at(col2).uppb == doctest::Approx(LinearProblem::DblMax));

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
    const auto flat_lp = lp.lp_build({
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
        .col_scales = {1.0, 1.0},
        .row_scales = {},
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
    REQUIRE(flat_lp.col_scales == flat_lp2.col_scales);
    REQUIRE(flat_lp.colnm == flat_lp2.colnm);
    REQUIRE(flat_lp.rownm == flat_lp2.rownm);
    REQUIRE(flat_lp.colmp == flat_lp2.colmp);
    REQUIRE(flat_lp.rowmp == flat_lp2.rowmp);
    REQUIRE(flat_lp.name == flat_lp2.name);
  }

  {
    const auto flat_lp =
        lp.lp_build({.col_with_names = false, .col_with_name_map = false});

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
        .col_scales = {1.0, 1.0},
        .row_scales = {},
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
    REQUIRE(flat_lp.col_scales == flat_lp2.col_scales);
    REQUIRE(flat_lp.colnm == flat_lp2.colnm);
    REQUIRE(flat_lp.rownm == flat_lp2.rownm);
    REQUIRE(flat_lp.colmp == flat_lp2.colmp);
    REQUIRE(flat_lp.rowmp == flat_lp2.rowmp);
    REQUIRE(flat_lp.name == flat_lp2.name);
  }
}

TEST_CASE("Linear problem lp_build row ordering")
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

  const auto flat = lp.lp_build({
      .row_equilibration = false,
  });

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

TEST_CASE("Linear problem lp_build column and row names")
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

  SUBCASE("col_with_names only (no maps)")
  {
    const auto flat =
        lp.lp_build({.col_with_names = true, .col_with_name_map = false});

    REQUIRE(flat.colnm.size() == 3);
    CHECK(flat.colnm[0] == "alpha");
    CHECK(flat.colnm[1] == "beta");
    CHECK(flat.colnm[2] == "gamma");

    CHECK(flat.rownm.empty());
    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("row_with_names only (no maps)")
  {
    const auto flat = lp.lp_build({
        .col_with_names = false,
        .row_with_names = true,
        .col_with_name_map = false,
    });

    CHECK(flat.colnm.empty());

    REQUIRE(flat.rownm.size() == 2);
    CHECK(flat.rownm[0] == "con1");
    CHECK(flat.rownm[1] == "con2");

    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("both names enabled (no maps)")
  {
    const auto flat = lp.lp_build({
        .col_with_names = true,
        .row_with_names = true,
        .col_with_name_map = false,
        .row_with_name_map = false,
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
    const auto flat = lp.lp_build({.col_with_name_map = true});

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
    const auto flat = lp.lp_build({
        .col_with_names = false,
        .col_with_name_map = false,
        .row_with_name_map = true,
    });

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
    const auto flat = lp.lp_build({
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

  SUBCASE("default LpBuildOptions (level 0: col names only, no name map)")
  {
    const auto flat = lp.lp_build();

    // Default LpBuildOptions has col_with_names=true, col_with_name_map=false
    REQUIRE(flat.colnm.size() == 3);
    CHECK(flat.colnm[0] == "alpha");
    CHECK(flat.colnm[1] == "beta");
    CHECK(flat.colnm[2] == "gamma");

    CHECK(flat.rownm.empty());
    CHECK(flat.colmp.empty());
    CHECK(flat.rowmp.empty());
  }

  SUBCASE("col_with_name_map builds name-to-index map")
  {
    const auto flat = lp.lp_build({
        .col_with_name_map = true,
    });

    REQUIRE(flat.colmp.size() == 3);
    CHECK(flat.colmp.at("alpha") == 0);
    CHECK(flat.colmp.at("beta") == 1);
    CHECK(flat.colmp.at("gamma") == 2);
  }
}

TEST_CASE("Linear problem lp_build with epsilon filtering")
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
  const auto flat = lp.lp_build({
      .eps = 0.01,
      .row_equilibration = false,
  });

  REQUIRE(flat.ncols == 2);
  CHECK(flat.matbeg[0] == 0);  // col0 starts at 0
  CHECK(flat.matbeg[1] == 2);  // col1 starts at 2
  CHECK(flat.matbeg[2] == 2);  // col1 has 0 entries (end at 2)

  CHECK(flat.matind[0] == 0);
  CHECK(flat.matval[0] == 1.0);
  CHECK(flat.matind[1] == 1);
  CHECK(flat.matval[1] == -2.0);
}

TEST_CASE(
    "Linear problem lp_build compute_stats with col names and zeroed count")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearProblem lp("stats_test");

  const auto c0 = lp.add_col(SparseCol {.name = "big_col"});
  const auto c1 = lp.add_col(SparseCol {.name = "small_col"});
  const auto c2 = lp.add_col(SparseCol {.name = "tiny_col"});

  auto r0 = lp.add_row(SparseRow {.name = "r0"});
  auto r1 = lp.add_row(SparseRow {.name = "r1"});
  auto r2 = lp.add_row(SparseRow {.name = "r2"});

  lp.set_coeff(r0, c0, 100.0);  // largest coefficient
  lp.set_coeff(r1, c1, 0.5);  // medium
  lp.set_coeff(r2, c2, 1e-12);  // tiny — below stats_eps default (1e-10)

  SUBCASE("compute_stats=true with default stats_eps=1e-10")
  {
    const auto flat = lp.lp_build({
        .col_with_names = true,
        .compute_stats = true,
        .row_equilibration = false,
    });

    // All three pass the default eps=0 filter → nnz=3
    CHECK(flat.stats_nnz == 3);
    // The 1e-12 value is below stats_eps=1e-10, so it is NOT counted as zeroed
    // (zeroed = filtered by eps, not by stats_eps)
    CHECK(flat.stats_zeroed == 0);

    // max = 100.0 from c0 (col index 0, name "big_col")
    CHECK(flat.stats_max_abs == doctest::Approx(100.0));
    CHECK(flat.stats_max_col == 0);
    CHECK(flat.stats_max_col_name == "big_col");

    // min from stats tracking: 1e-12 < stats_eps=1e-10, so min = 0.5 (c1)
    CHECK(flat.stats_min_abs == doctest::Approx(0.5));
    CHECK(flat.stats_min_col == 1);
    CHECK(flat.stats_min_col_name == "small_col");
  }

  SUBCASE("compute_stats=true with eps=1.0 (filters 0.5 and 1e-12)")
  {
    const auto flat = lp.lp_build({
        .eps = 1.0,
        .col_with_names = true,
        .compute_stats = true,
        .row_equilibration = false,
    });

    // Only 100.0 passes eps=1.0; 0.5 and 1e-12 are zeroed
    CHECK(flat.stats_nnz == 1);
    CHECK(flat.stats_zeroed == 2);

    CHECK(flat.stats_max_abs == doctest::Approx(100.0));
    CHECK(flat.stats_max_col == 0);
    CHECK(flat.stats_max_col_name == "big_col");

    // Only one value in the matrix — min equals max
    CHECK(flat.stats_min_abs == doctest::Approx(100.0));
    CHECK(flat.stats_min_col == 0);
    CHECK(flat.stats_min_col_name == "big_col");
  }

  SUBCASE("compute_stats=true without col names — col index still tracked")
  {
    // col_with_names=false AND col_with_name_map=false: indices available,
    // names empty
    const auto flat = lp.lp_build({
        .col_with_names = false,
        .col_with_name_map = false,
        .compute_stats = true,
        .row_equilibration = false,
    });

    CHECK(flat.stats_max_col == 0);
    CHECK(flat.stats_max_col_name.empty());
    CHECK(flat.stats_min_col == 1);
    CHECK(flat.stats_min_col_name.empty());
  }

  SUBCASE("compute_stats=false — stats fields stay default")
  {
    const auto flat = lp.lp_build({
        .col_with_names = true,
        .compute_stats = false,
    });

    CHECK(flat.stats_nnz == 0);
    CHECK(flat.stats_zeroed == 0);
    CHECK(flat.stats_max_col == -1);
    CHECK(flat.stats_min_col == -1);
    CHECK(flat.stats_max_col_name.empty());
    CHECK(flat.stats_min_col_name.empty());
  }
}

TEST_CASE("Linear problem set_coeff overwrite preserves correctness")
{
  gtopt::LinearProblem lp("overwrite_test");

  const auto c0 = lp.add_col(gtopt::SparseCol {.name = "c0"});
  auto r0 = lp.add_row(gtopt::SparseRow {.name = "r0"});

  // Set initial coefficient
  lp.set_coeff(r0, c0, 1.0);
  CHECK(lp.get_coeff(r0, c0) == doctest::Approx(1.0));

  // Overwrite the same coefficient
  lp.set_coeff(r0, c0, 5.0);
  CHECK(lp.get_coeff(r0, c0) == doctest::Approx(5.0));

  // Verify flat conversion has exactly one entry, not two
  const auto flat = lp.lp_build({
      .row_equilibration = false,
  });
  CHECK(flat.ncols == 1);
  CHECK(flat.nrows == 1);
  CHECK(flat.matval.size() == 1);
  CHECK(flat.matval[0] == doctest::Approx(5.0));
}

TEST_CASE("lp_build name map skips empty column names")
{
  gtopt::LinearProblem lp("empty_names_test");

  // Add multiple columns with empty names to verify the name map skips them
  // (simulates minimal names_level where non-state columns get empty names)
  [[maybe_unused]] const auto c0 = lp.add_col(gtopt::SparseCol {.name = ""});
  [[maybe_unused]] const auto c1 =
      lp.add_col(gtopt::SparseCol {.name = "named_col"});
  [[maybe_unused]] const auto c2 = lp.add_col(gtopt::SparseCol {.name = ""});
  [[maybe_unused]] const auto c3 = lp.add_col(gtopt::SparseCol {.name = ""});

  // lp_build requires at least one row to produce output
  auto rrow = gtopt::SparseRow {.name = "r0"};
  rrow[c0] = 1;
  [[maybe_unused]] const auto r0 = lp.add_row(std::move(rrow.less_equal(1)));

  REQUIRE(lp.get_numcols() == 4);
  REQUIRE(lp.get_numrows() == 1);

  auto flat = lp.lp_build({
      .col_with_names = true,
      .col_with_name_map = true,
  });

  // All 4 column names should be present in colnm
  REQUIRE(flat.colnm.size() == 4);
  CHECK(flat.colnm[0].empty());
  CHECK(flat.colnm[1] == "named_col");
  CHECK(flat.colnm[2].empty());
  CHECK(flat.colnm[3].empty());

  // The name map should only contain the non-empty name
  CHECK(flat.colmp.size() == 1);
  CHECK(flat.colmp.count("named_col") == 1);
  CHECK(flat.colmp.at("named_col") == 1);
}

// ---------------------------------------------------------------
// Row equilibration scaling tests
// ---------------------------------------------------------------

TEST_CASE("lp_build row_equilibration normalizes row max to 1")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearProblem lp("row_eq_test");

  // Create 2 variables, 2 rows with different coefficient magnitudes.
  // Row 0: [100, 1]   → max=100, after scaling: [1, 0.01]
  // Row 1: [0.5, 2]   → max=2,   after scaling: [0.25, 1]
  const auto c0 = lp.add_col(SparseCol {
      .name = "c0",
  });
  const auto c1 = lp.add_col(SparseCol {
      .name = "c1",
  });

  auto r0 = SparseRow {
      .name = "r0",
  };
  r0[c0] = 100.0;
  r0[c1] = 1.0;
  r0.bound(0.0, 50.0);  // RHS: [0, 50] → scaled [0, 0.5]
  std::ignore = lp.add_row(std::move(r0));

  auto r1 = SparseRow {
      .name = "r1",
  };
  r1[c0] = 0.5;
  r1[c1] = 2.0;
  r1.bound(-4.0, 10.0);  // RHS: [-4, 10] → scaled [-2, 5]
  std::ignore = lp.add_row(std::move(r1));

  SUBCASE("without row_equilibration — coefficients unchanged")
  {
    const auto flat = lp.lp_build({
        .row_equilibration = false,
    });

    CHECK(flat.row_scales.empty());

    // Find coefficient for (row0, col0) = 100
    // Column 0: matbeg[0] to matbeg[1]
    bool found_r0_c0 = false;
    for (auto k = flat.matbeg[0]; k < flat.matbeg[1]; ++k) {
      if (flat.matind[k] == 0) {
        CHECK(flat.matval[k] == doctest::Approx(100.0));
        found_r0_c0 = true;
      }
    }
    CHECK(found_r0_c0);
    CHECK(flat.rowub[0] == doctest::Approx(50.0));
  }

  SUBCASE("with row_equilibration — row max normalized to 1")
  {
    const auto flat = lp.lp_build({
        .row_equilibration = true,
    });

    REQUIRE(flat.row_scales.size() == 2);
    CHECK(flat.row_scales[0] == doctest::Approx(100.0));
    CHECK(flat.row_scales[1] == doctest::Approx(2.0));

    // Row 0 RHS scaled by 1/100
    CHECK(flat.rowlb[0] == doctest::Approx(0.0));
    CHECK(flat.rowub[0] == doctest::Approx(0.5));

    // Row 1 RHS scaled by 1/2
    CHECK(flat.rowlb[1] == doctest::Approx(-2.0));
    CHECK(flat.rowub[1] == doctest::Approx(5.0));

    // Matrix coefficients: scan column 0 for both rows.
    for (auto k = flat.matbeg[0]; k < flat.matbeg[1]; ++k) {
      if (flat.matind[k] == 0) {
        // row0, col0: 100/100 = 1.0
        CHECK(flat.matval[k] == doctest::Approx(1.0));
      } else if (flat.matind[k] == 1) {
        // row1, col0: 0.5/2 = 0.25
        CHECK(flat.matval[k] == doctest::Approx(0.25));
      }
    }

    // Column 1:
    for (auto k = flat.matbeg[1]; k < flat.matbeg[2]; ++k) {
      if (flat.matind[k] == 0) {
        // row0, col1: 1.0/100 = 0.01
        CHECK(flat.matval[k] == doctest::Approx(0.01));
      } else if (flat.matind[k] == 1) {
        // row1, col1: 2.0/2 = 1.0
        CHECK(flat.matval[k] == doctest::Approx(1.0));
      }
    }
  }
}

TEST_CASE("LinearInterface row_equilibration unscales duals")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a small LP: min x s.t. x >= 5, solve, verify dual unscaling.
  // Row: [1000] * x >= 5000  (large coefficient for scaling test)
  LinearProblem lp("dual_unscale_test");

  const auto c0 = lp.add_col(SparseCol {
      .name = "x",
      .lowb = 0,
      .uppb = 100,
      .cost = 1.0,
  });

  auto r0 = SparseRow {
      .name = "r0",
  };
  r0[c0] = 1000.0;
  r0.greater_equal(5000.0);
  std::ignore = lp.add_row(std::move(r0));

  SUBCASE("without row_equilibration")
  {
    const auto flat = lp.lp_build({
        .row_equilibration = false,
    });
    LinearInterface li("", flat);
    std::ignore = li.initial_solve({});
    REQUIRE(li.is_optimal());

    const auto duals = li.get_row_dual();
    REQUIRE(duals.size() == 1);
    // Dual for 1000x >= 5000, optimal x=5, dual=cost/coeff=1/1000=0.001
    CHECK(duals[0] == doctest::Approx(0.001).epsilon(1e-4));
  }

  SUBCASE("with row_equilibration — duals match physical values")
  {
    const auto flat = lp.lp_build({
        .row_equilibration = true,
    });
    REQUIRE(flat.row_scales.size() == 1);
    CHECK(flat.row_scales[0] == doctest::Approx(1000.0));

    LinearInterface li("", flat);
    std::ignore = li.initial_solve({});
    REQUIRE(li.is_optimal());

    // After unscaling: dual_phys = dual_LP / row_scale = same physical dual
    const auto duals = li.get_row_dual();
    REQUIRE(duals.size() == 1);
    CHECK(duals[0] == doctest::Approx(0.001).epsilon(1e-4));
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface row_equilibration handles dynamically added rows")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build LP with one row, enable row_equilibration, then add a new row
  // after construction (simulating SDDP cut addition).  get_row_dual()
  // must not crash on the newly added row that has no scale entry.
  LinearProblem lp("dynamic_row_test");

  const auto c0 = lp.add_col(SparseCol {
      .name = "x",
      .lowb = 0,
      .uppb = 100,
      .cost = 1.0,
  });

  auto r0 = SparseRow {
      .name = "r0",
  };
  r0[c0] = 500.0;
  r0.greater_equal(2500.0);
  std::ignore = lp.add_row(std::move(r0));

  const auto flat = lp.lp_build({
      .row_equilibration = true,
  });
  REQUIRE(flat.row_scales.size() == 1);
  CHECK(flat.row_scales[0] == doctest::Approx(500.0));

  LinearInterface li("", flat);
  std::ignore = li.initial_solve({});
  REQUIRE(li.is_optimal());

  // Add a new row dynamically (like an SDDP cut).
  auto new_row = SparseRow {
      .name = "cut_0",
  };
  new_row[c0] = 1.0;
  new_row.greater_equal(3.0);
  std::ignore = li.add_row(new_row);

  // Re-solve with the new constraint.
  std::ignore = li.resolve({});
  REQUIRE(li.is_optimal());

  // get_row_dual() must not crash — the new row has no row_scale entry
  // and should be treated as scale=1.0.
  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 2);

  // Original row dual: physical dual for 500x >= 2500, optimal x=5,
  // dual = 1/500 = 0.002
  CHECK(duals[0] == doctest::Approx(0.002).epsilon(1e-4));
  // New row: x >= 3 is non-binding (x=5 > 3), so dual should be 0.
  CHECK(duals[1] == doctest::Approx(0.0).epsilon(1e-4));
}

TEST_CASE("lp_build per-row-type coefficient stats")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a small LP with named rows of different constraint types.
  // Row name format: cname_type_uid_... → type is 2nd underscore token.
  LinearProblem lp("row_type_stats_test");

  const auto c0 = lp.add_col(SparseCol {.name = "x0", .cost = 1.0});
  const auto c1 = lp.add_col(SparseCol {.name = "x1", .cost = 2.0});

  // Two "bal" rows with different coefficient magnitudes
  auto r_bal0 = SparseRow {.name = "bus_bal_1_0_0_0"};
  r_bal0[c0] = 1.0;
  r_bal0[c1] = 100.0;
  r_bal0.equal(50.0);
  std::ignore = lp.add_row(std::move(r_bal0));

  auto r_bal1 = SparseRow {.name = "bus_bal_2_0_0_0"};
  r_bal1[c0] = 0.5;
  r_bal1[c1] = 200.0;
  r_bal1.equal(100.0);
  std::ignore = lp.add_row(std::move(r_bal1));

  // One "theta" row
  auto r_theta = SparseRow {.name = "line_theta_1_0_0_0"};
  r_theta[c0] = 0.001;
  r_theta[c1] = 1000.0;
  r_theta.equal(0.0);
  std::ignore = lp.add_row(std::move(r_theta));

  // Build with stats and row names enabled, without equilibration
  // so coefficient values are preserved for exact comparison.
  const auto flat = lp.lp_build({
      .row_with_names = true,
      .compute_stats = true,
      .row_equilibration = false,
  });

  REQUIRE(!flat.row_type_stats.empty());

  // Find "bal" and "theta" entries
  const FlatLinearProblem::RowTypeStatsEntry* bal_entry = nullptr;
  const FlatLinearProblem::RowTypeStatsEntry* theta_entry = nullptr;
  for (const auto& entry : flat.row_type_stats) {
    if (entry.type == "bal") {
      bal_entry = &entry;
    } else if (entry.type == "theta") {
      theta_entry = &entry;
    }
  }

  REQUIRE(bal_entry != nullptr);
  CHECK(bal_entry->count == 2);
  CHECK(bal_entry->nnz == 4);
  CHECK(bal_entry->max_abs == doctest::Approx(200.0));
  CHECK(bal_entry->min_abs == doctest::Approx(0.5));

  REQUIRE(theta_entry != nullptr);
  CHECK(theta_entry->count == 1);
  CHECK(theta_entry->nnz == 2);
  CHECK(theta_entry->max_abs == doctest::Approx(1000.0));
  CHECK(theta_entry->min_abs == doctest::Approx(0.001));
}

TEST_CASE("lp_build per-row-type stats empty without row names")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearProblem lp("no_row_names_test");
  const auto c0 = lp.add_col(SparseCol {.name = "x0", .cost = 1.0});
  auto r0 = SparseRow {.name = "bus_bal_1_0_0_0"};
  r0[c0] = 1.0;
  r0.equal(0.0);
  std::ignore = lp.add_row(std::move(r0));

  // stats enabled but row names NOT enabled → row_type_stats should be empty
  const auto flat = lp.lp_build({
      .row_with_names = false,
      .compute_stats = true,
  });
  CHECK(flat.row_type_stats.empty());
}

TEST_CASE("lp_build per-row-type stats empty without compute_stats")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearProblem lp("no_stats_test");
  const auto c0 = lp.add_col(SparseCol {.name = "x0", .cost = 1.0});
  auto r0 = SparseRow {.name = "bus_bal_1_0_0_0"};
  r0[c0] = 1.0;
  r0.equal(0.0);
  std::ignore = lp.add_row(std::move(r0));

  // row names enabled but stats NOT enabled → row_type_stats should be empty
  const auto flat = lp.lp_build({
      .row_with_names = true,
      .compute_stats = false,
  });
  CHECK(flat.row_type_stats.empty());
}
