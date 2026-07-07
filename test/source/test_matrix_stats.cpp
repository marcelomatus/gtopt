// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/matrix_stats.hpp>

using namespace gtopt;

// MatrixStats was extracted from LinearInterface (step 1 of decomposing that
// class).  The point of the extraction is that these conditioning statistics
// are now a behaviour-free value type that can be constructed and asserted on
// without standing up a solver backend — this test exercises exactly that.
TEST_CASE("MatrixStats value aggregate")  // NOLINT
{
  SUBCASE("default construction is zero/empty")
  {
    const MatrixStats s {};
    CHECK(s.nnz == 0);
    CHECK(s.zeroed == 0);
    CHECK(s.max_abs == doctest::Approx(0.0));
    CHECK(s.min_abs == doctest::Approx(0.0));
    CHECK_FALSE(s.max_col.has_value());
    CHECK_FALSE(s.min_col.has_value());
    CHECK(s.max_col_name.empty());
    CHECK(s.min_col_name.empty());
    CHECK(s.row_type_stats.empty());
  }

  SUBCASE("designated aggregate initialization")
  {
    const MatrixStats s {
        .nnz = 42,
        .zeroed = 3,
        .max_abs = 1.0e7,
        .min_abs = 1.0e-3,
        .max_col = ColIndex {5},
        .min_col = ColIndex {9},
        .max_col_name = "gen_cost",
        .min_col_name = "flow",
    };
    CHECK(s.nnz == 42);
    CHECK(s.zeroed == 3);
    CHECK(s.max_abs == doctest::Approx(1.0e7));
    CHECK(s.min_abs == doctest::Approx(1.0e-3));
    CHECK((s.max_col && *s.max_col == ColIndex {5}));
    CHECK((s.min_col && *s.min_col == ColIndex {9}));
    CHECK(s.max_col_name == "gen_cost");
    CHECK(s.min_col_name == "flow");
  }

  SUBCASE("copyable — a plain value type with no hidden ownership")
  {
    MatrixStats a {.nnz = 7, .max_abs = 2.5};
    const MatrixStats b = a;  // copy
    a.nnz = 99;  // mutate original
    CHECK(b.nnz == 7);  // copy is independent
    CHECK(b.max_abs == doctest::Approx(2.5));
  }
}
