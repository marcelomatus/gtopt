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
    CHECK(li.get_obj_value() == doctest::Approx(4.0));
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
    CHECK(li.get_obj_value() == doctest::Approx(10.0));
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
    CHECK(li.get_obj_value() == doctest::Approx(10.0));
  }
}

TEST_CASE("LinearInterface — low_memory save_snapshot round-trip")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  // Solve baseline
  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value();
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
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
      CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
    }
  }
}

TEST_CASE(
    "LinearInterface — low_memory reconstruct with dynamic cols")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Add a dynamic column (simulating alpha variable)
  SparseCol alpha_col;
  alpha_col.uppb = 1000.0;
  alpha_col.cost = 0.0;
  [[maybe_unused]] const auto alpha = li.add_col(alpha_col);
  li.record_dynamic_col(alpha_col);
  li.save_base_numrows();

  CHECK(li.get_numcols() == 3);

  // Release and reconstruct — dynamic col should be replayed
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numcols() == 3);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // Alpha has zero cost, so objective is unchanged
  CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
  const double obj_with_cut = li.get_obj_value();
  // x1 <= 3, so optimal x1=3, x2=2 → obj = 6 + 6 = 12
  CHECK(obj_with_cut == doctest::Approx(12.0));

  // Release and reconstruct — cut should be replayed
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numrows() == base_nrows + 1);

  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(obj_with_cut));
}

TEST_CASE("LinearInterface — low_memory cut deletion tracking")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  [[maybe_unused]] const double orig_obj = li.get_obj_value();

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
  CHECK(li.get_obj_value() == doctest::Approx(13.0));
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
  const double orig_obj = li.get_obj_value();

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
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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

TEST_CASE("LinearInterface — low_memory reconstruct with warm-start")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture solution for warm-start
  std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                              li.get_col_sol_raw().end());
  std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                               li.get_row_dual_raw().end());
  const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  li.release_backend();
  li.reconstruct_backend(col_sol, row_dual);

  // Warm-start should allow immediate optimal
  SolverOptions ws_opts;
  auto r = li.resolve(ws_opts);
  REQUIRE(r.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
  CHECK(li.get_obj_value() == doctest::Approx(12.0));
}

TEST_CASE(
    "LinearInterface — low_memory clone from reconstructed backend")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture solution
  std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                              li.get_col_sol_raw().end());
  std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                               li.get_row_dual_raw().end());
  const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  // Release and reconstruct
  li.release_backend();
  li.reconstruct_backend(col_sol, row_dual);

  // Clone from reconstructed backend with warm-start
  auto cloned = li.clone();

  auto r = cloned.resolve();
  REQUIRE(r.has_value());
  CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));

  // Modify clone — original is unmodified
  cloned.set_col_upp(x1, 3.0);
  auto r2 = cloned.resolve();
  REQUIRE(r2.has_value());
  // x1 <= 3 → x1=3, x2=2 → obj = 6 + 6 = 12
  CHECK(cloned.get_obj_value() == doctest::Approx(12.0));

  // Original still produces the same objective
  auto r3 = li.resolve();
  REQUIRE(r3.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
}

TEST_CASE("LinearInterface — low_memory level 2 multiple cycles")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_li_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  [[maybe_unused]] const double orig_obj = li.get_obj_value();

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
  const double obj1 = li.get_obj_value();
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
  CHECK(li.get_obj_value() == doctest::Approx(13.0));
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
    CHECK(cloned.get_obj_value() == doctest::Approx(li.get_obj_value()));
  }

  SUBCASE("clone without warm-start also works")
  {
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value() == doctest::Approx(li.get_obj_value()));
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
  const double orig_obj = li.get_obj_value();

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
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple clones can be created and destroyed")
  {
    for (int i = 0; i < 5; ++i) {
      auto cloned = li.clone();
      auto r = cloned.resolve();
      REQUIRE(r.has_value());
      CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));
      // clone destroyed each iteration
    }

    // Parent still works
    CHECK(li.has_backend());
    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
    CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));

    // Destroy clone, release parent again
    cloned = LinearInterface {};
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    // Reconstruct again and verify
    li.reconstruct_backend();
    auto r2 = li.resolve();
    REQUIRE(r2.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
  const double obj_with_cut = li.get_obj_value();

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
    CHECK(li.get_obj_value() == doctest::Approx(obj_with_cut));
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
  const double orig_obj = li.get_obj_value();

  SUBCASE("released cached scalars")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());

    // Cached scalars are still available
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("reconstruct then resolve produces correct result")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    li.reconstruct_backend();  // cold start

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
      CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
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
  CHECK(li.get_obj_value() == doctest::Approx(10.0));

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
      CHECK(li.get_obj_value() == doctest::Approx(10.0));
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
  SparseCol alpha_col;
  alpha_col.lowb = 0.0;
  alpha_col.uppb = 1000.0;
  alpha_col.cost = 0.0;
  alpha_col.class_name = "Sddp";
  alpha_col.variable_name = "alpha";
  [[maybe_unused]] const auto alpha = li.add_col(alpha_col);
  li.record_dynamic_col(alpha_col);
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

// ── add_row_lp_space with scale != 1.0 ───────────────────────────────────

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

  CHECK(li_scaled.get_obj_value() == doctest::Approx(10.0));
  CHECK(li_plain.get_obj_value() == doctest::Approx(10.0));

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
