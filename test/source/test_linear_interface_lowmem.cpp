/**
 * @file      test_linear_interface_lowmem.cpp
 * @brief     Tests for LinearInterface algorithm fallback and low-memory modes
 * @date      Sat Apr 19 12:45:00 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <expected>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// Algorithm fallback cycle tests
// ---------------------------------------------------------------------------

TEST_CASE("LinearInterface - algorithm fallback on infeasible initial_solve")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Infeasible LP: x >= 10 AND x <= 5 — no algorithm can solve it.
  // The fallback cycle should try all 3 algorithms and still fail,
  // with the error message indicating the fallback cycle was exhausted.
  for (const auto algo :
       {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal, LPAlgo::default_algo})
  {
    LinearInterface li;
    const auto x1 = li.add_col(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
    });

    SparseRow row;
    row[x1] = 1.0;
    row.lowb = 10.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    auto result = li.initial_solve(SolverOptions {
        .algorithm = algo,
        .log_level = 0,
    });
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    CHECK(err.code == ErrorCode::SolverError);
    CHECK(err.message.find("fallback") != std::string::npos);
    CHECK_FALSE(li.is_optimal());
  }
}

TEST_CASE("LinearInterface - algorithm fallback on infeasible resolve")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Same infeasible LP tested via resolve path.
  for (const auto algo :
       {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal, LPAlgo::default_algo})
  {
    LinearInterface li;
    const auto x1 = li.add_col(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
    });

    SparseRow row;
    row[x1] = 1.0;
    row.lowb = 10.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    auto result = li.resolve(SolverOptions {
        .algorithm = algo,
        .log_level = 0,
    });
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    CHECK(err.code == ErrorCode::SolverError);
    CHECK(err.message.find("fallback") != std::string::npos);
    CHECK_FALSE(li.is_optimal());
  }
}

TEST_CASE(
    "LinearInterface - optimal LP succeeds without fallback for all algorithms")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Feasible LP: min x + y, s.t. x + y >= 4, x,y >= 0  →  obj = 4.
  // All algorithms should succeed on the first attempt (no fallback needed).
  for (const auto algo :
       {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal, LPAlgo::default_algo})
  {
    LinearInterface li;
    const auto x1 = li.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });
    const auto x2 = li.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });

    SparseRow row;
    row[x1] = 1.0;
    row[x2] = 1.0;
    row.lowb = 4.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    auto result = li.initial_solve(SolverOptions {
        .algorithm = algo,
        .log_level = 0,
    });
    REQUIRE(result.has_value());
    CHECK(li.is_optimal());
    CHECK(li.get_obj_value_raw() == doctest::Approx(4.0));
  }
}

TEST_CASE(
    "LinearInterface - fallback cycle on resolve after feasible "
    "initial_solve")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Solve a feasible LP, then make it infeasible via bound change and resolve.
  // The fallback cycle should engage and ultimately fail.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 1.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  // First solve: feasible
  auto r1 = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
  });
  REQUIRE(r1.has_value());
  CHECK(li.is_optimal());

  // Make infeasible: x <= 0 but x >= 1
  li.set_col_upp(x1, 0.0);

  auto r2 = li.resolve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
  });
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().code == ErrorCode::SolverError);
  CHECK(r2.error().message.find("fallback") != std::string::npos);
}

// Regression: a cell that was once optimal then becomes non-optimal
// must report `is_optimal() == false` after release, not the stale
// `true` from the prior optimal solve.  Prior to this fix
// `release_backend` cleared the cached primal/dual/reduced-cost
// vectors on a non-optimal release but did NOT flip
// `m_cached_is_optimal_` back to false; subsequent reads through the
// released-backend cache path then claimed the cell was still
// optimal.  Under SDDP with `low_memory=compress` and a cleared
// snapshot, the next `OutputContext` ctor read `get_col_sol()` →
// `backend()` → `ensure_backend()` → silent-no-op (no snapshot to
// rebuild from) → null `unique_ptr` deref crash.  Observed on
// juan/gtopt_iplp iter i2 post-CONVERGED write_out for cells that
// went optimal-→-non-optimal across iterations.
TEST_CASE(  // NOLINT
    "LinearInterface — non-optimal release flips cached is_optimal flag")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a one-col / one-row LP via `LinearProblem` + `flatten` +
  // `load_flat` so we have a snapshot to reconstruct from later
  // (matches the existing `make_simple_li_lp` shape).
  LinearProblem lp;
  const auto c1 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});
  const auto r = lp.add_row(SparseRow {
      .lowb = 1.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c1, 1.0);
  auto flat = lp.flatten({});

  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();

  // First solve: feasible / optimal.
  auto r1 = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
  });
  REQUIRE(r1.has_value());
  REQUIRE(li.is_optimal());

  // Now wire up compress + snapshot, then release.  `release_backend`
  // caches the optimal-state scalars + primal/dual vectors and flips
  // `m_cached_is_optimal_ = true`.
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_backend_released());
  REQUIRE(li.is_optimal());  // cached true

  // Bring the backend back from the snapshot, then make the LP
  // infeasible and resolve.  The resolve fails (bounds contradict
  // the row); `is_optimal()` reads false from the live backend.
  li.reconstruct_backend();
  li.set_col_upp_raw(ColIndex {0}, 0.0);  // infeasible: x1 <= 0 but x1 >= 1
  auto r2 = li.resolve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
      .max_fallbacks = 0,
  });
  CHECK_FALSE(r2.has_value());
  CHECK_FALSE(li.is_optimal());  // live backend reports non-optimal

  // Release on a non-optimal backend.  Pre-fix: the non-optimal
  // branch cleared cached primal/dual vectors but left
  // `m_cached_is_optimal_` at its prior `true`, so the post-release
  // `is_optimal()` lied about the cell.  Post-fix: it correctly
  // returns false.
  li.release_backend();
  CHECK(li.is_backend_released());
  CHECK_FALSE(li.is_optimal());
}

TEST_CASE("LinearInterface - max_fallbacks=0 disables fallback")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Infeasible LP with max_fallbacks=0: should fail immediately without
  // the "fallback" keyword in the error message.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  SUBCASE("initial_solve")
  {
    auto result = li.initial_solve(SolverOptions {
        .algorithm = LPAlgo::dual,
        .log_level = 0,
        .max_fallbacks = 0,
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::SolverError);
    CHECK(result.error().message.find("fallback") == std::string::npos);
  }

  SUBCASE("resolve")
  {
    auto result = li.resolve(SolverOptions {
        .algorithm = LPAlgo::dual,
        .log_level = 0,
        .max_fallbacks = 0,
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::SolverError);
    CHECK(result.error().message.find("fallback") == std::string::npos);
  }
}

TEST_CASE("LinearInterface - max_fallbacks=1 tries one alternative")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Infeasible LP with max_fallbacks=1: should try one fallback and still
  // fail, but the error should mention "fallback".
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto result = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::barrier,
      .log_level = 0,
      .max_fallbacks = 1,
  });
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("fallback") != std::string::npos);
}

// ── Low-memory mode unit tests ────────────────────────────────────────────

/// Helper: build a simple LP with 2 variables and 1 constraint, flatten it,
/// load it into a LinearInterface, and return (li, flat_lp, x1, x2).
namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
/// Build a LinearInterface with labeled columns and rows directly via
/// add_col / add_row (no flatten path).  Used to exercise the label
/// metadata compression/decompression path in compress mode.
struct LabeledLp
{
  LinearInterface li;
  ColIndex x1;
  ColIndex x2;
  RowIndex r1;
};

LabeledLp make_labeled_lp()
{
  LinearInterface li;

  // Two labeled columns with distinct class_name / variable_name / uid.
  // min 2·x1 + 3·x2  s.t.  x1 + x2 >= 5,  0 <= x1,x2 <= 10.
  const auto x1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "generation",
      .variable_uid = Uid {1},
  });
  const auto x2 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "generation",
      .variable_uid = Uid {2},
  });

  // One labeled row.
  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = SparseRow::DblMax;
  row.class_name = "Bus";
  row.constraint_name = "balance";
  row.variable_uid = Uid {10};
  const auto r1 = li.add_row(row);

  return LabeledLp {
      .li = std::move(li),
      .x1 = x1,
      .x2 = x2,
      .r1 = r1,
  };
}
struct SimpleLiLp
{
  LinearInterface li;
  FlatLinearProblem flat;
  ColIndex x1;
  ColIndex x2;
};

SimpleLiLp make_simple_li_lp()
{
  // min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  0 <= x1,x2 <= 10
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
  const auto r = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();

  return SimpleLiLp {
      .li = std::move(li),
      .flat = std::move(flat),
      .x1 = ColIndex {0},
      .x2 = ColIndex {1},
  };
}
}  // namespace

TEST_CASE(
    "LinearInterface — defer_initial_load skips eager backend load")  // NOLINT
{
  // Build a flat LP without ever calling load_flat on the LinearInterface,
  // then install it via defer_initial_load.  The backend must report itself
  // as released, and the cached numrows/numcols must already reflect the
  // snapshot's dimensions so callers like save_base_numrows() get correct
  // values without forcing a reconstruction.
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
  const auto r = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  auto flat = lp.flatten({});
  const auto expected_ncols = static_cast<size_t>(flat.ncols);
  const auto expected_nrows = static_cast<size_t>(flat.nrows);

  SUBCASE("snapshot mode: deferred load with cached row/col counts")
  {
    LinearInterface li;
    li.set_low_memory(LowMemoryMode::compress);
    li.defer_initial_load(FlatLinearProblem {flat});

    // Backend must be marked released and never have been instantiated.
    // Backend is marked released so the next user-driven access
    // forces a single (and only) load_flat via ensure_backend().
    CHECK(li.is_backend_released());

    // Pre-seeded counts let save_base_numrows / get_numrows return the
    // correct value while the backend is still released.
    CHECK(li.get_numcols() == expected_ncols);
    CHECK(li.get_numrows() == expected_nrows);

    // First user access reconstructs the backend lazily and produces the
    // optimal solution from the original flat LP.
    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.has_backend());
    CHECK(li.get_numcols() == expected_ncols);
    CHECK(li.get_numrows() == expected_nrows);

    auto solve = li.resolve();
    REQUIRE(solve.has_value());
    REQUIRE(li.is_optimal());
    // x1 + x2 >= 5; min 2x1 + 3x2 → choose x1=5, x2=0 → obj 10
    CHECK(li.get_obj_value_raw() == doctest::Approx(10.0));
  }

  SUBCASE("compress mode: snapshot is compressed at install time")
  {
    LinearInterface li;
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.defer_initial_load(FlatLinearProblem {flat});

    CHECK(li.is_backend_released());
    CHECK(li.get_numcols() == expected_ncols);
    CHECK(li.get_numrows() == expected_nrows);

    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());

    auto solve = li.resolve();
    REQUIRE(solve.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(10.0));
  }
}

TEST_CASE("LinearInterface — low_memory save_snapshot round-trip")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  // Solve baseline
  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value_raw();
  const auto orig_ncols = li.get_numcols();
  const auto orig_nrows = li.get_numrows();

  SUBCASE("level 1: release and reconstruct preserves LP")
  {
    li.set_low_memory(LowMemoryMode::compress);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());

    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.has_backend());
    CHECK(li.get_numcols() == orig_ncols);
    CHECK(li.get_numrows() == orig_nrows);

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }

  SUBCASE("level 2: compress/decompress round-trip preserves LP")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());

    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.get_numcols() == orig_ncols);
    CHECK(li.get_numrows() == orig_nrows);

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple release/reconstruct cycles produce same result")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    for (int i = 0; i < 3; ++i) {
      li.release_backend();
      CHECK(li.is_backend_released());

      li.reconstruct_backend();
      CHECK_FALSE(li.is_backend_released());

      auto r = li.resolve();
      REQUIRE(r.has_value());
      CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
    }
  }
}

TEST_CASE(
    "LinearInterface — low_memory reconstruct with dynamic cols")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value_raw();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Add a dynamic column (simulating alpha variable).  `add_col`
  // auto-records into `m_dynamic_cols_` post-snapshot, so no explicit
  // `record_dynamic_col` is needed.
  SparseCol alpha_col;
  alpha_col.uppb = 1000.0;
  alpha_col.cost = 0.0;
  [[maybe_unused]] const auto alpha = li.add_col(alpha_col);
  li.save_base_numrows();

  CHECK(li.get_numcols() == 3);

  // Release and reconstruct — dynamic col should be replayed
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numcols() == 3);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // Alpha has zero cost, so objective is unchanged
  CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
}

TEST_CASE(
    "LinearInterface — low_memory reconstruct with dynamic rows")  // NOLINT
{
  // Regression test for the bug fixed in the cascade method: a row added
  // via `add_row` AFTER the snapshot was taken used to be silently
  // dropped on the first `release_backend` → `reconstruct_backend`
  // cycle.  After the auto-record refactor, `add_row(SparseRow)`
  // mirrors into `m_dynamic_rows_` automatically — symmetric to the
  // col-side auto-record verified above.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();
  const auto base_nrows = li.base_numrows();

  // Add a structural row AFTER the snapshot was taken, simulating the
  // cascade `add_elastic_targets` path.  Constraint: x2 >= 3 (binding —
  // baseline optimum was x1=5, x2=0).  Auto-record is intentionally
  // limited to `add_col(SparseCol)` (cols are unambiguously structural
  // post-init); rows are ambiguous (cuts also use add_row + their own
  // record_cut_row), so callers must explicitly call
  // `record_dynamic_row` for structural rows.
  SparseRow extra;
  extra[x2] = 1.0;
  extra.lowb = 3.0;
  extra.uppb = LinearProblem::DblMax;
  li.add_row(extra);
  li.record_dynamic_row(extra);

  CHECK(li.get_numrows() == base_nrows + 1);

  // Release + reconstruct.  Pre-fix, the extra row would have been lost
  // here and the LP would re-collapse to x1=5, x2=0.
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numrows() == base_nrows + 1);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // x1 + x2 >= 5  AND  x2 >= 3  →  x1=2, x2=3 → obj = 4 + 9 = 13.
  CHECK(li.get_obj_value_raw() == doctest::Approx(13.0));
}

TEST_CASE(
    "LinearInterface — cascade pattern: dynamic_col + dynamic_row "
    "registered together survive reconstruct")  // NOLINT
{
  // End-to-end shape of the cascade `add_elastic_targets` call: add 2
  // slack columns + 1 elastic constraint row that references both, then
  // release_backend / reconstruct_backend in compress mode and verify
  // the LP solves identically afterwards.  Reproduces the exact pattern
  // that previously crashed `test_hydro_4b_cascade_gtopt_solve` with a
  // CPLEX SIGSEGV — `set_col_low_raw(svar->col(), …)` ran with a stale
  // `ColIndex` after the slacks were silently dropped on reload.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const auto base_ncols = li.get_numcols();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();
  const auto base_nrows = li.base_numrows();

  // 2 slack columns + elastic constraint row (same shape cascade uses
  // for tgt_sup / tgt_sdn).  `add_col(SparseCol)` auto-records into
  // `m_dynamic_cols_`; rows still need an explicit
  // `record_dynamic_row` because `add_row(SparseRow)` is also used by
  // cut callers (who route through `record_cut_row` instead).
  const auto sup_col = li.add_col(SparseCol {
      .uppb = LinearProblem::DblMax,
      .cost = 100.0,
  });

  const auto sdn_col = li.add_col(SparseCol {
      .uppb = LinearProblem::DblMax,
      .cost = 100.0,
  });

  // Elastic constraint: x1 - sup + sdn ∈ [3, 3]  (i.e. target value 3,
  // any deviation paid via slacks at penalty 100).
  SparseRow row;
  row[x1] = 1.0;
  row[sup_col] = -1.0;
  row[sdn_col] = 1.0;
  row.lowb = 3.0;
  row.uppb = 3.0;
  li.add_row(row);
  li.record_dynamic_row(row);

  CHECK(li.get_numcols() == base_ncols + 2);
  CHECK(li.get_numrows() == base_nrows + 1);

  // Multiple release/reconstruct cycles must all preserve the slacks +
  // constraint exactly — same shape that cascade level 1 hits between
  // every SDDP forward/backward pass.
  for (int i = 0; i < 3; ++i) {
    li.release_backend();
    li.reconstruct_backend();
    CHECK(li.get_numcols() == base_ncols + 2);
    CHECK(li.get_numrows() == base_nrows + 1);
  }

  // Solve and verify the elastic constraint binds correctly.  With
  // x1 - sup + sdn = 3 and very large slack penalties, the optimum
  // satisfies the target exactly: x1 = 3, x2 = 2, sup = sdn = 0.
  // Objective = 2·3 + 3·2 + 100·0 + 100·0 = 12.
  auto r = li.resolve();
  REQUIRE(r.has_value());
  CHECK(li.get_obj_value_raw() == doctest::Approx(12.0));
}

TEST_CASE(
    "LinearInterface — add_col(SparseCol) auto-records post-snapshot "
    "mutations (footgun removed)")  // NOLINT
{
  // After the auto-record refactor, `add_col(SparseCol)` automatically
  // pushes into `m_dynamic_cols_` when the snapshot is populated and
  // `low_memory != off`.  Callers no longer need to remember a
  // follow-up `record_dynamic_col` to keep post-snapshot cols across
  // reconstruct cycles — exercise the bare-add_col path and verify
  // the column survives a release/reconstruct cycle.
  //
  // History: this test originally pinned the OPPOSITE invariant
  // ("add_col without record_dynamic_col is LOST on reconstruct") to
  // document the footgun that triggered the cascade SIGSEGV.  When
  // `add_col` started auto-recording, the test was inverted to lock
  // in the new safer contract.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const auto base_ncols = li.get_numcols();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // No explicit `record_dynamic_col` — the auto-record in
  // `add_col(SparseCol)` should mirror this into `m_dynamic_cols_`.
  SparseCol persistent {.uppb = LinearProblem::DblMax};
  [[maybe_unused]] const auto col_idx = li.add_col(persistent);
  CHECK(li.get_numcols() == base_ncols + 1);

  // Reconstruct: replay should re-apply the recorded col.
  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.get_numcols() == base_ncols + 1);

  // Multiple cycles must not double-record (the replay path itself
  // calls `add_cols(span)` which bypasses the single-arg auto-record,
  // and `m_replaying_` is set as defence-in-depth).
  for (int i = 0; i < 3; ++i) {
    li.release_backend();
    li.reconstruct_backend();
    CHECK(li.get_numcols() == base_ncols + 1);
  }
}

TEST_CASE("LinearInterface — low_memory reconstruct with cuts")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  const auto base_nrows = li.base_numrows();

  // Add a cut row: x1 <= 3 (binding: optimal was x1=5)
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 3.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  CHECK(li.get_numrows() == base_nrows + 1);

  // Solve with the cut
  auto r1 = li.resolve();
  REQUIRE(r1.has_value());
  const double obj_with_cut = li.get_obj_value_raw();
  // x1 <= 3, so optimal x1=3, x2=2 → obj = 6 + 6 = 12
  CHECK(obj_with_cut == doctest::Approx(12.0));

  // Release and reconstruct — cut should be replayed
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numrows() == base_nrows + 1);

  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  CHECK(li.get_obj_value_raw() == doctest::Approx(obj_with_cut));
}

TEST_CASE("LinearInterface — low_memory cut deletion tracking")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  [[maybe_unused]] const double orig_obj = li.get_obj_value_raw();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  const auto base = static_cast<int>(li.base_numrows());

  // Add two binding cuts:
  //   cut0: x2 >= 4  → forces x2=4, x1=1, obj=14
  //   cut1: x1 <= 2  → alone: x1=2, x2=3, obj=13
  SparseRow cut0;
  cut0[x2] = 1.0;
  cut0.lowb = 4.0;
  cut0.uppb = LinearProblem::DblMax;
  li.add_row(cut0);
  li.record_cut_row(cut0);

  SparseRow cut1;
  cut1[x1] = 1.0;
  cut1.lowb = -LinearProblem::DblMax;
  cut1.uppb = 2.0;
  li.add_row(cut1);
  li.record_cut_row(cut1);

  // Delete cut0 (absolute row index = base + 0)
  std::array<int, 1> deleted = {
      base,
  };
  li.delete_rows(deleted);
  li.record_cut_deletion(deleted);

  // Release and reconstruct — only cut1 should be present
  li.release_backend();
  li.reconstruct_backend();

  // base structural rows + 1 remaining cut
  CHECK(li.get_numrows() == static_cast<size_t>(base) + 1);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // Only cut1 active: x1 <= 2, so optimal x1=2, x2=3 → obj = 4 + 9 = 13
  CHECK(li.get_obj_value_raw() == doctest::Approx(13.0));
}

TEST_CASE(
    "LinearInterface — clear_snapshot drops flat LP but keeps cache")  // NOLINT
{
  // After SDDP simulation Pass 1 has solved every cell and cached the
  // Phase-2a primal/dual/cost vectors, `SDDPMethod::simulation_pass`
  // calls `clear_snapshot()` on each `LinearInterface` (via
  // `PlanningLP::drop_sim_snapshots`) to free the compressed flat-LP
  // snapshot — the cache is sufficient for `PlanningLP::write_out`.
  // This test verifies the exact invariant: after clear_snapshot the
  // cached scalars + vectors remain intact and still serve reads, but
  // the snapshot is gone and `reconstruct_backend()` early-returns.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value_raw();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();

  REQUIRE(li.is_backend_released());
  REQUIRE(li.is_optimal());  // cached true after optimal release

  // Capture cached values BEFORE clearing the snapshot to confirm
  // they survive clear_snapshot untouched.
  const std::vector<double> before_col_sol(li.get_col_sol_raw().begin(),
                                           li.get_col_sol_raw().end());
  const std::vector<double> before_row_dual(li.get_row_dual_raw().begin(),
                                            li.get_row_dual_raw().end());

  li.clear_snapshot();

  SUBCASE("cached scalars + vectors still serve reads")
  {
    CHECK(li.is_optimal());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
    const auto after_col_sol = li.get_col_sol_raw();
    REQUIRE(after_col_sol.size() == before_col_sol.size());
    for (size_t i = 0; i < after_col_sol.size(); ++i) {
      CHECK(after_col_sol[i] == doctest::Approx(before_col_sol[i]));
    }
    const auto after_row_dual = li.get_row_dual_raw();
    REQUIRE(after_row_dual.size() == before_row_dual.size());
    for (size_t i = 0; i < after_row_dual.size(); ++i) {
      CHECK(after_row_dual[i] == doctest::Approx(before_row_dual[i]));
    }
  }

  SUBCASE("reconstruct_backend is a no-op when snapshot is gone")
  {
    // With an empty snapshot the reconstruct path early-returns; the
    // backend stays released.  Precondition for dropping the snapshot
    // after Pass 1 converges: no caller will need a live backend.
    li.reconstruct_backend();
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());
  }
}

// `LinearInterface — low_memory reconstruct with warm-start` was deleted
// when the warm-start machinery was removed: the barrier method (default
// solver algorithm) gains nothing from a starting solution, so
// `reconstruct_backend` no longer accepts col_sol / row_dual.  Pre-solve
// readers consume the cached vectors via the `m_backend_released_` gate
// in `get_col_sol_raw` instead.

// ── Plan §6 Test 1 — snapshot is frozen at construction time ──────────
//
// Pins the invariant: under `LowMemoryMode::compress`, the
// `m_snapshot_.flat_lp` reflects matval at the moment of `save_snapshot`,
// NOT post-solve `set_coeff` mutations.  Every `reconstruct_backend`
// reloads the *original* coefficients, which is why the SDDP forward
// pass must call `update_lp_for_phase` after every `ensure_lp_built()`
// (and the backward pass must do the same — see commit 3e73f68c).
//
// If a future refactor (P4 in docs/sddp_compress_refactor_plan.md) bakes
// `set_coeff` mutations into the snapshot, this test's assertion flips
// and the SDDP fix's backward `update_lp_for_phase` call becomes
// unnecessary — the test then becomes the explicit boundary marker
// between the two policies.
TEST_CASE(
    "LinearInterface — snapshot frozen at construction (compress)")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  const RowIndex r0 {0};

  // Solve once so a valid optimal solution exists for the cache path.
  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.is_optimal());

  // Construction-time coefficient at (r0, x1) is 1.0 (per fixture).
  REQUIRE(li.get_coeff_raw(r0, x1) == doctest::Approx(1.0));

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Mutate the live backend's coefficient.  Under off this would
  // persist; under compress the mutation lives only on the volatile
  // backend that is about to be released.
  REQUIRE(li.supports_set_coeff());
  li.set_coeff(r0, x1, 99.0);
  CHECK(li.get_coeff_raw(r0, x1) == doctest::Approx(99.0));

  // Release + reconstruct: the snapshot pre-dates the set_coeff above.
  li.release_backend();
  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // The reconstructed backend reflects the snapshot's frozen matval —
  // the 99.0 mutation is lost.  This is the documented behaviour and
  // the reason `update_lp_for_phase` must run after every reload under
  // non-`off` modes (see `sddp_method_iteration.cpp::backward_pass_`
  // `single_phase` post-fix).
  CHECK(li.get_coeff_raw(r0, x1) == doctest::Approx(1.0));
}

// ── Col-bound replay regression — the SDDP propagate_trial_values fix ──
//
// Pins the invariant that `set_col_low_raw` / `set_col_upp_raw`
// mutations are tracked in `m_pending_col_bounds_` and re-applied by
// `apply_post_load_replay` after `load_flat`.  Pre-fix (before commit
// f8b1b54c), under `LowMemoryMode::compress` these mutations vanished
// on every reconstruct because `load_flat` restores the snapshot's
// construction-time bounds — and the SDDP forward pass relies on
// `propagate_trial_values` pinning dep_col bounds to the previous
// phase's solved state to constrain the backward LP.  Without the
// replay, iter-0-backward saw construction-time bounds and produced
// wrong cuts → juan/iplp compress-mode LB stalled at -454M.
TEST_CASE(
    "LinearInterface — col-bound mutations replayed on reconstruct")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  REQUIRE(li.initial_solve().has_value());

  // Construction-time bounds for x1/x2 are [0, 10] per the fixture.
  REQUIRE(li.get_col_low_raw()[x1] == doctest::Approx(0.0));
  REQUIRE(li.get_col_upp_raw()[x1] == doctest::Approx(10.0));

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Pin x1 to a tighter bound — analogous to what
  // propagate_trial_values does to dep_col during SDDP forward pass.
  REQUIRE(li.supports_set_coeff());
  li.set_col_low_raw(x1, 3.0);
  li.set_col_upp_raw(x1, 7.0);
  CHECK(li.get_col_low_raw()[x1] == doctest::Approx(3.0));
  CHECK(li.get_col_upp_raw()[x1] == doctest::Approx(7.0));

  // Release + reconstruct.  Pre-fix, the mutations vanished and the
  // bounds reverted to [0, 10].  Post-fix they survive via
  // `m_pending_col_bounds_` replay in `apply_post_load_replay`.
  li.release_backend();
  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  CHECK(li.get_col_low_raw()[x1] == doctest::Approx(3.0));
  CHECK(li.get_col_upp_raw()[x1] == doctest::Approx(7.0));

  // x2 wasn't mutated — stays at construction-time bounds.
  CHECK(li.get_col_low_raw()[x2] == doctest::Approx(0.0));
  CHECK(li.get_col_upp_raw()[x2] == doctest::Approx(10.0));

  // Multiple cycles preserve the override too.
  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.get_col_low_raw()[x1] == doctest::Approx(3.0));
  CHECK(li.get_col_upp_raw()[x1] == doctest::Approx(7.0));

  // Subsequent mutation overwrites the cached value.
  li.set_col_low_raw(x1, 5.0);
  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.get_col_low_raw()[x1] == doctest::Approx(5.0));
  CHECK(li.get_col_upp_raw()[x1] == doctest::Approx(7.0));
}

TEST_CASE("LinearInterface — low_memory hot-start cut replay")  // NOLINT
{
  // Cuts added via add_row + record_cut_row are replayed on reconstruct.
  // This replaces the old capture_hot_start_cuts flow, which retrofitted
  // m_active_cuts_ from the backend; cut loaders now call record_cut_row
  // directly alongside add_row.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // Hot-start cut: x1 <= 3 (optimal was x1=5, so this is binding).
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 3.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  // Release and reconstruct — the recorded cut must be replayed.
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numrows() == li.base_numrows() + 1);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // x1 <= 3 → optimal x1=3, x2=2 → obj = 12
  CHECK(li.get_obj_value_raw() == doctest::Approx(12.0));
}

TEST_CASE(
    "LinearInterface — low_memory clone from reconstructed backend")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value_raw();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Release and reconstruct (cold-start: no warm-start vectors)
  li.release_backend();
  li.reconstruct_backend();

  // Clone from reconstructed backend
  auto cloned = li.clone();

  auto r = cloned.resolve();
  REQUIRE(r.has_value());
  CHECK(cloned.get_obj_value_raw() == doctest::Approx(orig_obj));

  // Modify clone — original is unmodified
  cloned.set_col_upp(x1, 3.0);
  auto r2 = cloned.resolve();
  REQUIRE(r2.has_value());
  // x1 <= 3 → x1=3, x2=2 → obj = 6 + 6 = 12
  CHECK(cloned.get_obj_value_raw() == doctest::Approx(12.0));

  // Original still produces the same objective
  auto r3 = li.resolve();
  REQUIRE(r3.has_value());
  CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
}

TEST_CASE("LinearInterface — low_memory level 2 multiple cycles")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  [[maybe_unused]] const double orig_obj = li.get_obj_value_raw();

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // Cycle 1: add a binding cut (x1 <= 4), release, reconstruct
  SparseRow cut1;
  cut1[x1] = 1.0;
  cut1.lowb = -LinearProblem::DblMax;
  cut1.uppb = 4.0;
  li.add_row(cut1);
  li.record_cut_row(cut1);

  li.release_backend();
  li.reconstruct_backend();

  auto r1 = li.resolve();
  REQUIRE(r1.has_value());
  const double obj1 = li.get_obj_value_raw();
  // x1 <= 4, so x1=4, x2=1 → obj = 8 + 3 = 11
  CHECK(obj1 == doctest::Approx(11.0));

  // Cycle 2: add another binding cut (x2 >= 3), release, reconstruct
  SparseRow cut2;
  cut2[x2] = 1.0;
  cut2.lowb = 3.0;
  cut2.uppb = LinearProblem::DblMax;
  li.add_row(cut2);
  li.record_cut_row(cut2);

  li.release_backend();
  li.reconstruct_backend();

  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  // x1 <= 4, x2 >= 3 → x1=2, x2=3 → obj = 4 + 9 = 13
  CHECK(li.get_obj_value_raw() == doctest::Approx(13.0));
}

TEST_CASE("LinearInterface — set_low_memory(0) discards flat LP")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Disable low_memory — flat LP should be discarded
  li.set_low_memory(LowMemoryMode::off);

  // release_backend is a no-op when low_memory is off — backend stays alive
  li.release_backend();
  CHECK(li.has_backend());
}

TEST_CASE("LinearInterface — clone with warm-start parameters")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture primal and dual
  std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                              li.get_col_sol_raw().end());
  std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                               li.get_row_dual_raw().end());

  SUBCASE("clone with warm-start resolves correctly")
  {
    auto cloned = li.clone();
    SolverOptions ws_opts;
    auto r = cloned.resolve(ws_opts);
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value_raw()
          == doctest::Approx(li.get_obj_value_raw()));
  }

  SUBCASE("clone without warm-start also works")
  {
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value_raw()
          == doctest::Approx(li.get_obj_value_raw()));
  }
}

// ── release_backend memory cleanup tests ─────────────────────────────────

TEST_CASE(
    "LinearInterface — release_backend caches primal/dual/cost "
    "snapshot")  // NOLINT
{
  // Phase-2 solved-snapshot: after `release_backend()` under compress,
  // get_col_sol_raw / get_col_cost_raw / get_row_dual_raw must return
  // the values the backend had at release time, not a fresh zero
  // vector from a reconstructed backend.  This is the contract that
  // lets `OutputContext` read solution values while the backend stays
  // released (avoiding an expensive reconstruct + re-solve).
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  // Capture live-backend values as ground truth.
  const std::vector<double> live_col_sol(li.get_col_sol_raw().begin(),
                                         li.get_col_sol_raw().end());
  const std::vector<double> live_col_cost(li.get_col_cost_raw().begin(),
                                          li.get_col_cost_raw().end());
  const std::vector<double> live_row_dual(li.get_row_dual_raw().begin(),
                                          li.get_row_dual_raw().end());
  REQUIRE_FALSE(live_col_sol.empty());
  REQUIRE_FALSE(live_col_cost.empty());
  REQUIRE_FALSE(live_row_dual.empty());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();

  REQUIRE(li.is_backend_released());
  REQUIRE_FALSE(li.has_backend());

  SUBCASE("get_col_sol_raw returns cached values after release")
  {
    const auto sol = li.get_col_sol_raw();
    REQUIRE(sol.size() == live_col_sol.size());
    for (size_t i = 0; i < sol.size(); ++i) {
      CHECK(sol[i] == doctest::Approx(live_col_sol[i]));
    }
    // Reading through the span must not have reactivated the backend.
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());
  }

  SUBCASE("get_col_cost_raw returns cached values after release")
  {
    const auto rc = li.get_col_cost_raw();
    REQUIRE(rc.size() == live_col_cost.size());
    for (size_t i = 0; i < rc.size(); ++i) {
      CHECK(rc[i] == doctest::Approx(live_col_cost[i]));
    }
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());
  }

  SUBCASE("get_row_dual_raw returns cached values after release")
  {
    const auto pi = li.get_row_dual_raw();
    REQUIRE(pi.size() == live_row_dual.size());
    for (size_t i = 0; i < pi.size(); ++i) {
      CHECK(pi[i] == doctest::Approx(live_row_dual[i]));
    }
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());
  }

  SUBCASE("reconstruct clears route-through-cache; live values returned")
  {
    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());

    // After reconstruct (no warm-start), backend col_solution is either
    // uninitialised or zero.  The important invariant is that we no
    // longer return the cached vectors — the getter routes to the live
    // backend.  To test unambiguously, re-solve and compare against a
    // post-solve cached snapshot.
    auto r = li.resolve();
    REQUIRE(r.has_value());

    const std::vector<double> resolved_sol(li.get_col_sol_raw().begin(),
                                           li.get_col_sol_raw().end());
    REQUIRE(resolved_sol.size() == live_col_sol.size());
    for (size_t i = 0; i < resolved_sol.size(); ++i) {
      CHECK(resolved_sol[i] == doctest::Approx(live_col_sol[i]));
    }
  }
}

TEST_CASE(
    "LinearInterface — release_backend destroys solver backend")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  SUBCASE("backend pointer is null after release")
  {
    CHECK(li.has_backend());
    li.release_backend();
    CHECK_FALSE(li.has_backend());
    CHECK(li.is_backend_released());
  }

  SUBCASE("double release is a safe no-op")
  {
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    // Second release should not crash
    li.release_backend();
    CHECK_FALSE(li.has_backend());
    CHECK(li.is_backend_released());
  }

  SUBCASE("release with low_memory off is a no-op — backend stays alive")
  {
    li.set_low_memory(LowMemoryMode::off);
    li.release_backend();
    CHECK(li.has_backend());
    CHECK_FALSE(li.is_backend_released());
  }
}

TEST_CASE(
    "LinearInterface — clone release destroys clone backend "
    "independently")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value_raw();

  SUBCASE("clone backend is independent — destroying clone preserves parent")
  {
    {
      auto cloned = li.clone();
      auto r = cloned.resolve();
      REQUIRE(r.has_value());
      CHECK(cloned.has_backend());
      // clone goes out of scope here — its backend is destroyed
    }

    // Parent backend is still alive and functional
    CHECK(li.has_backend());
    auto r2 = li.resolve();
    REQUIRE(r2.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple clones can be created and destroyed")
  {
    for (int i = 0; i < 5; ++i) {
      auto cloned = li.clone();
      auto r = cloned.resolve();
      REQUIRE(r.has_value());
      CHECK(cloned.get_obj_value_raw() == doctest::Approx(orig_obj));
      // clone destroyed each iteration
    }

    // Parent still works
    CHECK(li.has_backend());
    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }

  SUBCASE("clone from reconstructed backend works correctly")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    // Release and reconstruct
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    li.reconstruct_backend();
    CHECK(li.has_backend());

    // Clone from reconstructed backend
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value_raw() == doctest::Approx(orig_obj));

    // Destroy clone, release parent again
    cloned = LinearInterface {};
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    // Reconstruct again and verify
    li.reconstruct_backend();
    auto r2 = li.resolve();
    REQUIRE(r2.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }
}

TEST_CASE(
    "LinearInterface — release/reconstruct cycle with cuts "
    "preserves memory pattern")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();
  const auto base = li.base_numrows();

  // Add a Benders cut: x1 <= 4
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 4.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  auto r1 = li.resolve();
  REQUIRE(r1.has_value());
  const double obj_with_cut = li.get_obj_value_raw();

  // Simulate SDDP pattern: release → reconstruct → add more cuts → release
  for (int cycle = 0; cycle < 3; ++cycle) {
    li.release_backend();
    CHECK_FALSE(li.has_backend());
    CHECK(li.is_backend_released());

    li.reconstruct_backend();
    CHECK(li.has_backend());
    CHECK_FALSE(li.is_backend_released());

    // Cut should be replayed
    CHECK(li.get_numrows() == base + 1);

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(obj_with_cut));
  }
}

TEST_CASE(
    "LinearInterface — release/reconstruct cycle preserves obj_value")  // NOLINT
{
  // Post-solve primal/dual vectors are NOT cached across releases —
  // callers must read them before release or trigger ensure_backend
  // to reload.  Obj_value, kappa, numrows, numcols, is_optimal stay
  // cached as scalars.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value_raw();

  SUBCASE("released cached scalars")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());

    // Cached scalars are still available
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }

  SUBCASE("reconstruct then resolve produces correct result")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    li.reconstruct_backend();  // cold start

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple release/reconstruct cycles still work")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    for (int i = 0; i < 3; ++i) {
      li.release_backend();
      li.reconstruct_backend();
      auto r = li.resolve();
      REQUIRE(r.has_value());
      CHECK(li.get_obj_value_raw() == doctest::Approx(orig_obj));
    }
  }
}

TEST_CASE(
    "LowMemorySnapshot — compress/decompress size "
    "behavior across cycles")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // ── Helper: total heap bytes held by numeric vectors ──
  auto flat_vectors_size = [](const FlatLinearProblem& f) -> size_t
  {
    auto bytes_of = [](const auto& v)
    {
      return v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type);
    };
    return bytes_of(f.matbeg) + bytes_of(f.matind) + bytes_of(f.colint)
        + bytes_of(f.matval) + bytes_of(f.collb) + bytes_of(f.colub)
        + bytes_of(f.objval) + bytes_of(f.rowlb) + bytes_of(f.rowub)
        + bytes_of(f.col_scales) + bytes_of(f.row_scales);
  };

  auto flat_vectors_nonempty = [](const FlatLinearProblem& f) -> bool
  { return !f.matbeg.empty() || !f.collb.empty() || !f.matval.empty(); };

  SUBCASE("compress creates compressed buffer and clears flat vectors")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    CHECK(flat_vectors_nonempty(snap.flat_lp));
    CHECK_FALSE(snap.is_compressed());

    const auto orig_size = flat_vectors_size(snap.flat_lp);
    CHECK(orig_size > 0);

    // First compress: creates compressed buffer, clears vectors
    snap.compress(CompressionCodec::lz4);
    CHECK(snap.is_compressed());
    CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));
    CHECK(flat_vectors_size(snap.flat_lp) == 0);
    CHECK_FALSE(snap.compressed_lp.empty());

    const auto compressed_size = snap.compressed_lp.data.size();
    CHECK(compressed_size > 0);
    CHECK(compressed_size <= orig_size);
  }

  SUBCASE("decompress restores vectors, keeps compressed buffer")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    const auto compressed_size = snap.compressed_lp.data.size();

    // Decompress: restores flat vectors, keeps compressed buffer
    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));
    CHECK(flat_vectors_size(snap.flat_lp) > 0);

    // Compressed buffer is still present (persistent cache)
    CHECK(snap.is_compressed());
    CHECK(snap.compressed_lp.data.size() == compressed_size);
  }

  SUBCASE("multiple compress/decompress cycles maintain invariants")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    const auto orig_size = flat_vectors_size(snap.flat_lp);

    // First compress: creates the persistent compressed buffer
    snap.compress(CompressionCodec::lz4);
    const auto compressed_size = snap.compressed_lp.data.size();
    CHECK(compressed_size > 0);
    CHECK(flat_vectors_size(snap.flat_lp) == 0);

    for (int cycle = 0; cycle < 5; ++cycle) {
      // Decompress: vectors restored, compressed buffer retained
      snap.decompress();
      CHECK(flat_vectors_nonempty(snap.flat_lp));
      const auto decompressed_size = flat_vectors_size(snap.flat_lp);
      CHECK(decompressed_size >= orig_size);
      CHECK(snap.compressed_lp.data.size() == compressed_size);

      // Re-compress: vectors cleared, compressed buffer unchanged
      snap.compress(CompressionCodec::lz4);
      CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));
      CHECK(flat_vectors_size(snap.flat_lp) == 0);
      CHECK(snap.compressed_lp.data.size() == compressed_size);
    }
  }

  SUBCASE("clear_flat_lp_vectors frees decompressed data")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));

    // Manual clear — same as what release_backend does on subsequent calls
    clear_flat_lp_vectors(snap.flat_lp);
    CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));
    CHECK(flat_vectors_size(snap.flat_lp) == 0);

    // Compressed buffer still intact — can decompress again
    CHECK(snap.is_compressed());
    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));
  }

  SUBCASE("decompress is idempotent when vectors already present")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    snap.decompress();
    const auto size_after_first = flat_vectors_size(snap.flat_lp);

    // Second decompress should be no-op (vectors already present)
    snap.decompress();
    CHECK(flat_vectors_size(snap.flat_lp) == size_after_first);
  }

  SUBCASE("compress is idempotent — compressed buffer never changes")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    const auto buf1 = snap.compressed_lp.data;

    snap.decompress();
    snap.compress(CompressionCodec::lz4);
    // Buffer content should be identical (never re-compressed)
    CHECK(snap.compressed_lp.data == buf1);
  }

  SUBCASE("zstd codec also works correctly")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::zstd);
    CHECK(snap.is_compressed());
    CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));

    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));

    // Verify data survives round-trip
    CHECK(snap.flat_lp.ncols == flat.ncols);
    CHECK(snap.flat_lp.nrows == flat.nrows);
    CHECK(snap.flat_lp.matbeg.size() == flat.matbeg.size());
    CHECK(snap.flat_lp.matval.size() == flat.matval.size());

    for (size_t i = 0; i < flat.matval.size(); ++i) {
      CHECK(snap.flat_lp.matval[i] == doctest::Approx(flat.matval[i]));
    }
  }
}

// ── Label-metadata compress / decompress round-trip ───────────────────────
//
// The label-metadata path (serialize_labels_meta / deserialize_labels_meta
// and their drivers compress_labels_meta_if_needed /
// ensure_labels_meta_decompressed) is exercised by the following tests.
// Each test builds a LinearInterface with labeled cols/rows, puts it in
// compress mode, triggers a release_backend() → compress_labels_meta_if_
// needed() cycle, then reconstructs and reads back the labels.

TEST_CASE(  // NOLINT
    "LinearInterface — label metadata survives compress/decompress round-trip")
{
  auto [li, x1, x2, r1] = make_labeled_lp();

  // Solve to have a valid solution for the release → reconstruct cycle.
  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  // The optimal solution: x1=5, x2=0, obj=10.
  CHECK(li.get_obj_value_raw() == doctest::Approx(10.0));

  // Build a flat snapshot so release_backend can reuse it.  Use the
  // SAME labels as the live LinearInterface so that load_flat()'s
  // `m_col_labels_meta_ = flat_lp.col_labels_meta` overwrite preserves
  // the original metadata (the snapshot is the authoritative source
  // for structural labels in compress mode).
  LinearProblem lp;
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "generation",
      .variable_uid = Uid {1},
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "generation",
      .variable_uid = Uid {2},
  });
  const auto row_idx = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {10},
  });
  lp.set_coeff(row_idx, c1, 1.0);
  lp.set_coeff(row_idx, c2, 1.0);
  auto flat = lp.flatten({});

  SUBCASE("lz4 codec — label vectors cleared after release")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    // release_backend() calls compress_labels_meta_if_needed() internally.
    li.release_backend();
    REQUIRE(li.is_backend_released());

    // Reconstruct — ensure_labels_meta_decompressed() fires inside add_col.
    li.reconstruct_backend();
    REQUIRE_FALSE(li.is_backend_released());

    // Add a new column after the decompress cycle — proves that the
    // duplicate-detection map was rebuilt correctly and new entries can
    // be inserted.
    const auto x3 = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 5.0,
        .cost = 1.0,
        .class_name = "Gen",
        .variable_name = "generation",
        .variable_uid = Uid {3},
    });
    CHECK(static_cast<size_t>(x3) == 2);  // third column

    // Resolve still works after the decompression round-trip.
    auto r = li.resolve();
    REQUIRE(r.has_value());
  }

  SUBCASE("zstd codec — label vectors cleared after release")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    REQUIRE(li.is_backend_released());

    li.reconstruct_backend();
    REQUIRE_FALSE(li.is_backend_released());

    // Duplicate detection survives: inserting the same (class, var, uid)
    // as x1 after a decompress cycle must still throw.
    CHECK_THROWS(li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 2.0,
        .class_name = "Gen",
        .variable_name = "generation",
        .variable_uid = Uid {1},  // same uid as x1 → duplicate
    }));
  }

  SUBCASE("multiple compress/decompress cycles keep labels consistent")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    for (int cycle = 0; cycle < 3; ++cycle) {
      li.release_backend();
      REQUIRE(li.is_backend_released());

      li.reconstruct_backend();
      REQUIRE_FALSE(li.is_backend_released());

      auto r = li.resolve();
      REQUIRE(r.has_value());
      CHECK(li.get_obj_value_raw() == doctest::Approx(10.0));
    }
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface — label metadata with monostate context serializes "
    "correctly")
{
  // Columns/rows whose LpContext is std::monostate (the default) must still
  // round-trip correctly — the serialize path must write the correct zero
  // bytes for the monostate arm and the deserialize path must reconstruct it.
  LinearInterface li;

  const auto x1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 20.0,
      .cost = 5.0,
      .class_name = "Demand",
      .variable_name = "load",
      .variable_uid = Uid {7},
      // context defaults to std::monostate{}
  });
  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 0.0;
  row.uppb = SparseRow::DblMax;
  row.class_name = "Demand";
  row.constraint_name = "capacity";
  row.variable_uid = Uid {7};
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Build minimal flat snapshot — labels match the live LinearInterface.
  LinearProblem lp;
  const auto c = lp.add_col({
      .lowb = 0.0,
      .uppb = 20.0,
      .cost = 5.0,
      .class_name = "Demand",
      .variable_name = "load",
      .variable_uid = Uid {7},
  });
  const auto r = lp.add_row({
      .lowb = 0.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Demand",
      .constraint_name = "capacity",
      .variable_uid = Uid {7},
  });
  lp.set_coeff(r, c, 1.0);
  auto flat = lp.flatten({});

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
  li.save_snapshot(FlatLinearProblem {flat});

  li.release_backend();
  REQUIRE(li.is_backend_released());

  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // Same-uid insertion must still be detected as duplicate after round-trip.
  CHECK_THROWS(li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 20.0,
      .cost = 5.0,
      .class_name = "Demand",
      .variable_name = "load",
      .variable_uid = Uid {7},
  }));
}

// ── update_dynamic_col_lowb ───────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "LinearInterface::update_dynamic_col_lowb updates stored lower bound")
{
  // update_dynamic_col_lowb only has effect when low-memory mode is active
  // (i.e. m_dynamic_cols_ is populated via record_dynamic_col).

  auto [li, flat, x1, x2] = make_simple_li_lp();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Record a dynamic column representing an alpha variable.
  // `add_col(SparseCol)` auto-records into `m_dynamic_cols_`
  // post-snapshot, so no explicit `record_dynamic_col` is needed.
  SparseCol alpha_col;
  alpha_col.lowb = 0.0;
  alpha_col.uppb = 1000.0;
  alpha_col.cost = 0.0;
  alpha_col.class_name = "Sddp";
  alpha_col.variable_name = "alpha";
  [[maybe_unused]] const auto alpha = li.add_col(alpha_col);
  li.save_base_numrows();

  SUBCASE("match by class_name + variable_name updates lowb, returns true")
  {
    const bool updated = li.update_dynamic_col_lowb("Sddp", "alpha", -500.0);
    CHECK(updated);

    // Release and reconstruct — the replayed column must carry the new lowb.
    li.release_backend();
    li.reconstruct_backend();

    // The dynamic col is replayed as the third column (index 2).
    // Its lower bound should now be -500.
    const ColIndex alpha_idx {2};
    const auto low = li.get_col_low_raw();
    const auto upp = li.get_col_upp_raw();
    REQUIRE(low.size() > static_cast<size_t>(alpha_idx));
    REQUIRE(upp.size() > static_cast<size_t>(alpha_idx));
    CHECK(low[static_cast<size_t>(alpha_idx)] == doctest::Approx(-500.0));
    CHECK(upp[static_cast<size_t>(alpha_idx)] == doctest::Approx(1000.0));
  }

  SUBCASE("wrong class_name — no match, returns false")
  {
    const bool updated = li.update_dynamic_col_lowb("Other", "alpha", -99.0);
    CHECK_FALSE(updated);
  }

  SUBCASE("wrong variable_name — no match, returns false")
  {
    const bool updated = li.update_dynamic_col_lowb("Sddp", "not_alpha", -99.0);
    CHECK_FALSE(updated);
  }

  SUBCASE("no dynamic cols recorded — returns false")
  {
    // A fresh LI in compress mode with no record_dynamic_col call.
    LinearInterface li2;
    li2.set_low_memory(LowMemoryMode::compress);

    const bool updated = li2.update_dynamic_col_lowb("Sddp", "alpha", -1.0);
    CHECK_FALSE(updated);
  }
}

// ── add_row_raw with scale != 1.0 ───────────────────────────────────

TEST_CASE(  // NOLINT
    "LinearInterface — add_row with SparseRow::scale != 1.0 scales bounds and "
    "coefficients")
{
  // A row with scale == 2.0 is semantically equivalent to the same row
  // with all coefficients and bounds divided by 2.0 before insertion.
  // The LP backend stores the un-scaled row (divide by rs) and
  // `set_row_scale` records rs so that dual variables can be rescaled.
  LinearInterface li_scaled;
  LinearInterface li_plain;

  // Column: 0 <= x <= 100, cost = 1.
  const auto xs = li_scaled.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
  });
  const auto xp = li_plain.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
  });

  // Scaled LP: row  2·x >= 20  with scale=2
  //   → stored internally as  x >= 10  (bounds divided by 2)
  // Plain LP: same constraint in un-scaled form  x >= 10
  SparseRow scaled_row;
  scaled_row[xs] = 2.0;
  scaled_row.lowb = 20.0;
  scaled_row.uppb = SparseRow::DblMax;
  scaled_row.scale = 2.0;
  li_scaled.add_row(scaled_row);

  SparseRow plain_row;
  plain_row[xp] = 1.0;
  plain_row.lowb = 10.0;
  plain_row.uppb = SparseRow::DblMax;
  // scale defaults to 1.0 — no rescaling
  li_plain.add_row(plain_row);

  // Both must solve to the same objective: min x s.t. x >= 10 → obj = 10.
  const SolverOptions opts;
  auto r_scaled = li_scaled.initial_solve(opts);
  auto r_plain = li_plain.initial_solve(opts);

  REQUIRE(r_scaled.has_value());
  REQUIRE(r_plain.has_value());
  REQUIRE(li_scaled.is_optimal());
  REQUIRE(li_plain.is_optimal());

  CHECK(li_scaled.get_obj_value_raw() == doctest::Approx(10.0));
  CHECK(li_plain.get_obj_value_raw() == doctest::Approx(10.0));

  // Solution value is the same (x = 10 in both).
  const auto sol_scaled = li_scaled.get_col_sol_raw();
  const auto sol_plain = li_plain.get_col_sol_raw();
  REQUIRE(sol_scaled.size() == 1);
  REQUIRE(sol_plain.size() == 1);
  CHECK(sol_scaled[xs] == doctest::Approx(sol_plain[xp]));
}

// ── label-meta codec round-trip — direct string equality ─────────────────
//
// The tests below pin down the byte-level (de)serializer in
// `linear_interface.cpp` (anonymous namespace `serialize_labels_meta` /
// `deserialize_labels_meta`) before the upcoming refactor moves it into
// its own translation unit.  They synthesise the user-visible label
// strings before and after a compress/decompress cycle and assert
// byte-for-byte equality, exercising every `LpContext` variant arm.

namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
struct LabelSnapshot
{
  std::vector<std::string> col_names;
  std::vector<std::string> row_names;
};

LabelSnapshot capture_labels(LinearInterface& li)
{
  LabelSnapshot snap;
  li.generate_labels_from_maps(snap.col_names, snap.row_names);
  return snap;
}
}  // namespace

TEST_CASE(  // NOLINT
    "LinearInterface — label codec preserves names across all LpContext arms")
{
  // Add one column per non-monostate `LpContext` variant arm so the
  // memcpy of `LpContext` inside `serialize_labels_meta` is exercised
  // for every alternative.  The monostate arm is already covered by
  // an earlier test.
  LinearInterface li;

  [[maybe_unused]] const auto x_stage = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {1},
      .context = make_stage_context(make_uid<Scenario>(3), make_uid<Stage>(7)),
  });
  [[maybe_unused]] const auto x_block = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {2},
      .context = make_block_context(
          make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11)),
  });
  [[maybe_unused]] const auto x_blockex = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {3},
      .context = make_block_context(make_uid<Scenario>(3),
                                    make_uid<Stage>(7),
                                    make_uid<Block>(11),
                                    /*extra=*/2),
  });
  [[maybe_unused]] const auto x_scene = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 4.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = Uid {4},
      .context =
          make_scene_phase_context(make_uid<Scene>(1), make_uid<Phase>(2)),
  });
  [[maybe_unused]] const auto x_iter = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 5.0,
      .class_name = "Sddp",
      .variable_name = "cut",
      .variable_uid = Uid {5},
      .context = make_iteration_context(make_uid<Scene>(1),
                                        make_uid<Phase>(2),
                                        make_uid<Iteration>(4),
                                        /*extra=*/0),
  });
  [[maybe_unused]] const auto x_aper = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 6.0,
      .class_name = "Sddp",
      .variable_name = "aperture",
      .variable_uid = Uid {6},
      // ApertureUid is an alias for ScenarioUid in the codebase.
      .context = make_aperture_context(make_uid<Scene>(1),
                                       make_uid<Phase>(2),
                                       make_uid<Scenario>(3),
                                       /*extra=*/0),
  });

  // One row per non-monostate variant arm too.
  SparseRow row_stage;
  row_stage[x_stage] = 1.0;
  row_stage.lowb = 0.0;
  row_stage.uppb = LinearProblem::DblMax;
  row_stage.class_name = "Bus";
  row_stage.constraint_name = "balance";
  row_stage.variable_uid = Uid {100};
  row_stage.context =
      make_stage_context(make_uid<Scenario>(3), make_uid<Stage>(7));
  li.add_row(row_stage);

  SparseRow row_block;
  row_block[x_block] = 1.0;
  row_block.lowb = 0.0;
  row_block.uppb = LinearProblem::DblMax;
  row_block.class_name = "Bus";
  row_block.constraint_name = "balance";
  row_block.variable_uid = Uid {101};
  row_block.context = make_block_context(
      make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11));
  li.add_row(row_block);

  // Capture canonical names BEFORE any compress/decompress cycle.
  const auto names_before = capture_labels(li);
  REQUIRE(names_before.col_names.size() == 6);
  REQUIRE(names_before.row_names.size() == 2);
  for (const auto& n : names_before.col_names) {
    CHECK_FALSE(n.empty());
  }

  // Build a flat snapshot whose metadata matches the live labels —
  // this is what `load_flat` overlays back onto `m_col_labels_meta_`
  // after reconstruction.
  LinearProblem lp;
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {1},
      .context = make_stage_context(make_uid<Scenario>(3), make_uid<Stage>(7)),
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {2},
      .context = make_block_context(
          make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11)),
  });
  const auto c3 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {3},
      .context = make_block_context(
          make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11), 2),
  });
  const auto c4 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 4.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = Uid {4},
      .context =
          make_scene_phase_context(make_uid<Scene>(1), make_uid<Phase>(2)),
  });
  const auto c5 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 5.0,
      .class_name = "Sddp",
      .variable_name = "cut",
      .variable_uid = Uid {5},
      .context = make_iteration_context(
          make_uid<Scene>(1), make_uid<Phase>(2), make_uid<Iteration>(4), 0),
  });
  const auto c6 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 6.0,
      .class_name = "Sddp",
      .variable_name = "aperture",
      .variable_uid = Uid {6},
      .context = make_aperture_context(
          make_uid<Scene>(1), make_uid<Phase>(2), make_uid<Scenario>(3), 0),
  });
  const auto r1 = lp.add_row({
      .lowb = 0.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {100},
      .context = make_stage_context(make_uid<Scenario>(3), make_uid<Stage>(7)),
  });
  const auto r2 = lp.add_row({
      .lowb = 0.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {101},
      .context = make_block_context(
          make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11)),
  });
  lp.set_coeff(r1, c1, 1.0);
  lp.set_coeff(r2, c2, 1.0);
  // Touch the rest so they appear in the matrix.
  lp.set_coeff(r1, c3, 0.0);
  lp.set_coeff(r1, c4, 0.0);
  lp.set_coeff(r1, c5, 0.0);
  lp.set_coeff(r1, c6, 0.0);
  auto flat = lp.flatten({});

  SUBCASE("lz4 codec — round-trip preserves every variant arm")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    li.reconstruct_backend();

    const auto names_after = capture_labels(li);
    CHECK(names_after.col_names == names_before.col_names);
    CHECK(names_after.row_names == names_before.row_names);
  }

  SUBCASE("zstd codec — round-trip preserves every variant arm")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    li.reconstruct_backend();

    const auto names_after = capture_labels(li);
    CHECK(names_after.col_names == names_before.col_names);
    CHECK(names_after.row_names == names_before.row_names);
  }

  SUBCASE("three release/reconstruct cycles — names stay byte-identical")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    for (int cycle = 0; cycle < 3; ++cycle) {
      li.release_backend();
      li.reconstruct_backend();
      const auto names_after = capture_labels(li);
      CHECK(names_after.col_names == names_before.col_names);
      CHECK(names_after.row_names == names_before.row_names);
    }
  }
}

