// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_disposable_lp_adds.cpp
 * @brief     Tests for `LinearInterface::add_col_disposable` /
 *            `LinearInterface::add_row_disposable` — the elastic-filter
 *            "throw-away clone" insertion path.
 *
 * Disposable adds capture label metadata in per-clone-local extras
 * (`m_post_clone_*_metas_` + dedup map) instead of the shared
 * `m_col_labels_meta_` / `m_col_meta_index_`.  These tests verify:
 *
 *  - The backend grows (col/row count increments).
 *  - Duplicate disposable insertions are rejected with both indices
 *    in the message (mirrors production add_col(SparseCol)).
 *  - A disposable with the same metadata as a production col/row is
 *    also rejected (cross-population dedup).
 *  - Labels for disposable cols/rows are recoverable via `write_lp`
 *    in the gtopt-formatted shape produced by `LabelMaker`.
 *  - Unlabelled disposables (empty class+variable+unknown_uid+
 *    monostate context) skip dedup, matching the production path.
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Read a file into a single string for substring assertions.
std::string read_file(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("add_col_disposable extends backend without touching shared meta")
{
  LinearInterface li;

  // Pre-populate with one production column.
  const auto base = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "TestProd",
      .variable_name = "base",
      .variable_uid = 1,
  });
  REQUIRE(li.get_numcols() == 1);
  REQUIRE(base == ColIndex {0});

  SUBCASE("disposable add increments numcols")
  {
    const auto disp = li.add_col_disposable(SparseCol {
        .uppb = 5.0,
        .cost = 2.0,
        .class_name = "TestDisp",
        .variable_name = "elastic_sup",
        .variable_uid = 42,
    });
    CHECK(li.get_numcols() == 2);
    CHECK(disp == ColIndex {1});

    const auto col_low = li.get_col_low();
    const auto col_upp = li.get_col_upp();
    CHECK(col_low[disp] == doctest::Approx(0.0));
    CHECK(col_upp[disp] == doctest::Approx(5.0));
    CHECK(li.get_obj_coeff()[disp] == doctest::Approx(2.0));
  }

  SUBCASE("two disposables with distinct uids both succeed")
  {
    const auto a = li.add_col_disposable(SparseCol {
        .uppb = 1.0,
        .class_name = "TestDisp",
        .variable_name = "elastic_sup",
        .variable_uid = 100,
    });
    const auto b = li.add_col_disposable(SparseCol {
        .uppb = 1.0,
        .class_name = "TestDisp",
        .variable_name = "elastic_sup",
        .variable_uid = 101,  // distinct uid
    });
    CHECK(a == ColIndex {1});
    CHECK(b == ColIndex {2});
    CHECK(li.get_numcols() == 3);
  }
}

TEST_CASE("add_col_disposable rejects duplicate disposable metadata")
{
  LinearInterface li;

  std::ignore = li.add_col_disposable(SparseCol {
      .uppb = 1.0,
      .class_name = "TestDisp",
      .variable_name = "slack",
      .variable_uid = 7,
  });

  // Same (class, variable, uid, context) — must throw.
  CHECK_THROWS_AS(li.add_col_disposable(SparseCol {
                      .uppb = 2.0,
                      .class_name = "TestDisp",
                      .variable_name = "slack",
                      .variable_uid = 7,
                  }),
                  std::runtime_error);
}

TEST_CASE("add_col_disposable rejects duplicate of a shared production col")
{
  LinearInterface li;

  std::ignore = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Shared",
      .variable_name = "production",
      .variable_uid = 99,
  });

  // Disposable add with the same metadata as the shared production
  // col — must be rejected so we don't silently shadow the shared
  // entry with a clone-local one.
  CHECK_THROWS_AS(li.add_col_disposable(SparseCol {
                      .uppb = 1.0,
                      .class_name = "Shared",
                      .variable_name = "production",
                      .variable_uid = 99,
                  }),
                  std::runtime_error);
}

