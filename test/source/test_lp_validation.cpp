/**
 * @file      test_lp_validation.cpp
 * @brief     Regression tests for the build-time LP validation hooks
 *            and the post-equilibration LP_QUALITY verdict.
 * @date      2026-05-01
 * @copyright BSD-3-Clause
 */

#include <limits>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/lp_validation.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

#include "log_capture.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
// NOLINTBEGIN(misc-const-correctness)

namespace
{
LpValidationOptions make_validation_opts(double coeff_max = 1e10,
                                         double coeff_min = 1e-10,
                                         double bound_max = 1e12,
                                         double rhs_max = 1e12,
                                         double obj_max = 1e10,
                                         int max_warns = 50)
{
  return LpValidationOptions {
      .enable = true,
      .coeff_warn_max = coeff_max,
      .coeff_warn_min = coeff_min,
      .bound_warn_max = bound_max,
      .rhs_warn_max = rhs_max,
      .obj_warn_max = obj_max,
      .max_warnings_per_kind = max_warns,
  };
}
}  // namespace

TEST_CASE("LP validation: huge coefficient triggers WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  const auto x = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });

  // 1e15 · x >= 1
  SparseRow row;
  row[x] = 1e15;
  row.greater_equal(1.0);
  (void)li.add_row(row);  // NOLINT

  CHECK(logs.contains("LP_VALIDATION huge coefficient"));
  CHECK((logs.contains("1.000e+15") || logs.contains("1e15")
         || logs.contains("1e+15")));
  CHECK(li.lp_validation_stats().coeff_huge_count == 1);
}

TEST_CASE("LP validation: tiny coefficient triggers WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  const auto x = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });
  const auto y = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });

  // 1e-12 · x + 1.0 · y >= 1
  SparseRow row;
  row[x] = 1e-12;
  row[y] = 1.0;
  row.greater_equal(1.0);
  (void)li.add_row(row);  // NOLINT

  CHECK(logs.contains("LP_VALIDATION tiny coefficient"));
  CHECK(li.lp_validation_stats().coeff_tiny_count == 1);
}

TEST_CASE("LP validation: filtered coefficient triggers filter-WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  const auto x = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });
  const auto y = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x] = 1e-9;  // gets filtered by eps below
  row[y] = 1.0;
  row.lowb = 0.0;
  row.uppb = 10.0;

  // eps = 1e-8 ⇒ 1e-9 is below the threshold and is dropped, but its
  // magnitude is well above the 1e-30 noise floor → filter WARN must
  // fire.
  (void)li.add_row(row, 1e-8);  // NOLINT

  CHECK(logs.contains("LP_VALIDATION coefficient"));
  CHECK(logs.contains("filtered to 0"));
  CHECK(li.lp_validation_stats().coeff_filtered_count >= 1);
}

TEST_CASE("LP validation: solver-infinity bound is NOT noted")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  // Free column: bounds at solver infinity.  Must not produce a
  // huge-bound warning regardless of how large the sentinel is.
  (void)li.add_col(SparseCol {}.free());  // NOLINT

  CHECK(!logs.contains("LP_VALIDATION huge bound"));
  CHECK(li.lp_validation_stats().bound_huge_count == 0);

  // 1e13 finite bound: must produce a huge-bound warning.
  (void)li.add_col(SparseCol {
      // NOLINT
      .uppb = 1e13,
      .cost = 1.0,
  });

  CHECK(logs.contains("LP_VALIDATION huge bound"));
  CHECK(li.lp_validation_stats().bound_huge_count >= 1);
}

TEST_CASE("LP validation: warning cap respected")  // NOLINT
{
  test::LogCapture logs;

  // Cap warnings at 10; we will inject 1000 huge coefficients via
  // set_coeff_raw.
  LinearInterface li;
  li.set_validation_options(make_validation_opts(
      /*coeff_max=*/1e10,
      /*coeff_min=*/1e-10,
      /*bound_max=*/1e12,
      /*rhs_max=*/1e12,
      /*obj_max=*/1e10,
      /*max_warns=*/10));

  // Build a single column to allow 1000 set_coeff_raw writes.
  std::vector<ColIndex> cols;
  cols.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    cols.push_back(li.add_col(SparseCol {
        .uppb = 1.0,
        .cost = 0.0,
    }));
  }
  // Single dummy row so set_coeff_raw has somewhere to land.
  SparseRow row;
  row[cols[0]] = 1.0;
  row.lowb = 0.0;
  row.uppb = 1.0;
  const auto r0 = li.add_row(row);

  // Inject 1000 out-of-band coefficients; only the first 10 should
  // produce a WARN line.
  for (int i = 0; i < 1000; ++i) {
    li.set_coeff_raw(r0, cols[i], 1e15);
  }

  size_t warn_lines = 0;
  for (const auto& msg : logs.messages()) {
    if (msg.contains("LP_VALIDATION huge coefficient")) {
      ++warn_lines;
    }
  }
  CHECK(warn_lines == 10);
  // Counter still tracks every offence (here also includes the seed
  // SparseRow add above which contributed one in-band coeff = 1.0 and
  // therefore did not bump coeff_huge_count).
  CHECK(li.lp_validation_stats().coeff_huge_count == 1000);
}