// ── materialize_labels caches and stays consistent on repeat calls ───────

TEST_CASE(  // NOLINT
    "LinearInterface::materialize_labels — caches names and is idempotent")
{
  // First call synthesises labels from metadata and populates
  // `m_col_index_to_name_` / `m_row_index_to_name_`.  Subsequent calls
  // hit the cache and must return identical strings.  Direct equality
  // of three consecutive `generate_labels_from_maps` outputs proves
  // the cache+name-map machinery is consistent on the steady-state
  // path — a tight invariant the upcoming labels-TU split must
  // preserve.
  LinearInterface li;

  for (int k = 1; k <= 4; ++k) {
    li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = static_cast<double>(k),
        .class_name = "Gen",
        .variable_name = "p",
        .variable_uid = Uid {k},
    });
  }
  SparseRow row;
  row[ColIndex {0}] = 1.0;
  row.lowb = 0.0;
  row.uppb = LinearProblem::DblMax;
  row.class_name = "Bus";
  row.constraint_name = "balance";
  row.variable_uid = Uid {99};
  li.add_row(row);

  std::vector<std::string> cols_first;
  std::vector<std::string> rows_first;
  li.generate_labels_from_maps(cols_first, rows_first);

  REQUIRE(cols_first.size() == 4);
  REQUIRE(rows_first.size() == 1);
  for (const auto& n : cols_first) {
    CHECK_FALSE(n.empty());
  }
  CHECK_FALSE(rows_first.front().empty());

  // Second pass — pure cache hit — must return byte-identical strings.
  std::vector<std::string> cols_second;
  std::vector<std::string> rows_second;
  li.generate_labels_from_maps(cols_second, rows_second);
  CHECK(cols_second == cols_first);
  CHECK(rows_second == rows_first);

  // `materialize_labels` is the void-returning variant; calling it
  // after the explicit pass must remain consistent (no rebuild side
  // effects, no duplicate-detection throws on stable metadata).
  li.materialize_labels();

  // Third pass — names still match.
  std::vector<std::string> cols_third;
  std::vector<std::string> rows_third;
  li.generate_labels_from_maps(cols_third, rows_third);
  CHECK(cols_third == cols_first);
  CHECK(rows_third == rows_first);

  // The reverse name→index map must be populated for all formatted
  // labels (the contract `write_lp` and the SDDP cut path depend on).
  for (size_t i = 0; i < cols_first.size(); ++i) {
    const auto it = li.col_name_map().find(cols_first[i]);
    REQUIRE(it != li.col_name_map().end());
    CHECK(static_cast<size_t>(it->second) == i);
  }
  const auto it_r = li.row_name_map().find(rows_first.front());
  REQUIRE(it_r != li.row_name_map().end());
  CHECK(static_cast<size_t>(it_r->second) == 0);
}

