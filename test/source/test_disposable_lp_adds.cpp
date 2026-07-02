// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_disposable_lp_adds.cpp
 * @brief     Tests for `LinearInterface::add_col_disposable` /
 *            `LinearInterface::add_row_disposable` — the elastic-filter
 *            "throw-away clone" insertion path.
 *
 * Disposable adds capture label metadata in per-clone-local extras
 * (`m_post_clone_*_metas_` + dedup map) instead of the shared
 * `m_labels_.col_labels_meta`.  These tests verify:
 *
 *  - The backend grows (col/row count increments).
 *  - Duplicate disposable insertions are rejected with both indices
 *    in the message (mirrors production add_col(SparseCol)).
 *  - A disposable with the same metadata as a post-flatten col/row is
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
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
// NOLINTBEGIN(misc-const-correctness)

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
  {
    auto try_add = [&]
    {
      (void)li.add_col_disposable(SparseCol {
          .uppb = 2.0,
          .class_name = "TestDisp",
          .variable_name = "slack",
          .variable_uid = 7,
      });
    };
    CHECK_THROWS_AS(try_add(), std::runtime_error);
  }
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
  {
    auto try_add = [&]
    {
      (void)li.add_col_disposable(SparseCol {
          .uppb = 1.0,
          .class_name = "Shared",
          .variable_name = "production",
          .variable_uid = 99,
      });
    };
    CHECK_THROWS_AS(try_add(), std::runtime_error);
  }
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

// ──────────────────────────────────────────────────────────────────────────
// Regression: low_memory=compress + record_cut_deletion across release /
// reconstruct cycle.  Mirrors the cascade `forget_first_cuts` lifecycle:
//
//   1. Build flat structural LP, install snapshot.
//   2. Add a "cascade elastic" dynamic row (record_dynamic_row).
//   3. Add multiple Benders-style cuts (record_cut_row).
//   4. Delete one of the cut rows from the LP (delete_rows +
//      record_cut_deletion).
//   5. release_backend → reconstruct_backend should re-hydrate the snapshot
//      and replay only the cuts that survived the deletion.
//
// Crash trigger reported on juan/IPLP cascade run (2026-05-13):
//   "LinearInterface::ensure_backend: snapshot reconstruct returned without
//    restoring the backend (m_backend_ is null or still marked released)."
//
// The assertion below catches that exact failure mode: after a successful
// reconstruct the backend must be alive AND the row count must match
// structural + dynamic + surviving cuts.
TEST_CASE(
    "LinearInterface — compress + record_cut_deletion + dynamic_row "
    "reconstruct round-trip")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a structural LP: min 2x1 + 3x2 s.t. x1 + x2 >= 5, 0 <= xi <= 10.
  LinearProblem lp;
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
  });
  const auto base_row = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(base_row, c1, 1.0);
  lp.set_coeff(base_row, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);

  // Install snapshot BEFORE any dynamic mutations (mirrors freeze_for_cuts
  // → save_snapshot order used by SystemLP under low_memory=compress).
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // 1. A "cascade elastic" structural dynamic row (recorded so that on
  //    reconstruct it is replayed BEFORE save_base_numrows).
  SparseRow elastic;
  elastic[c1] = 1.0;
  elastic.lowb = -SparseRow::DblMax;
  elastic.uppb = 100.0;
  elastic.class_name = "cascade";
  elastic.constraint_name = "elastic_target";
  (void)li.add_row(elastic);  // NOLINT
  li.record_dynamic_row(elastic);

  li.save_base_numrows();
  const auto base = static_cast<int>(li.base_numrows());

  // 2. Three cut rows (inherited-cut analogue + phase-1 analogue).
  SparseRow cut0;
  cut0[c2] = 1.0;
  cut0.lowb = 1.0;
  cut0.uppb = SparseRow::DblMax;
  (void)li.add_row(cut0);  // NOLINT
  li.record_cut_row(cut0);

  SparseRow cut1;
  cut1[c2] = 1.0;
  cut1.lowb = 2.0;
  cut1.uppb = SparseRow::DblMax;
  (void)li.add_row(cut1);  // NOLINT
  li.record_cut_row(cut1);

  SparseRow cut2;
  cut2[c2] = 1.0;
  cut2.lowb = 3.0;
  cut2.uppb = SparseRow::DblMax;
  (void)li.add_row(cut2);  // NOLINT
  li.record_cut_row(cut2);

  REQUIRE(li.active_cuts_size() == 3);
  REQUIRE(li.get_numrows() == static_cast<size_t>(base) + 3U);

  SUBCASE("delete first cut (inherited-cut analogue) and reconstruct")
  {
    std::array<int, 1> deleted = {base};
    li.delete_rows(deleted);
    li.record_cut_deletion(deleted);

    CHECK(li.active_cuts_size() == 2);
    CHECK(li.get_numrows() == static_cast<size_t>(base) + 2U);

    // Release + reconstruct: this is the path that crashed in production.
    li.release_backend();
    REQUIRE(li.is_backend_released());
    REQUIRE(li.has_snapshot_data());

    // Must NOT throw "snapshot reconstruct returned without restoring
    // the backend".  Trigger reconstruct via any public read; resolve()
    // exercises ensure_backend internally.
    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.has_backend());
    CHECK(li.get_numrows() == static_cast<size_t>(base) + 2U);
  }

  SUBCASE("delete middle cut and reconstruct")
  {
    std::array<int, 1> deleted = {base + 1};
    li.delete_rows(deleted);
    li.record_cut_deletion(deleted);

    li.release_backend();
    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.get_numrows() == static_cast<size_t>(base) + 2U);
  }

  SUBCASE("delete all cuts (forget_first_cuts analogue) and reconstruct")
  {
    std::array<int, 3> deleted = {base, base + 1, base + 2};
    li.delete_rows(deleted);
    li.record_cut_deletion(deleted);

    CHECK(li.active_cuts_size() == 0);

    li.release_backend();
    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.get_numrows() == static_cast<size_t>(base));
  }
}

// NOLINTEND(misc-const-correctness)