TEST_CASE("LP validation: clean LP produces zero warnings")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  // 2-bus IEEE-style LP: balanced, 1.0 coefficients, modest bounds.
  const auto g1 = li.add_col(SparseCol {
      .uppb = 200.0,
      .cost = 30.0,
  });
  const auto g2 = li.add_col(SparseCol {
      .uppb = 150.0,
      .cost = 50.0,
  });

  SparseRow balance;
  balance[g1] = 1.0;
  balance[g2] = 1.0;
  balance.lowb = 100.0;
  balance.uppb = 100.0;
  (void)li.add_row(balance);  // NOLINT

  CHECK(li.lp_validation_stats().clean());
  CHECK(!logs.contains("LP_VALIDATION"));
}

TEST_CASE("LP validation: post-equilibration verdict on conditioned LP")
{  // NOLINT
  test::LogCapture logs;

  // Construct a 100-col / 100-row LP whose worst-case |coeff| is 1e8
  // and best is 1e-3.  After Ruiz equilibration the matrix should be
  // close to unit-magnitude → max ~ 1, no WARN.
  LinearProblem lp("verdict_test");

  std::vector<ColIndex> cols;
  cols.reserve(100);
  for (int i = 0; i < 100; ++i) {
    cols.push_back(lp.add_col(SparseCol {
        .uppb = 1000.0,
        .cost = 1.0,
    }));
  }

  for (int i = 0; i < 100; ++i) {
    SparseRow row;
    // diagonal entry with widely-varying scale
    const double diag_val = (i % 2 == 0) ? 1e8 : 1e-3;
    row[cols[static_cast<size_t>(i)]] = diag_val;
    row.lowb = 0.0;
    row.uppb = diag_val;  // make the row reasonable in physical units
    [[maybe_unused]] const auto r = lp.add_row(std::move(row));
  }

  LpMatrixOptions opts;
  opts.compute_stats = true;
  opts.equilibration_method = LpEquilibrationMethod::ruiz;
  opts.validation = make_validation_opts(/*coeff_max=*/1e10);

  const auto flat = lp.flatten(opts);

  // Verdict should report the equilibrated max (≈ 1.0), not the raw
  // 1e8 envelope.  The exact value depends on Ruiz iteration count
  // but must be well below the 1e10 threshold.
  CHECK(flat.stats_max_abs < 1e10);

  // LP_QUALITY info line must have fired.
  CHECK(logs.contains("LP_QUALITY"));
  // No EXCEEDS warning expected on the equilibrated matrix.
  CHECK(!logs.contains("EXCEEDS threshold"));
}

TEST_CASE("LP validation: post-equilibration verdict escalates on huge max")
{  // NOLINT
  test::LogCapture logs;

  // No equilibration → the raw 1e15 coefficient lands as the matrix
  // max, which exceeds the 1e10 default threshold and must produce
  // an EXCEEDS WARN.
  LinearProblem lp("verdict_warn");

  const auto x = lp.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x] = 1e15;
  row.lowb = 0.0;
  row.uppb = 1e16;
  [[maybe_unused]] const auto r = lp.add_row(std::move(row));

  LpMatrixOptions opts;
  opts.compute_stats = true;
  opts.equilibration_method = LpEquilibrationMethod::none;
  opts.validation = make_validation_opts(/*coeff_max=*/1e10);

  const auto flat = lp.flatten(opts);
  CHECK(flat.stats_max_abs >= 1e15);

  CHECK(logs.contains("LP_QUALITY"));
  CHECK(logs.contains("EXCEEDS threshold"));
}

TEST_CASE("LP validation: disabled options short-circuit hooks")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  // Master switch off — every hook must early-return.
  li.set_validation_options(LpValidationOptions {
      .enable = false,
  });

  const auto x = li.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x] = 1e20;  // would normally trigger huge_coeff
  row.lowb = 1e15;  // would normally trigger huge_rhs
  (void)li.add_row(row);  // NOLINT

  CHECK(li.lp_validation_stats().clean());
  CHECK(!logs.contains("LP_VALIDATION"));
}

TEST_CASE("LP validation: huge objective triggers WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  (void)li.add_col(SparseCol {
      // NOLINT
      .uppb = 1.0,
      .cost = 1e12,  // way above obj_warn_max=1e10
  });

  CHECK(logs.contains("LP_VALIDATION huge objective"));
  CHECK(li.lp_validation_stats().obj_huge_count == 1);
}