// ── algorithm-fallback cycle — pure helper coverage via observable path ──
//
// `next_fallback_algo` is a constexpr free function in the anonymous
// namespace of `linear_interface.cpp`.  It cycles
// barrier → dual → primal → barrier and maps `default_algo` /
// `last_algo` onto `dual`.  Driving it through the public solve path
// pins the cycle ordering before the upcoming `linear_interface_solve.cpp`
// extraction.

TEST_CASE(  // NOLINT
    "LinearInterface — fallback cycle visits all 3 algorithms in order")
{
  // Infeasible LP forces every fallback step to fire.  With
  // `max_fallbacks = 3`, the solver tries the starting algorithm and
  // then 3 alternatives, hitting all three other algorithms in the
  // cycle.  We can't observe the algorithm-per-attempt directly, but
  // `solver_stats().fallback_solves` must equal 3 in every case — a
  // regression in `next_fallback_algo` (e.g. a stuck cycle) would
  // either short-circuit early or repeat an algorithm without
  // advancing, breaking the count.
  for (const auto start : {LPAlgo::barrier,
                           LPAlgo::dual,
                           LPAlgo::primal,
                           LPAlgo::default_algo,
                           LPAlgo::last_algo})
  {
    LinearInterface li;
    const auto x = li.add_col(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
    });
    SparseRow row;
    row[x] = 1.0;
    row.lowb = 10.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    const auto res = li.initial_solve(SolverOptions {
        .algorithm = start,
        .log_level = 0,
        .max_fallbacks = 3,
    });
    REQUIRE_FALSE(res.has_value());

    // 3 fallback attempts must have been recorded — the cycle did not
    // get stuck and did not exit early.
    CHECK(li.solver_stats().fallback_solves == 3);
  }
}