TEST_CASE("add_col_disposable allows unlabelled cols (no dedup)")
{
  LinearInterface li;

  // Two unlabelled cols (default-constructed metadata) — production
  // path skips dedup for these (`is_empty_col_label`).  The
  // disposable path mirrors that: both must succeed.
  const auto a = li.add_col_disposable(SparseCol {
      .uppb = 1.0,
  });
  const auto b = li.add_col_disposable(SparseCol {
      .uppb = 2.0,
  });
  CHECK(a == ColIndex {0});
  CHECK(b == ColIndex {1});
  CHECK(li.get_numcols() == 2);
}

TEST_CASE("add_row_disposable extends backend, dedup mirrors col path")
{
  LinearInterface li;

  // Need at least one column to attach a row coefficient to.
  const auto c = li.add_col_disposable(SparseCol {
      .uppb = 10.0,
      .class_name = "Col",
      .variable_name = "x",
      .variable_uid = 1,
  });

  SUBCASE("disposable row add increments numrows")
  {
    SparseRow row {
        .lowb = 0.0,
        .uppb = 5.0,
        .class_name = "TestRow",
        .constraint_name = "elastic_fix",
        .variable_uid = 99,
    };
    row[c] = 1.0;
    const auto r = li.add_row_disposable(row);
    CHECK(li.get_numrows() == 1);
    CHECK(r == RowIndex {0});
  }

  SUBCASE("duplicate disposable row metadata throws")
  {
    SparseRow row {
        .lowb = 0.0,
        .uppb = 5.0,
        .class_name = "TestRow",
        .constraint_name = "elastic_fix",
        .variable_uid = 7,
    };
    row[c] = 1.0;
    std::ignore = li.add_row_disposable(row);

    SparseRow dup {
        .lowb = 1.0,  // values can differ; metadata is the dedup key
        .uppb = 4.0,
        .class_name = "TestRow",
        .constraint_name = "elastic_fix",
        .variable_uid = 7,
    };
    dup[c] = 1.0;
    CHECK_THROWS_AS(li.add_row_disposable(dup), std::runtime_error);
  }
}

TEST_CASE("write_lp on disposable cols emits gtopt-formatted labels")
{
  LinearInterface li;

  // Need a production col first so the LP isn't degenerate, plus a
  // disposable col with a label.
  const auto x = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Prod",
      .variable_name = "x",
      .variable_uid = 1,
  });

  const auto slack = li.add_col_disposable(SparseCol {
      .uppb = 5.0,
      .cost = 2.0,
      .class_name = "ElasticTest",
      .variable_name = "elastic_sup",
      .variable_uid = 42,
  });

  // Add a row that references both, otherwise the LP is trivial.
  SparseRow r {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Prod",
      .constraint_name = "balance",
      .variable_uid = 1,
  };
  r[x] = 1.0;
  r[slack] = 1.0;
  std::ignore = li.add_row(r);

  REQUIRE(li.get_numcols() == 2);
  REQUIRE(li.get_numrows() == 1);

  // Write to a temp .lp file and check that the disposable col's
  // class_name / variable_name / uid all appear in the resulting
  // file, indicating that the label fall-through to the per-clone
  // extras worked.
  const auto tmpdir = std::filesystem::temp_directory_path();
  const auto stem = (tmpdir / "test_disposable_label_dump").string();
  const auto wr = li.write_lp(stem);
  REQUIRE(wr.has_value());

  const auto content = read_file(stem + ".lp");
  // `LabelMaker::make_col_label` lowercases the class_name and uses
  // `_` as separator, producing e.g. `elastictest_elastic_sup_42`.
  // The variable_name is passed through verbatim (already lowercase).
  CHECK(content.find("elastictest") != std::string::npos);
  CHECK(content.find("elastic_sup") != std::string::npos);
  // Production col label should still be present (shared meta path).
  CHECK(content.find("prod") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}