TEST_CASE("LP validation: huge RHS triggers WARN")  // NOLINT
{
  // Source coverage gap: `note_rhs` had no targeted test (only an
  // indirect "disabled options short-circuit" sanity check that
  // explicitly verified it does NOT fire).  Construct a row whose
  // RHS exceeds `rhs_warn_max` (default 1e12) and confirm the
  // counter ticks plus the WARN line is emitted.  Cross-references
  // the `note_rhs` overload chain (lp_validation.hpp:211 + 253) and
  // the implementation at lp_validation.cpp:98-118.
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  const auto x = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x] = 1.0;
  row.lowb = 1e15;  // huge RHS — exceeds rhs_warn_max
  (void)li.add_row(row);  // NOLINT

  CHECK(logs.contains("LP_VALIDATION huge RHS"));
  CHECK(li.lp_validation_stats().rhs_huge_count >= 1);
}

TEST_CASE("LP validation: huge bound triggers WARN")  // NOLINT
{
  // Sister gap to `note_rhs` — `note_bound` was exercised only via
  // the "solver-infinity is NOT noted" negative test.  This positive
  // test feeds a finite-but-huge bound to confirm the counter and
  // the WARN line.
  test::LogCapture logs;

  LinearInterface li;
  li.set_validation_options(make_validation_opts());

  (void)li.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 1e15,  // huge but finite — exceeds bound_warn_max
      .cost = 1.0,
  });

  CHECK(logs.contains("LP_VALIDATION huge bound"));
  CHECK(li.lp_validation_stats().bound_huge_count >= 1);
}

TEST_CASE(
    "LP validation: log_summary(logger&) overload writes to "
    "the supplied logger")  // NOLINT
{
  // `LpValidationStats::log_summary(spdlog::logger&)` is the
  // explicit-logger overload (lp_validation.cpp:167).  It was
  // previously uncovered — the only summary-emit path in the test
  // suite went through `log_summary()` (default logger).  Capture
  // a custom logger's output and verify the summary lands in it.
  test::LogCapture logs;

  // Build a small "dirty" LP so `clean()` returns false and the
  // summary path emits a real line (the `clean()` early-return is
  // already covered by the "clean LP produces zero warnings" test).
  LinearInterface li;
  li.set_validation_options(make_validation_opts());
  (void)li.add_col(SparseCol {
      // NOLINT
      .uppb = 1.0,
      .cost = 1e12,  // tickles obj_huge
  });

  // Explicit logger — same default sinks, but we hand the reference
  // explicitly to drive the second overload.
  auto& logger = *spdlog::default_logger();
  li.lp_validation_stats().log_summary(logger);

  CHECK(logs.contains("LP_VALIDATION"));
}

// ── Flatten-time LP_QUALITY bound-envelope checks (Phase 3b) ───────────
// Distinct from the add-time `LP_VALIDATION huge bound` hook above: these
// scan the POST-flatten (scaled) bound vectors that are actually handed to
// the solver, flag FREE columns (which the add-time hook deliberately does
// NOT), and use the 1e7 GPU-solver threshold.

TEST_CASE("LP_QUALITY: free column triggers FREE-column WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearProblem lp("free_col_test");
  // A free column (both bounds infinite) plus a benign bounded column so
  // the matrix is non-empty.  GPU first-order solvers cannot project the
  // free column, so flatten() must warn.
  const auto f = lp.add_col(SparseCol {}.free());
  const auto x = lp.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });
  SparseRow row;
  row[f] = 1.0;
  row[x] = 1.0;
  row.greater_equal(1.0);
  (void)lp.add_row(std::move(row));  // NOLINT

  LpMatrixOptions opts;
  opts.compute_stats = true;
  (void)lp.flatten(opts);

  CHECK(logs.contains("LP_QUALITY"));
  CHECK(logs.contains("FREE column"));
}

TEST_CASE(
    "LP_QUALITY: large finite column bound triggers bound WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearProblem lp("big_col_bound");
  // 1e8 finite upper bound — above the 1e7 GPU threshold.  No
  // equilibration so the bound reaches flatten() un-rescaled.
  const auto x = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1e8,
      .cost = 1.0,
  });
  SparseRow row;
  row[x] = 1.0;
  row.greater_equal(1.0);
  (void)lp.add_row(std::move(row));  // NOLINT

  LpMatrixOptions opts;
  opts.compute_stats = true;
  opts.equilibration_method = LpEquilibrationMethod::none;
  (void)lp.flatten(opts);

  CHECK(logs.contains("LP_QUALITY"));
  CHECK(logs.contains("column(s) with |bound|"));
}