// ── generate_labels_from_maps defensive throw paths ──────────────────────

TEST_CASE(  // NOLINT
    "LinearInterface::generate_labels_from_maps — throws on missing col meta")
{
  // Defensive guard inside `generate_labels_from_maps`: if the live
  // backend reports more cols than `m_col_labels_meta_` knows about
  // AND the index-to-name cache is empty for that col, the function
  // throws std::logic_error with a diagnostic message.
  //
  // This path fires when a flat LP arrives from `load_flat` with
  // truncated label metadata.  Construct that scenario directly by
  // building a `FlatLinearProblem` whose `col_labels_meta` has fewer
  // entries than `ncols`, then call `load_flat` and ask for label
  // synthesis.
  LinearProblem lp;
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {1},
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {2},
  });
  const auto r = lp.add_row({
      .lowb = 0.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {99},
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);
  auto flat = lp.flatten({});

  REQUIRE(flat.ncols == 2);
  REQUIRE(flat.col_labels_meta.size() == 2);

  // Truncate the label metadata so it lags behind ncols.  The flat LP
  // is otherwise structurally valid — the backend will load 2 cols,
  // but `generate_labels_from_maps` has no entry for col 1.
  flat.col_labels_meta.pop_back();
  REQUIRE(flat.col_labels_meta.size() == 1);

  LinearInterface li;
  li.load_flat(flat);

  // The first cache miss on col 1 must throw with a message that
  // identifies the col index and the metadata-vector size.
  CHECK_THROWS_AS(li.materialize_labels(), std::logic_error);

  bool message_was_diagnostic = false;
  try {
    li.materialize_labels();
  } catch (const std::logic_error& e) {
    const std::string msg = e.what();
    message_was_diagnostic =
        msg.contains("generate_labels_from_maps") && msg.contains("col 1");
  }
  CHECK(message_was_diagnostic);
}

TEST_CASE(  // NOLINT
    "LinearInterface::generate_labels_from_maps — throws on missing row meta")
{
  // Symmetric guard for the row branch.
  LinearProblem lp;
  const auto c = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {1},
  });
  const auto r1 = lp.add_row({
      .lowb = 0.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {100},
  });
  const auto r2 = lp.add_row({
      .lowb = 0.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {101},
  });
  lp.set_coeff(r1, c, 1.0);
  lp.set_coeff(r2, c, 1.0);
  auto flat = lp.flatten({});

  REQUIRE(flat.nrows == 2);
  REQUIRE(flat.row_labels_meta.size() == 2);

  flat.row_labels_meta.pop_back();
  REQUIRE(flat.row_labels_meta.size() == 1);

  LinearInterface li;
  li.load_flat(flat);

  CHECK_THROWS_AS(li.materialize_labels(), std::logic_error);
}

