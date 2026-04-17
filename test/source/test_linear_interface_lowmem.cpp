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
struct SimpleLp
{
  LinearInterface li;
  FlatLinearProblem flat;
  ColIndex x1;
  ColIndex x2;
};

SimpleLp make_simple_lp()
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

  return SimpleLp {
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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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

TEST_CASE("LinearInterface — low_memory reconstruct with warm-start")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
    "LinearInterface — release_backend destroys solver backend")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
  auto [li, flat, x1, x2] = make_simple_lp();

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