TEST_CASE(
    "LP_QUALITY: large finite row bound triggers row-bound WARN")  // NOLINT
{
  test::LogCapture logs;

  LinearProblem lp("big_row_bound");
  const auto x = lp.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
  });
  SparseRow row;
  row[x] = 1.0;
  row.lowb = 0.0;
  row.uppb = 1e9;  // huge finite row upper bound — above 1e7
  (void)lp.add_row(std::move(row));  // NOLINT

  LpMatrixOptions opts;
  opts.compute_stats = true;
  opts.equilibration_method = LpEquilibrationMethod::none;
  (void)lp.flatten(opts);

  CHECK(logs.contains("LP_QUALITY"));
  CHECK(logs.contains("row(s) with |bound|"));
}

TEST_CASE("LP_QUALITY: clean LP emits no FREE/bound lines")  // NOLINT
{
  test::LogCapture logs;

  LinearProblem lp("clean_bounds");
  const auto g1 = lp.add_col(SparseCol {
      .uppb = 200.0,
      .cost = 30.0,
  });
  const auto g2 = lp.add_col(SparseCol {
      .uppb = 150.0,
      .cost = 50.0,
  });
  SparseRow balance;
  balance[g1] = 1.0;
  balance[g2] = 1.0;
  balance.lowb = 100.0;
  balance.uppb = 100.0;
  (void)lp.add_row(std::move(balance));  // NOLINT

  LpMatrixOptions opts;
  opts.compute_stats = true;
  (void)lp.flatten(opts);

  CHECK(!logs.contains("FREE column"));
  CHECK(!logs.contains("with |bound|"));
}

TEST_CASE(
    "LP validation: NaN is a hard ERROR, infinity stays silent")  // NOLINT
{
  // NaN is never legitimate LP data — a NaN row can make the solver
  // iterate forever (observed 2026-07-02: multi-hour CPLEX wedge on a
  // NaN Benders cut).  ±infinity, by contrast, is the standard
  // "no bound" sentinel the solvers treat correctly, so it must keep
  // its silent skip (no behavior change).
  const auto cfg = make_validation_opts();
  constexpr double nan_v = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf_v = std::numeric_limits<double>::infinity();

  SUBCASE("every hook counts NaN and logs at ERROR")
  {
    test::LogCapture logs;
    LpValidationStats st;
    st.note_bound(nan_v, "colX", cfg);
    st.note_rhs(nan_v, "rowY", cfg);
    st.note_coeff(nan_v, "rowY colX", cfg);
    st.note_obj(nan_v, "colX", cfg);
    CHECK(st.nan_count == 4);
    CHECK(st.first_nans.size() == 4);
    CHECK(logs.contains("LP_VALIDATION NaN bound"));
    CHECK(logs.contains("LP_VALIDATION NaN rhs"));
    CHECK(logs.contains("LP_VALIDATION NaN coeff"));
    CHECK(logs.contains("LP_VALIDATION NaN obj"));
    // NaN must not leak into the magnitude counters or min/max tracking.
    CHECK(st.bound_huge_count == 0);
    CHECK(st.rhs_huge_count == 0);
    CHECK(st.coeff_huge_count == 0);
    CHECK(st.max_abs_coeff == doctest::Approx(0.0));
    CHECK(!st.clean());  // total_count() includes nan_count
  }

  SUBCASE("infinity keeps its silent skip (legitimate no-bound sentinel)")
  {
    test::LogCapture logs;
    LpValidationStats st;
    st.note_bound(inf_v, "colX", cfg);
    st.note_bound(-inf_v, "colX", cfg);
    st.note_rhs(inf_v, "rowY", cfg);
    CHECK(st.nan_count == 0);
    CHECK(st.bound_huge_count == 0);
    CHECK(st.rhs_huge_count == 0);
    CHECK(st.clean());
    CHECK(!logs.contains("LP_VALIDATION"));
  }

  SUBCASE("reset clears the NaN state")
  {
    LpValidationStats st;
    st.note_rhs(nan_v, "rowY", cfg);
    REQUIRE(st.nan_count == 1);
    st.reset();
    CHECK(st.nan_count == 0);
    CHECK(st.first_nans.empty());
    CHECK(st.clean());
  }

  SUBCASE("end-to-end: a NaN coefficient through add_row is flagged")
  {
    test::LogCapture logs;
    LinearInterface li;
    li.set_validation_options(cfg);
    const auto x = li.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });
    SparseRow row;
    row[x] = nan_v;
    row.greater_equal(1.0);
    (void)li.add_row(row);  // NOLINT
    CHECK(li.lp_validation_stats().nan_count >= 1);
    CHECK(logs.contains("LP_VALIDATION NaN"));
  }
}

// NOLINTEND(misc-const-correctness)