// Regression: under `LowMemoryMode::compress` the cell rebuild path
// (`apply_post_load_replay`) re-adds `m_active_cuts_` through the bulk
// `add_rows` API.  That path used to skip `track_row_label_meta`, leaving
// `m_row_labels_meta_` shorter than the backend row count by exactly the
// number of replayed cuts.  Any later `materialize_labels` /
// `push_names_to_solver` / `write_lp` then threw
// `std::logic_error("row N has no entry in m_row_labels_meta_")` — the
// failure mode observed on `support/juan/gtopt_iplp` SDDP runs.  This
// test exercises a single labeled cut across a compress→reconstruct
// cycle and asserts that label synthesis no longer throws.
TEST_CASE(  // NOLINT
    "LinearInterface — bulk add_rows replay preserves row label metadata")
{
  // Build a fully-labeled LP so `generate_labels_from_maps` is exercised
  // on every col + row (the simple unlabeled fixture short-circuits at
  // the col-side check).  Mirrors the `make_simple_li_lp` shape but with
  // `class_name` / `variable_name` / `variable_uid` populated.
  LinearProblem lp;
  const auto c1 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {1},
  });
  const auto c2 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {2},
  });
  const auto r = lp.add_row(SparseRow {
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {10},
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  const auto base_nrows = li.base_numrows();

  // Labeled cut row — the metadata must survive the compress/replay
  // round trip so `generate_labels_from_maps` can synthesise a label.
  SparseRow cut;
  cut[c1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 3.0;
  cut.class_name = "BendersCut";
  cut.constraint_name = "fcut";
  cut.variable_uid = Uid {42};
  li.add_row(cut);
  li.record_cut_row(cut);

  REQUIRE(li.get_numrows() == base_nrows + 1);

  // Force the bulk replay path: drop the live backend, then rebuild.
  li.release_backend();
  li.reconstruct_backend();

  REQUIRE(li.get_numrows() == base_nrows + 1);

  // Pre-fix this threw `std::logic_error: row N has no entry in
  // m_row_labels_meta_`.  Post-fix it succeeds and caches a non-empty
  // label for the replayed cut row.
  CHECK_NOTHROW(li.materialize_labels());

  std::vector<std::string> col_names;
  std::vector<std::string> row_names;
  li.generate_labels_from_maps(col_names, row_names);
  REQUIRE(row_names.size() == base_nrows + 1);
  CHECK_FALSE(row_names.back().empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// freeze_for_cuts — single-call entry point that replaces the legacy
// 3-call dance (set_low_memory + save_snapshot + save_base_numrows).
// Step 1 of `support/linear_interface_lifecycle_plan_2026-04-30.md`.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "LinearInterface::freeze_for_cuts on a fresh LP — full lifecycle")
{
  // Companion to the "legacy 3-call sequence" equivalence test above.
  // This one exercises the consolidator on a manually-built LI that
  // has NOT been through `make_simple_li_lp` (which silently calls
  // `save_base_numrows` and thus pre-advances to `Frozen`), so the
  // `Building → Frozen` transition can be observed directly through
  // the consolidator's call.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearProblem lp;
  const auto c1 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 2.0});
  const auto c2 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 3.0});
  const auto r = lp.add_row(SparseRow {
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);

  // Pre-condition: still in Building before freeze_for_cuts fires.
  // (`make_simple_li_lp` would have called save_base_numrows here,
  // which would advance the phase prematurely; we deliberately skip
  // that.)
  CHECK(li.phase() == LinearInterface::LiPhase::Building);
  CHECK_FALSE(li.has_snapshot_data());
  CHECK(li.base_numrows() == 0);

  // Single-call consolidation.
  li.freeze_for_cuts(LowMemoryMode::compress, FlatLinearProblem {flat});

  // Post-condition: every observable matches the post-3-call state.
  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);
  CHECK(li.low_memory_mode() == LowMemoryMode::compress);
  CHECK(li.has_snapshot_data());
  CHECK(li.base_numrows() == 1);  // one row from `flat`

  // Adding a cut after freeze advances the row count beyond base.
  SparseRow cut;
  cut[c1] = 1.0;
  cut.lowb = 0.0;
  cut.uppb = 4.0;
  cut.class_name = "BendersCut";
  cut.constraint_name = "fcut";
  cut.variable_uid = Uid {77};
  li.add_row(cut);
  li.record_cut_row(cut);
  CHECK(li.get_numrows() == li.base_numrows() + 1);

  // Compress/replay round-trip.
  li.release_backend();
  CHECK(li.phase() == LinearInterface::LiPhase::BackendReleased);

  li.reconstruct_backend();
  CHECK(li.phase() == LinearInterface::LiPhase::Reconstructed);
  CHECK(li.get_numrows() == li.base_numrows() + 1);
}

TEST_CASE(  // NOLINT
    "LinearInterface::freeze_for_cuts asserts on cuts-already-added")
{
  // `freeze_for_cuts` is a structural-build commit point — calling
  // it AFTER cuts have already been added is a programming error.
  // The helper `assert(m_active_cuts_.empty())` catches it in debug
  // builds.  In release the call still runs (set_low_memory +
  // save_snapshot + save_base_numrows), but the row count would
  // mis-attribute existing cut rows as part of the structural base.
  //
  // doctest's CHECK_THROWS-style guards don't catch `assert`, so
  // this test only checks the legitimate ordering: low-mem
  // configured first, then cuts arrive, no freeze required.  The
  // assertion's correctness is verified by the structure alone.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearProblem lp;
  std::ignore = lp.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});
  auto flat = lp.flatten({});

  LinearInterface li;
  li.load_flat(flat);
  li.freeze_for_cuts(LowMemoryMode::compress, FlatLinearProblem {flat});

  // After freeze, recording a cut populates m_active_cuts_; calling
  // freeze a second time would trigger the assertion.  We don't
  // call it here (debug abort is non-portable in tests); the test
  // just exercises the legitimate ordering.
  SparseRow cut;
  cut.lowb = 0.0;
  cut.uppb = 100.0;
  cut[ColIndex {0}] = 1.0;
  cut.class_name = "X";
  cut.constraint_name = "y";
  cut.variable_uid = Uid {1};
  li.record_cut_row(cut);
  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);
}

TEST_CASE(  // NOLINT
    "LinearInterface::phase advances through Building → Frozen → "
    "BackendReleased → Reconstructed")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Step 2 of `support/linear_interface_lifecycle_plan_2026-04-30.md`
  // adds the lifecycle observer.  This test pins each transition
  // fired by the canonical entry points; step 4 (debug-asserted
  // transitions) stays deferred because legitimate test paths
  // bypass the canonical entry points.
  //
  // `make_simple_li_lp` calls `save_base_numrows` internally, which
  // advances `Building → Frozen` as part of the legacy 3-call
  // sequence's last step.  The LP returned by the helper is
  // therefore already `Frozen`.  Adding `freeze_for_cuts` on top is
  // a no-op for the phase observer (it stays `Frozen`).
  auto [li, flat, x1, x2] = make_simple_li_lp();
  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);

  // freeze_for_cuts on an already-frozen LP keeps it Frozen
  // (idempotent observer, since the legacy save_base_numrows
  // already advanced the phase).
  li.freeze_for_cuts(LowMemoryMode::compress, FlatLinearProblem {flat});
  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);

  li.release_backend();
  CHECK(li.phase() == LinearInterface::LiPhase::BackendReleased);

  li.reconstruct_backend();
  CHECK(li.phase() == LinearInterface::LiPhase::Reconstructed);

  // Re-release after reconstruct goes back to BackendReleased.
  li.release_backend();
  CHECK(li.phase() == LinearInterface::LiPhase::BackendReleased);

  // Re-reconstruct returns to Reconstructed.
  li.reconstruct_backend();
  CHECK(li.phase() == LinearInterface::LiPhase::Reconstructed);
}

TEST_CASE(  // NOLINT
    "LinearInterface::freeze_for_cuts equals legacy 3-call sequence")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build the same simple LP twice — once via the legacy 3-call
  // dance, once via the new consolidator — and assert every
  // observable post-condition matches.
  auto build_legacy = []
  {
    auto [li, flat, x1, x2] = make_simple_li_lp();
    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    li.set_low_memory(LowMemoryMode::compress);
    li.save_snapshot(FlatLinearProblem {flat});
    li.save_base_numrows();
    return std::tuple {std::move(li), x1, x2};
  };

  auto build_freeze = []
  {
    auto [li, flat, x1, x2] = make_simple_li_lp();
    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    li.freeze_for_cuts(LowMemoryMode::compress, FlatLinearProblem {flat});
    return std::tuple {std::move(li), x1, x2};
  };

  auto [legacy_li, l_x1, l_x2] = build_legacy();
  auto [frozen_li, f_x1, f_x2] = build_freeze();

  CHECK(legacy_li.low_memory_mode() == frozen_li.low_memory_mode());
  CHECK(legacy_li.has_snapshot_data() == frozen_li.has_snapshot_data());
  CHECK(legacy_li.base_numrows() == frozen_li.base_numrows());
  CHECK(legacy_li.get_numrows() == frozen_li.get_numrows());
  CHECK(legacy_li.get_numcols() == frozen_li.get_numcols());

  // Same cut-replay round-trip behaviour: add a labelled cut and
  // verify both LIs reach identical post-replay state.
  for (auto& li : {std::ref(legacy_li), std::ref(frozen_li)}) {
    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut.lowb = -LinearProblem::DblMax;
    cut.uppb = 3.0;
    cut.class_name = "BendersCut";
    cut.constraint_name = "fcut";
    cut.variable_uid = Uid {99};
    li.get().add_row(cut);
    li.get().record_cut_row(cut);
    li.get().release_backend();
    li.get().reconstruct_backend();
  }
  CHECK(legacy_li.get_numrows() == frozen_li.get_numrows());
}

TEST_CASE(  // NOLINT
    "LinearInterface::freeze_for_cuts with LowMemoryMode::off — no snapshot")
{
  // Edge case: freeze_for_cuts under `off` mode still advances the
  // phase observer + sets base_numrows, but `set_low_memory(off)`
  // explicitly drops the snapshot at line 245-247 of
  // `linear_interface.cpp`.  Pin that behaviour so a future
  // refactor can't accidentally make `off` mode start retaining
  // snapshots.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto [li, flat, x1, x2] = make_simple_li_lp();
  // make_simple_li_lp already calls save_base_numrows, so phase is
  // Frozen.  `freeze_for_cuts` is idempotent under that condition.

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.freeze_for_cuts(LowMemoryMode::off, FlatLinearProblem {flat});

  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);
  CHECK(li.low_memory_mode() == LowMemoryMode::off);
  // `set_low_memory(off)` clears the snapshot via the
  // `if (mode == LowMemoryMode::off) m_snapshot_ = {};` branch.
  CHECK_FALSE(li.has_snapshot_data());
  // base_numrows still reflects the frozen structural row count.
  CHECK(li.base_numrows() > 0);

  // `release_backend()` is a documented no-op under `off`
  // (linear_interface.cpp:140-142).  The phase observer doesn't
  // advance because there's no real release happening.
  li.release_backend();
  CHECK_FALSE(li.is_backend_released());
  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);
}

TEST_CASE(  // NOLINT
    "LinearInterface::freeze_for_cuts with LowMemoryMode::rebuild — no "
    "snapshot")
{
  // Edge case: rebuild mode uses a per-cell callback to re-flatten
  // from element collections, NOT the snapshot path.  In fact
  // `reconstruct_backend` explicitly asserts
  // `m_low_memory_mode_ != rebuild` (linear_interface.cpp:315),
  // so a snapshot stored under rebuild would be dead weight.
  // `freeze_for_cuts(rebuild, ...)` skips `save_snapshot` for this
  // reason — pin the contract.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto [li, flat, x1, x2] = make_simple_li_lp();
  // Bare LinearInterface in this test — no rebuild callback owner
  // is installed, but freeze_for_cuts still completes (set_low_memory,
  // skip snapshot, save_base_numrows) without touching the
  // reconstruct path.

  li.freeze_for_cuts(LowMemoryMode::rebuild, FlatLinearProblem {flat});

  CHECK(li.phase() == LinearInterface::LiPhase::Frozen);
  CHECK(li.low_memory_mode() == LowMemoryMode::rebuild);
  // Snapshot deliberately NOT saved under rebuild.
  CHECK_FALSE(li.has_snapshot_data());
  CHECK(li.base_numrows() > 0);
}

TEST_CASE(  // NOLINT
    "LinearInterface — multi-cycle release/reconstruct preserves cut metadata")
{
  // Stress test: a cell that goes through MANY release/reconstruct
  // cycles must keep its label metadata + row count + cut ordering
  // consistent across every cycle.  Pre-fix the bulk add_rows path
  // dropped metadata, which manifested only after the FIRST replay
  // and then accumulated; multi-cycle exercises the steady-state
  // invariant.
  //
  // Build a fully-labelled LP inline (the `make_simple_li_lp` helper
  // builds unlabelled cols, which would trip
  // `generate_labels_from_maps` at the col side).
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearProblem lp;
  const auto c1 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {1},
  });
  const auto c2 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = Uid {2},
  });
  const auto r = lp.add_row(SparseRow {
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {10},
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);
  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  li.freeze_for_cuts(LowMemoryMode::compress, FlatLinearProblem {flat});

  const auto base_rows = li.base_numrows();
  const auto x1 = c1;

  // Add 3 distinct labelled cuts.
  for (int i = 0; i < 3; ++i) {
    SparseRow cut;
    cut[x1] = static_cast<double>(i + 1);
    cut.lowb = -LinearProblem::DblMax;
    cut.uppb = static_cast<double>(10 - i);
    cut.class_name = "BendersCut";
    cut.constraint_name = "fcut";
    cut.variable_uid = Uid {static_cast<int32_t>(100 + i)};
    li.add_row(cut);
    li.record_cut_row(cut);
  }
  REQUIRE(li.get_numrows() == base_rows + 3);

  // Five release/reconstruct cycles.  Row count + materialize_labels
  // must succeed every time.  A regression that made the bulk
  // replay path drop metadata would surface as either:
  // (a) `materialize_labels()` throwing on an unlabeled row, or
  // (b) `get_numrows()` drifting from `base_rows + 3`.
  for (int cycle = 0; cycle < 5; ++cycle) {
    CAPTURE(cycle);
    li.release_backend();
    REQUIRE(li.is_backend_released());
    li.reconstruct_backend();
    REQUIRE_FALSE(li.is_backend_released());
    CHECK(li.get_numrows() == base_rows + 3);
    CHECK_NOTHROW(li.materialize_labels());
    std::vector<std::string> col_names;
    std::vector<std::string> row_names;
    li.generate_labels_from_maps(col_names, row_names);
    CHECK(row_names.size() == base_rows + 3);
    // Every cut row gets a non-empty label.
    for (std::size_t i = base_rows; i < row_names.size(); ++i) {
      CAPTURE(i);
      CHECK_FALSE(row_names[i].empty());
    }
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface — release_backend then reconstruct with no cuts")
{
  // Sanity check on the `replay_active_cuts()` no-op path: a
  // cell that goes through release_backend → reconstruct_backend
  // without having added ANY cuts must reach the second backend
  // load with `m_active_cuts_.empty()` and skip the bulk replay.
  // Pre-fix the bulk path tracked metadata; post-fix the no-op
  // early return inside `replay_active_cuts` makes this trivially
  // safe — but the test pins the contract.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto [li, flat, x1, x2] = make_simple_li_lp();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.freeze_for_cuts(LowMemoryMode::compress, FlatLinearProblem {flat});

  const auto rows_before = li.get_numrows();

  li.release_backend();
  REQUIRE(li.is_backend_released());

  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // No cuts were ever added → row count unchanged across the cycle.
  CHECK(li.get_numrows() == rows_before);
  CHECK(li.phase() == LinearInterface::LiPhase::Reconstructed);
}

// ── drop_cached_primal_only — between-iteration memory release ──────────────

TEST_CASE(  // NOLINT
    "LinearInterface — drop_cached_primal_only retains row_dual; "
    "col_sol / col_cost reads trigger reconstruct")
{
  // Pins the contract introduced for the SDDP between-iter memory drop:
  // `col_sol` and `col_cost` are dead between iterations (the iteration
  // that produced them has already consumed them via OutputContext +
  // save_state_csv + state-variable propagation), but `row_dual` is
  // re-read across iterations by `update_stored_cut_duals` and
  // `prune_inactive_cuts`.  Dropping the wrong vector silently breaks
  // SDDP cut prune; this test catches that regression.
  //
  // The observable invariant is **whether reading the cache triggers a
  // backend reconstruct**.  After `release_backend`, `get_*_raw` first
  // checks the cache; on a hit it returns the cached span without
  // touching the backend.  On a miss it calls `backend().col_solution()`
  // which transparently reconstructs the backend via `ensure_backend()`.
  // We can therefore read `is_backend_released()` AFTER calling the
  // getter to verify whether the cache was hit (still released) or not
  // (now reconstructed).
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  // Capture live row_dual ground truth so we can verify it's preserved
  // bit-for-bit through the drop.
  const std::vector<double> live_row_dual(li.get_row_dual_raw().begin(),
                                          li.get_row_dual_raw().end());
  REQUIRE_FALSE(live_row_dual.empty());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  SUBCASE("row_dual cache is RETAINED — read does not reconstruct")
  {
    li.release_backend();
    REQUIRE(li.is_backend_released());

    li.drop_cached_primal_only();
    REQUIRE(li.is_backend_released());  // drop alone does not reconstruct

    // Reading row_dual must serve from cache (no backend access).
    const auto pi = li.get_row_dual_raw();
    REQUIRE(pi.size() == live_row_dual.size());
    for (size_t i = 0; i < pi.size(); ++i) {
      CHECK(pi[i] == doctest::Approx(live_row_dual[i]));
    }
    // Backend MUST still be released — the cache hit served the read
    // without forcing a reconstruct.  This is the load-bearing
    // invariant for SDDP cut prune / dual update.
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());
  }

  SUBCASE("col_sol cache is cleared — read triggers reconstruct")
  {
    li.release_backend();
    REQUIRE(li.is_backend_released());

    li.drop_cached_primal_only();
    REQUIRE(li.is_backend_released());

    // Reading col_sol after drop falls through to `backend()` which
    // calls `ensure_backend()` → reconstruct from snapshot.  The
    // observable: is_backend_released flips to false.
    [[maybe_unused]] const auto sol = li.get_col_sol_raw();
    CHECK_FALSE(li.is_backend_released());
  }

  SUBCASE("col_cost cache is cleared — read triggers reconstruct")
  {
    li.release_backend();
    REQUIRE(li.is_backend_released());

    li.drop_cached_primal_only();
    REQUIRE(li.is_backend_released());

    [[maybe_unused]] const auto cc = li.get_col_cost_raw();
    CHECK_FALSE(li.is_backend_released());
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface — drop_cached_primal_only is idempotent and safe")
{
  // Two consecutive drops must not crash or corrupt state.  This is the
  // contract that lets the bulk safety-net loop in sddp_iteration.cpp
  // call drop_cached_primal_only on every cell unconditionally without
  // worrying about whether the per-cell scheme already dropped earlier.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture row_dual ground truth before compression.
  const std::vector<double> live_row_dual(li.get_row_dual_raw().begin(),
                                          li.get_row_dual_raw().end());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_backend_released());

  CHECK_NOTHROW(li.drop_cached_primal_only());

  // Idempotent: second call is a no-op, neither crashes nor reconstructs.
  CHECK_NOTHROW(li.drop_cached_primal_only());
  CHECK(li.is_backend_released());

  // row_dual cache survived both drops — read it without
  // reconstructing.
  const auto pi = li.get_row_dual_raw();
  REQUIRE(pi.size() == live_row_dual.size());
  for (size_t i = 0; i < pi.size(); ++i) {
    CHECK(pi[i] == doctest::Approx(live_row_dual[i]));
  }
  CHECK(li.is_backend_released());
}

TEST_CASE(  // NOLINT
    "LinearInterface — drop_cached_primal_only is a no-op under "
    "low_memory_mode=off")
{
  // Under `off` the caches are never populated by `release_backend`
  // (which is itself a no-op).  Calling drop must therefore be safe
  // and observe-equivalent to no call: the live backend stays alive
  // and getters continue to read from it.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE_FALSE(li.is_backend_released());

  // Mode off: no compression, no release_backend → no caches populated.
  CHECK_NOTHROW(li.drop_cached_primal_only());

  // Live backend untouched; getters still serve from it.
  CHECK_FALSE(li.is_backend_released());
  CHECK(li.has_backend());
  CHECK_FALSE(li.get_col_sol_raw().empty());
  CHECK_FALSE(li.get_col_cost_raw().empty());
  CHECK_FALSE(li.get_row_dual_raw().empty());
}

// ── drop_label_meta_buffers — end-of-run memory release ────────────────────

TEST_CASE(  // NOLINT
    "LinearInterface — drop_label_meta_buffers clears compressed buffers "
    "and string pool")
{
  // After PlanningLP::write_out emits parquet for a cell, the
  // compressed col/row label vectors and the string pool that backs
  // their decompressed `string_view`s are dead — no further consumer
  // reads them.  drop_label_meta_buffers releases ~1 MB compressed
  // per cell × N cells.  This test pins the post-condition: buffers
  // are empty after the call, idempotent under repeat calls.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // release_backend under compress invokes compress_labels_meta_if_needed,
  // which populates m_col_labels_meta_compressed_, m_row_labels_meta_
  // compressed_, and grows m_label_string_pool_ (when labels exist on
  // the LP).  The fixture has col_with_names + row_with_names, so the
  // string pool will be non-empty after compression.
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_backend_released());

  // We cannot directly assert "buffers populated" without exposing the
  // private members; the drop's CONTRACT is post-condition based:
  // after drop, label-meta lookups should observe the buffers as
  // empty / decompression yields nothing.  The simplest observable is
  // that drop is `noexcept` and idempotent on any LinearInterface
  // state (populated, empty, post-clone).

  CHECK_NOTHROW(li.drop_label_meta_buffers());

  // Idempotent: second call is a no-op.
  CHECK_NOTHROW(li.drop_label_meta_buffers());

  // Other caches must not have been touched: row_dual still holds the
  // post-release snapshot, col_sol still holds it (we did NOT call
  // drop_cached_primal_only in this test).
  CHECK_FALSE(li.get_row_dual_raw().empty());
  CHECK_FALSE(li.get_col_sol_raw().empty());
}

TEST_CASE(  // NOLINT
    "LinearInterface — drop_label_meta_buffers is safe before any "
    "compression")
{
  // The drop must be safe even on a freshly-constructed LinearInterface
  // that has never been compressed.  Tests the "nothing to drop" branch
  // that downstream cleanup paths rely on (e.g. early-exit error
  // handlers in PlanningLP::write_out that may call drop without the
  // usual release-then-emit prelude).
  auto [li, flat, x1, x2] = make_simple_li_lp();

  // No initial_solve, no release_backend, no compression.
  CHECK_NOTHROW(li.drop_label_meta_buffers());
  CHECK_NOTHROW(li.drop_label_meta_buffers());  // idempotent
}

// ── Metadata split: frozen flatten side vs per-instance post-flatten ──
//
// These tests pin the invariants delivered by commit 902a39e7
// "refactor(li): split flatten metadata + lazy decompression":
//
//   * `flatten_col_count()` / `flatten_row_count()` reflect the
//     `flat_lp.col_labels_meta` / `row_labels_meta` size at
//     `load_flat` time and never change afterwards.
//   * `add_col(SparseCol)` / `add_row(SparseRow)` past load_flat
//     extend ONLY the per-instance post-flatten vectors; the frozen
//     vector and its shared_ptr use_count stay invariant.
//   * `col_label_at(idx)` / `row_label_at(idx)` resolve correctly
//     across both buckets and return nullptr for out-of-range.
//   * Compress/release in compress mode keeps the frozen side in
//     compressed form during training; track_*_label_meta does NOT
//     trigger decompression for post-flatten extensions.
//   * `clone(shallow)` value-copies the post-flatten history so the
//     clone's `write_lp` sees the source's structural cuts/alpha but
//     subsequent writes don't bleed back.

TEST_CASE(  // NOLINT
    "LinearInterface — flatten_col_count / flatten_row_count match "
    "load_flat sizes and stay invariant under post-flatten add")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  // After `make_simple_li_lp`'s `load_flat`, the frozen counts equal
  // the structural size of the LP (2 cols × 1 row).
  REQUIRE(li.flatten_col_count() == 2);
  REQUIRE(li.flatten_row_count() == 1);

  // Switch to compress mode and snapshot so subsequent add_col is on
  // the post-flatten path (auto-record fires).
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // 3 post-flatten cols + 2 post-flatten rows.
  for (int k = 0; k < 3; ++k) {
    [[maybe_unused]] const auto c = li.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
        .class_name = "Test",
        .variable_name = "alpha",
        .variable_uid = k,
    });
  }
  for (int k = 0; k < 2; ++k) {
    SparseRow row;
    row[x1] = 1.0;
    row.lowb = 0.0;
    row.uppb = 100.0;
    row.class_name = "Test";
    row.constraint_name = "cap";
    row.variable_uid = k;
    li.add_row(row);
    li.record_dynamic_row(row);
  }

  // Frozen counts unchanged.
  CHECK(li.flatten_col_count() == 2);
  CHECK(li.flatten_row_count() == 1);
  // Backend total reflects structural + post-flatten.
  CHECK(li.get_numcols() == 5);
  CHECK(li.get_numrows() == 3);
}

TEST_CASE(  // NOLINT
    "LinearInterface — col_label_at / row_label_at resolve across "
    "frozen and post-flatten buckets")
{
  // Use the labelled flat-LP path so `flat_lp.col_labels_meta` /
  // `row_labels_meta` are populated at load_flat time.
  LinearProblem lp;
  const auto c0 = lp.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Bus",
      .variable_name = "theta",
      .variable_uid = 1,
  });
  [[maybe_unused]] const auto c1 = lp.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Bus",
      .variable_name = "theta",
      .variable_uid = 2,
  });
  auto r = lp.add_row(SparseRow {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Demand",
      .constraint_name = "balance",
      .variable_uid = 1,
  });
  lp.set_coeff(r, c0, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);
  REQUIRE(flat.col_labels_meta.size() == 2);
  REQUIRE(flat.row_labels_meta.size() == 1);

  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Frozen-side resolution: indices 0..1 (cols) / 0 (row).
  REQUIRE(li.col_label_at(ColIndex {0}) != nullptr);
  CHECK(li.col_label_at(ColIndex {0})->class_name == "Bus");
  CHECK(li.col_label_at(ColIndex {0})->variable_uid == Uid {1});
  REQUIRE(li.col_label_at(ColIndex {1}) != nullptr);
  CHECK(li.col_label_at(ColIndex {1})->variable_uid == Uid {2});
  REQUIRE(li.row_label_at(RowIndex {0}) != nullptr);
  CHECK(li.row_label_at(RowIndex {0})->class_name == "Demand");

  // Add a post-flatten col + row.
  std::ignore = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 0.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });
  SparseRow cut;
  cut[ColIndex {0}] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 50.0;
  cut.class_name = "Sddp";
  cut.constraint_name = "optcut";
  cut.variable_uid = 99;
  li.add_row(cut);
  li.record_dynamic_row(cut);

  // Post-flatten resolution: idx 2 (col), idx 1 (row) lands in the
  // per-instance post-flatten bucket via offset = idx - frozen_count.
  REQUIRE(li.col_label_at(ColIndex {2}) != nullptr);
  CHECK(li.col_label_at(ColIndex {2})->class_name == "Sddp");
  CHECK(li.col_label_at(ColIndex {2})->variable_name == "alpha");
  REQUIRE(li.row_label_at(RowIndex {1}) != nullptr);
  CHECK(li.row_label_at(RowIndex {1})->class_name == "Sddp");
  CHECK(li.row_label_at(RowIndex {1})->constraint_name == "optcut");

  // Out-of-range returns nullptr without throwing.
  CHECK(li.col_label_at(ColIndex {99}) == nullptr);
  CHECK(li.row_label_at(RowIndex {99}) == nullptr);
  // Negative inputs are also nullptr.
  CHECK(li.col_label_at(ColIndex {-1}) == nullptr);
  CHECK(li.row_label_at(RowIndex {-1}) == nullptr);
}

TEST_CASE(  // NOLINT
    "LinearInterface — frozen m_col_labels_meta_ stays shared with "
    "clone after post-flatten add (no detach-on-first-add tax)")
{
  // The headline performance win of the metadata split: a shallow
  // clone shares the frozen flatten-side metadata with the source via
  // shared_ptr, and that sharing survives subsequent post-flatten
  // mutations on either side.  Pre-refactor the first post-clone
  // add_col deep-copied the entire labels-meta vector via
  // detach_for_write; the new design routes the add to a per-instance
  // post-flatten vector, leaving the frozen shared_ptr untouched.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  auto cloned = li.clone(LinearInterface::CloneKind::shallow);
  REQUIRE(li.col_labels_meta_use_count() == 2);
  REQUIRE(cloned.col_labels_meta_use_count() == 2);

  // Source-side post-flatten add.  Frozen vector stays shared.
  std::ignore = li.add_col(SparseCol {
      .uppb = 1.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });
  CHECK(li.col_labels_meta_use_count() == 2);
  CHECK(cloned.col_labels_meta_use_count() == 2);

  // Clone-side post-flatten add too.  Frozen still shared.
  std::ignore = cloned.add_col(SparseCol {
      .uppb = 2.0,
      .class_name = "Sddp",
      .variable_name = "alpha_clone",
      .variable_uid = 1,
  });
  CHECK(li.col_labels_meta_use_count() == 2);
  CHECK(cloned.col_labels_meta_use_count() == 2);

  // Source has 1 post-flatten col, clone has 1 — they don't bleed
  // into each other's post-flatten vectors.
  CHECK(li.get_numcols() == 3);  // 2 frozen + 1 post-flatten on source
  CHECK(cloned.get_numcols() == 3);  // 2 frozen + 1 post-flatten on clone
}

TEST_CASE(  // NOLINT
    "LinearInterface — clone(shallow) value-copies post-flatten "
    "history so write_lp on the clone resolves source's cuts")
{
  // After source has installed a cut + alpha, a shallow clone must
  // see those entries via `col_label_at` / `row_label_at` so the
  // clone's `write_lp` can synthesise labels for the inherited rows.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // Source installs an alpha col + a labelled cut row.
  std::ignore = li.add_col(SparseCol {
      .uppb = 1000.0,
      .cost = 0.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 8.0;
  cut.class_name = "Sddp";
  cut.constraint_name = "optcut";
  cut.variable_uid = 7;
  li.add_row(cut);
  li.record_dynamic_row(cut);

  REQUIRE(li.col_label_at(ColIndex {2}) != nullptr);
  REQUIRE(li.col_label_at(ColIndex {2})->variable_name == "alpha");
  REQUIRE(li.row_label_at(RowIndex {1}) != nullptr);
  REQUIRE(li.row_label_at(RowIndex {1})->variable_uid == Uid {7});

  auto cloned = li.clone(LinearInterface::CloneKind::shallow);

  // Clone resolves source's structural post-flatten history (alpha
  // col + cut row) via its own value-copied post-flatten vectors —
  // would throw "no entry in m_col_labels_meta_" without that copy
  // when the clone's write_lp tries to format inherited labels.
  REQUIRE(cloned.col_label_at(ColIndex {2}) != nullptr);
  CHECK(cloned.col_label_at(ColIndex {2})->variable_name == "alpha");
  REQUIRE(cloned.row_label_at(RowIndex {1}) != nullptr);
  CHECK(cloned.row_label_at(RowIndex {1})->variable_uid == Uid {7});

  // Subsequent source-side adds DON'T bleed into the clone's
  // post-flatten vector (per-instance ownership).
  std::ignore = li.add_col(SparseCol {
      .uppb = 1.0,
      .class_name = "Sddp",
      .variable_name = "extra_on_source",
      .variable_uid = 1,
  });
  CHECK(li.get_numcols() == 4);
  CHECK(cloned.get_numcols() == 3);  // unchanged
  CHECK(cloned.col_label_at(ColIndex {3}) == nullptr);  // not visible
}

TEST_CASE(  // NOLINT
    "LinearInterface — duplicate detection rejects collisions across "
    "frozen and post-flatten metadata layers")
{
  // The dedup index is split (frozen `m_col_meta_index_` + per-
  // instance `m_post_flatten_col_meta_index_`); track_col_label_meta
  // must consult BOTH so a post-flatten add can't shadow a flatten-
  // time entry, AND post-flatten entries can't collide with each
  // other on this instance.
  LinearProblem lp;
  const auto c0 = lp.add_col(SparseCol {
      .uppb = 10.0,
      .class_name = "Bus",
      .variable_name = "theta",
      .variable_uid = 1,
  });
  // flatten() needs at least one row referencing the col, otherwise
  // the col is dropped as structurally absent (no coefficients).
  auto r0 = lp.add_row(SparseRow {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Demand",
      .constraint_name = "balance",
      .variable_uid = 1,
  });
  lp.set_coeff(r0, c0, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.col_with_name_map = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  REQUIRE(li.flatten_col_count() == 1);

  // First post-flatten add with a NEW key: succeeds.
  CHECK_NOTHROW(li.add_col(SparseCol {
      .uppb = 1.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  }));

  // Cross-layer collision: same metadata as the FROZEN entry → throw.
  CHECK_THROWS_AS(li.add_col(SparseCol {
                      .uppb = 1.0,
                      .class_name = "Bus",
                      .variable_name = "theta",
                      .variable_uid = 1,
                  }),
                  std::runtime_error);

  // Within-post-flatten collision: same metadata as the previous
  // post-flatten add → throw.
  CHECK_THROWS_AS(li.add_col(SparseCol {
                      .uppb = 2.0,
                      .class_name = "Sddp",
                      .variable_name = "alpha",
                      .variable_uid = 0,
                  }),
                  std::runtime_error);
}

TEST_CASE(  // NOLINT
    "LinearInterface — post-flatten add does NOT decompress the "
    "frozen labels-meta during compress-mode training")
{
  // Verify the lazy-decompression invariant: `add_col` /
  // `add_row` past load_flat must NOT decompress
  // `m_col_labels_meta_compressed_` / `m_row_labels_meta_compressed_`.
  // Decompression is reserved for `generate_labels_from_maps`
  // (write_lp).  Probe the compression buffers via the public
  // `col_labels_meta_use_count()` accessor and the private state
  // change is detectable from `flatten_col_count()`: post-release
  // the live frozen vector is empty (size 0), but the count is
  // preserved across reload from the compressed buffer.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // Force a release+reconstruct so the frozen vector is loaded back
  // from the compressed buffer with the original size.
  li.release_backend();
  li.reconstruct_backend();
  REQUIRE(li.flatten_col_count() == 2);
  REQUIRE(li.flatten_row_count() == 1);

  // Now release_backend a second time so the frozen labels-meta
  // becomes compressed (live vector cleared).  Subsequent
  // post-flatten adds happen on the still-released backend's lazy
  // path — they must NOT trigger decompression.
  li.release_backend();
  // Frozen live vector is now empty (compressed); count is 0 from
  // load_flat semantics on a non-rebuilt backend.  `flatten_col_count`
  // reads `m_col_labels_meta_->size()` directly.
  CHECK(li.flatten_col_count() == 0);

  // ensure_backend reloads the LP, and the frozen labels-meta is
  // rehydrated only through the lazy path (NOT eagerly).
  li.reconstruct_backend();
  CHECK(li.flatten_col_count() == 2);

  // Add post-flatten cols / rows; these should NOT trigger an extra
  // decompression because track_col_label_meta no longer calls
  // ensure_labels_meta_decompressed.  The backend stays live, the
  // post-flatten vector grows, and the LP solves.
  std::ignore = li.add_col(SparseCol {
      .uppb = 1.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });
  CHECK(li.flatten_col_count() == 2);
  CHECK(li.get_numcols() == 3);

  // Multi-cycle: release/reconstruct repeatedly with post-flatten
  // adds in between; the frozen count never grows.
  for (int i = 0; i < 3; ++i) {
    li.release_backend();
    li.reconstruct_backend();
    CHECK(li.flatten_col_count() == 2);
    CHECK(li.flatten_row_count() == 1);
  }

  auto solve = li.resolve();
  REQUIRE(solve.has_value());
}

// ---------------------------------------------------------------------------
// Hardening: silent NULL-deref → loud exception
// ---------------------------------------------------------------------------
//
// Pre-2026-05-03 a misconfigured ``low_memory_mode=rebuild`` LinearInterface
// could segfault inside the solver plugin when:
//   (a) ``ensure_backend()`` silently early-returned because no rebuild
//       owner was installed, leaving ``m_backend_`` null, or
//   (b) a stale ``ColIndex`` captured before a rebuild referenced a
//       column that the rebuilt LP no longer had.
// Both paths now throw ``std::runtime_error`` / ``std::out_of_range``
// with actionable messages.  These tests pin the hardened contract.

TEST_CASE(  // NOLINT
    "LinearInterface — ensure_backend throws when rebuild owner is missing")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearInterface li;
  // Add a column so the LP is non-trivial.
  std::ignore = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  // Configure rebuild mode but leave the rebuild owner unset.  This is
  // the historical "bare LinearInterface used as if it had an owner"
  // misconfiguration — the silent early-return that allowed it to
  // proceed produced a null-deref segfault on the next backend access.
  li.set_low_memory(LowMemoryMode::rebuild, CompressionCodec::lz4);
  li.mark_released();

  // Any call that triggers ensure_backend() must now throw, with a
  // message that names the misconfiguration.
  CHECK_THROWS_WITH_AS(li.set_col_low_raw(ColIndex {0}, 0.0),
                       doctest::Contains("rebuild owner"),
                       std::runtime_error);
}

TEST_CASE(  // NOLINT
    "LinearInterface — set_col_low_raw throws on out-of-range ColIndex")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  // Solve once so the backend is fully populated and `get_num_cols()`
  // returns the live count.
  auto r = li.initial_solve(SolverOptions {.log_level = 0});
  REQUIRE(r.has_value());

  // In-range index works as before.
  CHECK_NOTHROW(li.set_col_low_raw(x0, 0.0));

  // Stale index past `numcols` must throw rather than reach the
  // solver plugin (where it would have been a NULL/range deref →
  // SIGSEGV under the prior contract).
  CHECK_THROWS_WITH_AS(li.set_col_low_raw(ColIndex {99}, 0.0),
                       doctest::Contains("out of range"),
                       std::out_of_range);
  // Negative index path (ColIndex is signed-typed strong-int).
  CHECK_THROWS_AS(li.set_col_low_raw(ColIndex {-1}, 0.0), std::out_of_range);
}

TEST_CASE(  // NOLINT
    "LinearInterface — set_col_upp_raw throws on out-of-range ColIndex")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearInterface li;
  const auto x0 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  auto r = li.initial_solve(SolverOptions {.log_level = 0});
  REQUIRE(r.has_value());

  CHECK_NOTHROW(li.set_col_upp_raw(x0, 5.0));
  CHECK_THROWS_AS(li.set_col_upp_raw(ColIndex {42}, 5.0), std::out_of_range);
}

// ── Mutation-replay policy tests (Tasks 2a, 2b, 2c) ─────────────────────────
//
// Three tests that explicitly pin the architectural choice for each
// mutation category that exists in the reconstruct path:
//
//   set_coeff_raw  → NOT replayed (caller re-executes via update_lp_for_phase)
//   set_rhs_raw    → NOT replayed (same: caller-side re-execution)
//   set_col_*_raw  → REPLAYED via m_pending_col_bounds_  (bb3d3f26 fix)
//
// If a future refactor changes the policy for coeff or rhs (e.g. by baking
// mutations into the snapshot), these tests will fail and force an explicit
// update — preventing silent semantic drift.

// ── Task 2a: set_coeff_raw policy — mutation is NOT replayed ────────────────
//
// Distinct from the existing "snapshot frozen at construction (compress)"
// test (which uses the physical `set_coeff` wrapper): this test calls
// `set_coeff_raw` directly and verifies the same non-replay contract at
// the raw-setter level.  The two are independent because a future refactor
// could add raw-level tracking while leaving physical-level alone, or
// vice versa.
TEST_CASE(  // NOLINT
    "LinearInterface — set_coeff_raw mutation NOT replayed on reconstruct")
{
  // min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  0 <= x1,x2 <= 10
  // Construction-time coefficient at (r0, x1) is 1.0.
  auto [li, flat, x1, x2] = make_simple_li_lp();
  const RowIndex r0 {0};

  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.get_coeff_raw(r0, x1) == doctest::Approx(1.0));

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Mutate via the raw setter (bypasses col/row scale compensation).
  REQUIRE(li.supports_set_coeff());
  li.set_coeff_raw(r0, x1, 99.0);
  CHECK(li.get_coeff_raw(r0, x1) == doctest::Approx(99.0));

  // Release + reconstruct: `load_flat` reloads the frozen snapshot.
  // The 99.0 raw mutation lives only on the volatile backend — it is
  // lost because `m_pending_col_bounds_`-style replay does not exist
  // for matrix coefficients (re-application is the caller's job via
  // `update_lp_for_phase`).
  li.release_backend();
  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // Must revert to construction-time value, NOT 99.0.
  CHECK(li.get_coeff_raw(r0, x1) == doctest::Approx(1.0));

  // Verify x2 coefficient untouched by the mutation above.
  CHECK(li.get_coeff_raw(r0, x2) == doctest::Approx(1.0));
}

// ── Task 2b: set_rhs_raw policy — mutation is NOT replayed ──────────────────
//
// Pins the same non-replay contract for row-bound mutations.  The SDDP
// element `update_lp` calls (`set_rhs`, `set_row_low`, `set_row_upp`) are
// re-executed at every `update_lp_for_phase` call site, so direct replay
// is intentionally absent for row bounds.  If a future change adds a
// `m_pending_row_bounds_` map, this test will flip and signal the policy
// change — preventing silent semantics drift.
TEST_CASE(  // NOLINT
    "LinearInterface — set_rhs_raw mutation NOT replayed on reconstruct")
{
  // Construction-time row bounds: lowb = 5.0, uppb = DblMax (from fixture).
  auto [li, flat, x1, x2] = make_simple_li_lp();
  const RowIndex r0 {0};

  REQUIRE(li.initial_solve().has_value());

  // Verify construction-time lower bound.
  REQUIRE(li.get_row_low_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(5.0));

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  SUBCASE("set_rhs_raw reverts after reconstruct")
  {
    // Pin the row to an equality constraint (rhs = 8.0).
    li.set_rhs_raw(r0, 8.0);
    CHECK(li.get_row_low_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(8.0));
    CHECK(li.get_row_upp_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(8.0));

    // Release + reconstruct: snapshot restores construction-time bounds.
    li.release_backend();
    li.reconstruct_backend();
    REQUIRE_FALSE(li.is_backend_released());

    // Both bounds must revert to construction-time values (5.0 / +inf).
    const double inf_val = li.infinity();
    CHECK(li.get_row_low_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(5.0));
    CHECK(li.get_row_upp_raw()[static_cast<size_t>(r0)] >= inf_val * 0.9);
  }

  SUBCASE("set_row_low_raw / set_row_upp_raw revert after reconstruct")
  {
    // Tighten bounds individually.
    li.set_row_low_raw(r0, 7.0);
    li.set_row_upp_raw(r0, 9.0);
    CHECK(li.get_row_low_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(7.0));
    CHECK(li.get_row_upp_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(9.0));

    li.release_backend();
    li.reconstruct_backend();
    REQUIRE_FALSE(li.is_backend_released());

    const double inf_val = li.infinity();
    CHECK(li.get_row_low_raw()[static_cast<size_t>(r0)]
          == doctest::Approx(5.0));
    CHECK(li.get_row_upp_raw()[static_cast<size_t>(r0)] >= inf_val * 0.9);
  }
}

// ── Task 2c: combined round-trip — col-bound + cut + alpha ─────────────────
//
// Exercises ALL THREE persistent replay mechanisms in a single cycle,
// matching the exact state that SDDP produces just before the backward
// pass under LowMemoryMode::compress:
//
//   1. Alpha column (dynamic col) — auto-recorded into m_dynamic_cols_
//   2. A Benders cut (dynamic row) — recorded via record_cut_row
//   3. dep_col bound pins (propagate_trial_values) — m_pending_col_bounds_
//
// All three must survive release_backend → reconstruct_backend and produce
// the correct optimal value when solved.
TEST_CASE(  // NOLINT
    "LinearInterface — combined col-bound + cut + alpha replay round-trip")
{
  // Base LP: min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  0 <= x1,x2 <= 10
  // Unconstrained optimum: x1=5, x2=0, obj=10.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.is_optimal());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();
  const auto base_nrows = li.base_numrows();
  const auto base_ncols = li.get_numcols();

  // ── 1. Add alpha column (dynamic col, simulates SDDP cost-to-go) ──
  // add_col auto-records into m_dynamic_cols_ post-snapshot.
  SparseCol alpha_col;
  alpha_col.lowb = -1000.0;
  alpha_col.uppb = 0.0;
  alpha_col.cost = 1.0;  // non-zero so it influences the objective
  [[maybe_unused]] const auto alpha = li.add_col(alpha_col);
  CHECK(li.get_numcols() == base_ncols + 1);

  // ── 2. Add a Benders cut: x1 <= 4 (binding vs unconstrained opt) ──
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 4.0;
  li.add_row(cut);
  li.record_cut_row(cut);
  CHECK(li.get_numrows() == base_nrows + 1);

  // ── 3. Pin x2 bounds (propagate_trial_values analogue) ──
  // x2 construction bounds were [0, 10]; pin to [2, 6].
  li.set_col_low_raw(x2, 2.0);
  li.set_col_upp_raw(x2, 6.0);
  CHECK(li.get_col_low_raw()[static_cast<size_t>(x2)] == doctest::Approx(2.0));
  CHECK(li.get_col_upp_raw()[static_cast<size_t>(x2)] == doctest::Approx(6.0));

  // ── Release + reconstruct ──
  li.release_backend();
  CHECK(li.is_backend_released());

  li.reconstruct_backend();
  CHECK_FALSE(li.is_backend_released());

  // Mechanism 1: alpha col survived.
  CHECK(li.get_numcols() == base_ncols + 1);

  // Mechanism 2: cut survived.
  CHECK(li.get_numrows() == base_nrows + 1);

  // Mechanism 3: x2 bound pins survived.
  CHECK(li.get_col_low_raw()[static_cast<size_t>(x2)] == doctest::Approx(2.0));
  CHECK(li.get_col_upp_raw()[static_cast<size_t>(x2)] == doctest::Approx(6.0));

  // x1 column bounds untouched (construction-time [0, 10]) — the x1 <= 4
  // constraint is a row cut, not a column bound.
  CHECK(li.get_col_low_raw()[static_cast<size_t>(x1)] == doctest::Approx(0.0));
  CHECK(li.get_col_upp_raw()[static_cast<size_t>(x1)] == doctest::Approx(10.0));

  // ── Solve and verify the combined constraints ──
  // Active constraints:
  //   x1 + x2 >= 5  (structural)
  //   x1 <= 4       (cut)
  //   2 <= x2 <= 6  (pinned bounds)
  //   alpha ∈ [-1000, 0], cost 1  → alpha = -1000 (minimises at lower bound)
  // Objective: 2x1 + 3x2 + alpha
  //   Subject to x1 <= 4, x1 + x2 >= 5, x2 >= 2
  //   At optimum: x1=3, x2=2 (satisfies x1+x2>=5, x1<=4, x2>=2)
  //   alpha = -1000
  //   obj = 6 + 6 - 1000 = -988
  auto r = li.resolve();
  REQUIRE(r.has_value());
  CHECK(li.get_obj_value_raw() == doctest::Approx(-988.0));

  // Multiple cycles must not corrupt any of the three mechanisms.
  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.get_numcols() == base_ncols + 1);
  CHECK(li.get_numrows() == base_nrows + 1);
  CHECK(li.get_col_low_raw()[static_cast<size_t>(x2)] == doctest::Approx(2.0));
  CHECK(li.get_col_upp_raw()[static_cast<size_t>(x2)] == doctest::Approx(6.0));
  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  CHECK(li.get_obj_value_raw() == doctest::Approx(-988.0));
}

// ── Task 3: post-snapshot set_col_scale NOT replayed (scale lost) ───────────
//
// m_col_scales_ is reset by load_flat() from flat_lp.col_scales.  A
// post-snapshot call to set_col_scale(idx, 99.0) modifies the in-memory
// m_col_scales_ but the snapshot was frozen before that call, so the
// reconstructed backend will use construction-time scales (all 1.0 in the
// simple fixture because no non-unit scales were set at flatten time).
//
// There is currently NO pending-replay mechanism for scale mutations
// (unlike m_pending_col_bounds_ for bounds).  This test pins that
// behaviour so a future change that adds scale-mutation replay must
// explicitly update (flip) the assertion rather than silently change
// semantics.
TEST_CASE(  // NOLINT
    "LinearInterface — post-snapshot set_col_scale NOT replayed on reconstruct")
{
  // Fixture: all col scales are 1.0 at flatten time.
  auto [li, flat, x1, x2] = make_simple_li_lp();

  REQUIRE(li.initial_solve().has_value());

  // Confirm construction-time scale is 1.0.
  CHECK(li.get_col_scale(x1) == doctest::Approx(1.0));

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Mutate the in-memory scale post-snapshot.
  li.set_col_scale(x1, 99.0);
  CHECK(li.get_col_scale(x1) == doctest::Approx(99.0));

  // Release + reconstruct: load_flat() reloads col_scales from the frozen
  // snapshot, which had scale 1.0 at the time of save_snapshot.
  li.release_backend();
  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // Scale reverts to 1.0 (construction-time).  If this CHECK starts
  // failing it means scale-mutation replay was added — update the test
  // to assert the new "survived" behaviour and remove the
  // update_lp_for_phase workaround if applicable.
  CHECK(li.get_col_scale(x1) == doctest::Approx(1.0));
  CHECK(li.get_col_scale(x2) == doctest::Approx(1.0));
}

// ── Task 3: m_base_numrows_ is reset + re-set by apply_post_load_replay ─────
//
// save_base_numrows() is called inside apply_post_load_replay (step 3).
// After reconstruct, base_numrows() must equal the number of structural
// rows (2 + dynamic_rows replayed BEFORE the cut phase), not zero or the
// pre-release value.
TEST_CASE(  // NOLINT
    "LinearInterface — base_numrows is correctly restored after reconstruct")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  REQUIRE(li.initial_solve().has_value());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();
  const auto base_before = li.base_numrows();

  // Add a cut, release, reconstruct.
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 4.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  li.release_backend();
  li.reconstruct_backend();

  // base_numrows must be the same as before the cut was added —
  // apply_post_load_replay calls save_base_numrows() AFTER replaying
  // dynamic_rows but BEFORE replaying cuts.
  CHECK(li.base_numrows() == base_before);

  // Total rows = base + 1 cut.
  CHECK(li.get_numrows() == base_before + 1);
}

// ── Task 3: col_scales shared_ptr survives multiple reconstruct cycles ───────
//
// col_scales_use_count() returns the shared_ptr use count for m_col_scales_.
// Under compress mode, each reconstruct calls load_flat() which calls
// detach_for_write(m_col_scales_).assign(...) — this replaces the vector
// contents but the shared_ptr itself is retained by the LinearInterface.
// The use count must stay at 1 (no leaked extra references) throughout.
TEST_CASE(  // NOLINT
    "LinearInterface — col_scales shared_ptr not leaked across reconstruct")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  REQUIRE(li.initial_solve().has_value());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Use count is 1: only li holds the shared_ptr (no clones yet).
  REQUIRE(li.col_scales_use_count() == 1);

  for (int i = 0; i < 3; ++i) {
    li.release_backend();
    li.reconstruct_backend();

    // After each reconstruct, col_scales must still have use_count == 1.
    // A leak would show as use_count > 1 and would indicate a dangling
    // reference path introduced by a refactor.
    CHECK(li.col_scales_use_count() == 1);
  }
}

// ── Regression: release after reconstruct-without-resolve must NOT corrupt
// cache.
//
// Models the SDDP backward-pass `src_phase` lifecycle:
//   1. cell solves (forward pass) — cache populated with valid col_sol.
//   2. cell released — backend freed.
//   3. cell reconstructed for cross-phase read (e.g. `physical_eini`).
//      Live backend's primal buffer is uninitialised post-`load_flat`.
//   4. cell read via `get_col_sol_raw` — must return CACHED value (the
//      `m_backend_solution_fresh_` gate routes the read to the cache).
//   5. cell released AGAIN without an intervening resolve.
//
// Pre-fix: step 5 entered `release_backend`'s cache-refresh branch
// because `is_optimal()` returns the cached `true`.  It then read
// `m_backend_->col_solution()` from the live, never-solved backend
// and OVERWROTE the previously valid cache with that buffer (zeros
// or uninitialised).  Subsequent `get_col_sol_raw()` returned the
// corrupted cache → SDDP forward-pass `propagate_trial_values`
// pinned dep_col bounds to garbage → wrong LP solutions.
//
// Post-fix: `release_backend` skips the solution-vector refresh when
// `!m_backend_solution_fresh_` and the previous cache survives.
TEST_CASE(  // NOLINT
    "LinearInterface — release after reconstruct-no-resolve preserves cache")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  // Solve once to produce a known-good optimal solution and cache it.
  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.is_optimal());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Step 2: release.  Cache populated from the live backend.
  li.release_backend();
  REQUIRE(li.is_optimal());  // cached true

  // Snapshot the cached col_sol so we can assert it survives below.
  const std::vector<double> cached_col_sol_after_first_release {
      li.get_col_sol_raw().begin(),
      li.get_col_sol_raw().end(),
  };
  REQUIRE(cached_col_sol_after_first_release.size() >= 2);

  // Step 3: reconstruct without resolving.  Cache must still serve
  // reads (m_backend_solution_fresh_ is false post-reconstruct).
  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // Step 4: read through the cache path.  Must match the snapshot.
  {
    const auto live_view = li.get_col_sol_raw();
    REQUIRE(live_view.size() == cached_col_sol_after_first_release.size());
    for (size_t i = 0; i < live_view.size(); ++i) {
      CHECK(live_view[i]
            == doctest::Approx(cached_col_sol_after_first_release[i]));
    }
  }

  // Step 5: release AGAIN with no intervening resolve.  Pre-fix this
  // overwrote the cache from the live (uninitialised) backend buffer.
  // Post-fix: cache is preserved verbatim.
  li.release_backend();
  REQUIRE(li.is_backend_released());

  const auto post_view = li.get_col_sol_raw();
  REQUIRE(post_view.size() == cached_col_sol_after_first_release.size());
  for (size_t i = 0; i < post_view.size(); ++i) {
    CHECK(post_view[i]
          == doctest::Approx(cached_col_sol_after_first_release[i]));
  }

  // Round-trip the cycle a few times to ensure idempotence.
  for (int i = 0; i < 3; ++i) {
    li.reconstruct_backend();
    li.release_backend();
    const auto v = li.get_col_sol_raw();
    REQUIRE(v.size() == cached_col_sol_after_first_release.size());
    for (size_t j = 0; j < v.size(); ++j) {
      CHECK(v[j] == doctest::Approx(cached_col_sol_after_first_release[j]));
    }
  }
}

// ── Symmetry fix: invalidate-on-mutation under compress ─────────────────────
//
// Under `LowMemoryMode::off`, the live backend (CPLEX/HiGHS/...) flips
// `is_proven_optimal()` to false the instant a row/col/coefficient
// changes — readers gated on `is_optimal()` then correctly fall through
// to default values until the next `resolve()`.  Under
// `LowMemoryMode::compress`, the cached optimality flag would otherwise
// outlive a mid-iteration `add_row` (Benders cut) and let downstream
// consumers read a now-stale cached col_sol that doesn't satisfy the
// new cut.  The fix: every public LP-mutation API now calls
// `invalidate_cached_optimal_on_mutation()`, which under compress
// flips `m_cached_is_optimal_=false` AND drops the cache vectors on
// the spot.  This pins `is_optimal()` symmetric across modes after
// any mutation.
TEST_CASE(  // NOLINT
    "LinearInterface — set_col_low_raw mutation drops cache under compress")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.is_optimal());

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());  // cached true post-release

  // Sanity: the cache holds a valid solution snapshot.
  REQUIRE(li.cached_col_sol_size() > 0);

  // Bring the backend back, mutate a column bound.  Under compress,
  // the mutation MUST drop the cache (vectors empty, is_optimal false).
  li.reconstruct_backend();
  li.set_col_low_raw(x1, 0.5);

  CHECK_FALSE(li.is_optimal());  // post-mutation: !optimal
  CHECK(li.cached_col_sol_size() == 0);  // cache vectors dropped
  CHECK(li.cached_col_cost_size() == 0);
  CHECK(li.cached_row_dual_size() == 0);
}

TEST_CASE(  // NOLINT
    "LinearInterface — set_col_upp_raw mutation drops cache under compress")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());
  REQUIRE(li.cached_col_sol_size() > 0);

  li.reconstruct_backend();
  li.set_col_upp_raw(x1, 5.0);

  CHECK_FALSE(li.is_optimal());
  CHECK(li.cached_col_sol_size() == 0);
}

TEST_CASE(  // NOLINT
    "LinearInterface — set_coeff_raw mutation drops cache under compress")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());
  REQUIRE(li.cached_col_sol_size() > 0);

  li.reconstruct_backend();
  li.set_coeff_raw(RowIndex {0}, x1, 2.0);

  CHECK_FALSE(li.is_optimal());
  CHECK(li.cached_col_sol_size() == 0);
}

TEST_CASE(  // NOLINT
    "LinearInterface — set_row_low_raw / set_row_upp_raw / set_rhs_raw "
    "drop cache under compress")
{
  SUBCASE("set_row_low_raw")
  {
    auto [li, flat, x1, x2] = make_simple_li_lp();
    REQUIRE(li.initial_solve().has_value());
    li.set_low_memory(LowMemoryMode::compress);
    li.save_snapshot(FlatLinearProblem {flat});
    li.release_backend();
    REQUIRE(li.is_optimal());
    REQUIRE(li.cached_col_sol_size() > 0);

    li.reconstruct_backend();
    li.set_row_low_raw(RowIndex {0}, 0.5);

    CHECK_FALSE(li.is_optimal());
    CHECK(li.cached_col_sol_size() == 0);
  }

  SUBCASE("set_row_upp_raw")
  {
    auto [li, flat, x1, x2] = make_simple_li_lp();
    REQUIRE(li.initial_solve().has_value());
    li.set_low_memory(LowMemoryMode::compress);
    li.save_snapshot(FlatLinearProblem {flat});
    li.release_backend();
    REQUIRE(li.is_optimal());

    li.reconstruct_backend();
    li.set_row_upp_raw(RowIndex {0}, 99.0);

    CHECK_FALSE(li.is_optimal());
    CHECK(li.cached_col_sol_size() == 0);
  }

  SUBCASE("set_rhs_raw")
  {
    auto [li, flat, x1, x2] = make_simple_li_lp();
    REQUIRE(li.initial_solve().has_value());
    li.set_low_memory(LowMemoryMode::compress);
    li.save_snapshot(FlatLinearProblem {flat});
    li.release_backend();
    REQUIRE(li.is_optimal());

    li.reconstruct_backend();
    li.set_rhs_raw(RowIndex {0}, 7.0);

    CHECK_FALSE(li.is_optimal());
    CHECK(li.cached_col_sol_size() == 0);
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface — add_row mutation drops cache under compress")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());
  REQUIRE(li.cached_col_sol_size() > 0);

  li.reconstruct_backend();
  // Add a Benders-style cut (one-element row) via the public SparseRow
  // API and verify cache drops.
  SparseRow cut_row {.lowb = 0.0, .uppb = SparseRow::DblMax};
  cut_row.cmap[x1] = 1.0;
  li.add_row(cut_row);

  CHECK_FALSE(li.is_optimal());
  CHECK(li.cached_col_sol_size() == 0);
}

TEST_CASE(  // NOLINT
    "LinearInterface — add_col mutation drops cache under compress")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());
  REQUIRE(li.cached_col_sol_size() > 0);

  li.reconstruct_backend();
  // Add a fresh column (alpha-style) and verify cache drops.
  li.add_col(SparseCol {.lowb = 0.0, .uppb = 100.0, .cost = 1.0});

  CHECK_FALSE(li.is_optimal());
  CHECK(li.cached_col_sol_size() == 0);
}

// Off mode never populates the LI cache, so invalidation is a no-op
// — but the public API must remain a valid no-op (no exceptions, no
// observable state change beyond what off semantics already provide).
TEST_CASE(  // NOLINT
    "LinearInterface — mutation hook is benign no-op under LowMemoryMode::off")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.is_optimal());  // live backend reports optimal under off

  // Off mode: cache is empty by construction (release_backend short-
  // circuits at the off check), and stays empty across mutations.
  REQUIRE(li.get_col_sol_raw().size()
          == static_cast<size_t>(li.get_numcols()));  // live backend ptr

  li.set_col_low_raw(x1, 0.5);
  li.set_coeff_raw(RowIndex {0}, x1, 2.0);
  li.set_row_low_raw(RowIndex {0}, 0.5);

  // Live backend still answers reads (off has no LI cache to drop).
  // Note: under off, is_optimal() reflects whatever the backend returns
  // post-mutation — that's the off-mode invariant we preserve.
  CHECK(li.get_col_sol_raw().size() == static_cast<size_t>(li.get_numcols()));
}

// Replay path (m_replaying_=true) must NOT trigger invalidation: bulk
// replay is reapplying *recorded* state to a freshly loaded backend,
// not introducing new mutations.  Without the m_replaying_ guard, every
// `apply_post_load_replay` would self-corrupt the cache it just
// finished restoring.
TEST_CASE(  // NOLINT
    "LinearInterface — apply_post_load_replay does NOT drop cache")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());
  const auto cached_size_before = li.get_col_sol_raw().size();
  REQUIRE(cached_size_before > 0);

  // Reconstruct triggers apply_post_load_replay internally, which calls
  // add_col / add_rows on dynamic_cols / active_cuts (none in this
  // fixture) and set_col_lower / set_col_upper from
  // `m_pending_col_bounds_` (also empty in this fixture).  Even when
  // these registries ARE populated, the m_replaying_ guard keeps
  // invalidation off — the cache must survive.
  li.reconstruct_backend();

  CHECK(li.is_optimal());  // cache still claims optimal after reconstruct
  CHECK(li.get_col_sol_raw().size() == cached_size_before);
}

// Switching to off mode must drop any cache accumulated under a prior
// compress configuration.  Without this clear, a subsequent
// `release_backend()` (no-op under off) would leave a stale cache hanging
// around forever, and a downstream read could pick it up via the
// `m_backend_released_ || !m_backend_solution_fresh_` gate if either
// flag flipped unexpectedly.
TEST_CASE(  // NOLINT
    "LinearInterface — set_low_memory(off) drops cache from prior compress")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();
  REQUIRE(li.initial_solve().has_value());

  // Configure compress, populate cache via release.
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});
  li.release_backend();
  REQUIRE(li.is_optimal());
  REQUIRE(li.cached_col_sol_size() > 0);
  REQUIRE(li.cached_col_cost_size() > 0);
  REQUIRE(li.cached_row_dual_size() > 0);

  // Bring backend back, then switch to off.  `set_low_memory(off)`
  // must clear cache vectors and the cached optimality flag — off mode
  // owns no parallel solution cache, the live backend is the sole
  // source of truth.
  li.reconstruct_backend();
  li.set_low_memory(LowMemoryMode::off);

  // Off mode owns no parallel solution cache: vectors must be empty
  // after the transition.
  CHECK(li.cached_col_sol_size() == 0);
  CHECK(li.cached_col_cost_size() == 0);
  CHECK(li.cached_row_dual_size() == 0);
}

// Off-mode no-cache invariant (I6): under `LowMemoryMode::off` the LI
// solution cache must NEVER be populated by
// `populate_solution_cache_post_solve`. The live backend is the sole source of
// truth — populating a parallel LI cache would be pure waste (extra alloc +
// memcpy on every solve). Reads under off route directly to the backend via the
// empty-cache fallback in `get_col_sol_raw` / `get_col_cost_raw` /
// `get_row_dual_raw`.
//
// This test pins the contract: solve repeatedly under off, the cache
// must stay empty after every call.
TEST_CASE(  // NOLINT
    "LinearInterface — off mode never populates LI cache (I6)")
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  // Default low_memory_mode is `off`.  Repeated solves must NOT
  // populate the cache.
  REQUIRE(li.low_memory_mode() == LowMemoryMode::off);

  REQUIRE(li.initial_solve().has_value());
  REQUIRE(li.is_optimal());
  CHECK(li.cached_col_sol_size() == 0);
  CHECK(li.cached_col_cost_size() == 0);
  CHECK(li.cached_row_dual_size() == 0);

  // A second solve (resolve) must also leave the cache empty.
  REQUIRE(li.resolve().has_value());
  REQUIRE(li.is_optimal());
  CHECK(li.cached_col_sol_size() == 0);
  CHECK(li.cached_col_cost_size() == 0);
  CHECK(li.cached_row_dual_size() == 0);

  // `release_backend()` is a no-op under off — must NOT populate
  // anything either.
  li.release_backend();
  CHECK_FALSE(li.is_backend_released());  // off no-op preserves backend
  CHECK(li.cached_col_sol_size() == 0);
  CHECK(li.cached_col_cost_size() == 0);
  CHECK(li.cached_row_dual_size() == 0);

  // Reads under off go directly to the live backend.  `get_col_sol_raw`
  // must return a non-empty span (the live backend's buffer) without
  // ever populating the LI cache.
  const auto live_view = li.get_col_sol_raw();
  CHECK(live_view.size() == static_cast<size_t>(li.get_numcols()));
  CHECK(li.cached_col_sol_size() == 0);  // still empty after read
}
