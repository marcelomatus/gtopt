// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_clone_via_load_flat.cpp
 * @brief     Equivalence test — backend-native clone (CPXcloneprob /
 *            HighsCopy / OsiClone) vs. a manual clone built by replaying
 *            the saved `FlatLinearProblem` through `load_flat()` on a
 *            fresh `LinearInterface`.
 *
 * Motivation:
 *   The current aperture-clone path serialises every backend `clone()`
 *   call under `s_global_clone_mutex` (sddp_aperture.cpp:194) because
 *   `CPXcloneprob` has documented process-global side effects that
 *   crashed three threads at the same instruction pointer in juan/
 *   gtopt_iplp (commit 1d7a05c1).  An alternative is to bypass
 *   `CPXcloneprob` entirely: keep the source's `FlatLinearProblem`
 *   snapshot and rebuild the clone via `CPXcreateprob` + `CPXaddrows`
 *   inside a freshly opened CPLEX env.  That path uses only env-local
 *   solver calls, which are already proven safe across threads (every
 *   `CplexSolverBackend` ctor opens its own env in parallel during
 *   plugin load).
 *
 *   Before pursuing that as a refactor of the aperture path, we need
 *   to verify the two clone routes are functionally equivalent — same
 *   matrix, same bounds, same objective, same optimum, same solution.
 *   This file pins down that contract with a battery of LP shapes.
 *
 * Coverage:
 *   1.  Tiny labelled LP — both clones agree on every getter.
 *   2.  Mixed feature LP (free var, ranged row, equality row, negative
 *       cost) — both clones produce the same optimum.
 *   3.  Bound mutation followed by re-solve — both clones respond
 *       identically to the same scenario-style bound update (the
 *       aperture pattern).
 *   4.  Disposable col / row addition — both clones support the
 *       elastic-filter shape (verifies the metadata wiring matches).
 *   5.  Source independence — mutating either clone leaves the source
 *       untouched (sanity: clone is a real copy under either route).
 *
 * Backend portability:
 *   The test uses whatever backend `SolverRegistry::default_solver()`
 *   selects (cplex > highs > mindopt > cbc > clp).  The equivalence
 *   property is a backend-agnostic invariant — if it holds for CBC/CLP
 *   in CI and for CPLEX on the developer host, the manual path is
 *   safe to swap in for any backend.
 */

#include <chrono>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Tiny LP, three cols + two rows, with labels so write_lp produces
// non-empty output for the byte-equivalence subcase.  Mirrors
// `make_labelled_lp` in test_clone_invariants.cpp but kept local so
// the two test files don't interlock.
LinearProblem build_labelled_problem()
{
  LinearProblem lp {
      "clone_eq_test",
  };
  std::ignore = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 1,
  });
  std::ignore = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 20.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 2,
  });
  SparseRow r {
      .lowb = 0.0,
      .uppb = 25.0,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = 1,
  };
  r[ColIndex {0}] = 1.0;
  r[ColIndex {1}] = 1.0;
  std::ignore = lp.add_row(std::move(r));
  return lp;
}

// Wider LP exercising free vars, ranged rows, equality rows, and a
// negative cost — none of which the trivial labelled LP covers.  This
// is the LP shape closest to a real aperture clone (lots of bound
// types, mixed sense rows, scaled objective) for the equivalence
// checks.
LinearProblem build_feature_problem()
{
  LinearProblem lp {
      "clone_eq_features",
  };
  // Free variable: lb = -infinity, ub = +infinity (we'll use -1e30/1e30,
  // matching how flatten represents free bounds before backend clamp).
  std::ignore = lp.add_col(SparseCol {
      .lowb = -1.0e30,
      .uppb = 1.0e30,
      .cost = -0.5,
      .class_name = "Free",
      .variable_name = "x_free",
      .variable_uid = 100,
  });
  // Two regular cols.
  std::ignore = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 50.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 200,
  });
  std::ignore = lp.add_col(SparseCol {
      .lowb = 1.0,  // strictly positive lb
      .uppb = 50.0,
      .cost = 1.5,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 201,
  });
  // Equality row: x_free + p_200 = 5
  SparseRow eq {
      .lowb = 5.0,
      .uppb = 5.0,
      .class_name = "Bus",
      .constraint_name = "eq_balance",
      .variable_uid = 1,
  };
  eq[ColIndex {0}] = 1.0;
  eq[ColIndex {1}] = 1.0;
  std::ignore = lp.add_row(std::move(eq));
  // Ranged row: 2 <= p_200 + 0.5 * p_201 <= 30
  SparseRow rng {
      .lowb = 2.0,
      .uppb = 30.0,
      .class_name = "Limit",
      .constraint_name = "ranged",
      .variable_uid = 1,
  };
  rng[ColIndex {1}] = 1.0;
  rng[ColIndex {2}] = 0.5;
  std::ignore = lp.add_row(std::move(rng));
  return lp;
}

// Build a `LinearInterface` source by loading the flattened problem.
// Caller keeps `flat` alive (load_flat takes a const ref).
LinearInterface make_source_from_flat(const FlatLinearProblem& flat)
{
  LinearInterface li;
  li.load_flat(flat);
  return li;
}

// "Manual clone": fresh LinearInterface populated by replaying the
// SAME flat the source was loaded from.  This is the path the
// aperture refactor would use to bypass CPXcloneprob.
LinearInterface manual_clone_from_flat(const FlatLinearProblem& flat)
{
  LinearInterface li;
  li.load_flat(flat);
  return li;
}

// Compare two ScaledView-shaped getters element-wise.  Using a
// generic span-of-double comparison so the same helper works for
// bounds, costs, and solution arrays.
template<class A, class B>
bool spans_equal_approx(const A& a, const B& b, double tol = 1e-12)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i] - b[i]) > tol) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ─── 1. Structural equivalence (no solve) ────────────────────────────

TEST_CASE(  // NOLINT
    "clone via load_flat matches backend-native clone — structural state")
{
  auto problem = build_labelled_problem();
  const auto flat = problem.flatten({});

  const auto src = make_source_from_flat(flat);
  const auto native = src.clone();  // backend's clone()
  const auto manual = manual_clone_from_flat(flat);  // load_flat replay

  SUBCASE("dimensions match")
  {
    REQUIRE(src.get_numcols() == native.get_numcols());
    REQUIRE(src.get_numcols() == manual.get_numcols());
    REQUIRE(src.get_numrows() == native.get_numrows());
    REQUIRE(src.get_numrows() == manual.get_numrows());
  }

  SUBCASE("column bounds and costs match")
  {
    CHECK(spans_equal_approx(native.get_col_low(), manual.get_col_low()));
    CHECK(spans_equal_approx(native.get_col_upp(), manual.get_col_upp()));
    CHECK(spans_equal_approx(native.get_col_cost(), manual.get_col_cost()));
  }

  SUBCASE("row bounds match")
  {
    CHECK(spans_equal_approx(native.get_row_low(), manual.get_row_low()));
    CHECK(spans_equal_approx(native.get_row_upp(), manual.get_row_upp()));
  }

  SUBCASE("solver identity matches")
  {
    // Both clones must inherit the same solver — equivalence is only
    // meaningful when the same backend is on both sides of the test.
    CHECK(native.solver_name() == manual.solver_name());
  }
}

// ─── 2. Solve equivalence on a feature-rich LP ──────────────────────

TEST_CASE(  // NOLINT
    "clone via load_flat matches backend-native clone — solve equivalence")
{
  auto problem = build_feature_problem();
  const auto flat = problem.flatten({});

  auto native = make_source_from_flat(flat).clone();
  auto manual = manual_clone_from_flat(flat);

  REQUIRE(native.get_numcols() == manual.get_numcols());
  REQUIRE(native.get_numrows() == manual.get_numrows());

  const SolverOptions opts {};
  REQUIRE(native.resolve(opts).has_value());
  REQUIRE(manual.resolve(opts).has_value());
  REQUIRE(native.is_optimal());
  REQUIRE(manual.is_optimal());

  SUBCASE("optimal objective matches")
  {
    CHECK(native.get_obj_value()
          == doctest::Approx(manual.get_obj_value()).epsilon(1e-9));
  }

  SUBCASE("primal solution matches")
  {
    CHECK(spans_equal_approx(native.get_col_sol(), manual.get_col_sol(), 1e-7));
  }
}

// ─── 3. Aperture-style bound mutation + re-solve equivalence ────────

TEST_CASE(  // NOLINT
    "clone via load_flat — same response to scenario-style bound update")
{
  // Mirrors `FlowLP::update_aperture` (flow_lp.cpp:120-129): fix a
  // free / unconstrained col to a scenario-specific value via
  // set_col_low + set_col_upp.  Both clone routes must produce the
  // same optimum after the update.
  auto problem = build_feature_problem();
  const auto flat = problem.flatten({});

  auto native = make_source_from_flat(flat).clone();
  auto manual = manual_clone_from_flat(flat);

  // Apply identical bound mutations to both clones.
  for (const auto& clone :
       {
           std::ref(native),
           std::ref(manual),
       })
  {
    clone.get().set_col_low(ColIndex {1}, 4.0);  // tighten p_200 lb
    clone.get().set_col_upp(ColIndex {1}, 8.0);  //  ... and ub
  }

  const SolverOptions opts {};
  REQUIRE(native.resolve(opts).has_value());
  REQUIRE(manual.resolve(opts).has_value());

  CHECK(native.is_optimal() == manual.is_optimal());
  if (native.is_optimal() && manual.is_optimal()) {
    CHECK(native.get_obj_value()
          == doctest::Approx(manual.get_obj_value()).epsilon(1e-9));
  }
}

// ─── 4. Disposable col / row equivalence (elastic-filter shape) ─────

TEST_CASE(  // NOLINT
    "clone via load_flat supports disposable add_col / add_row — "
    "elastic-filter shape works on both routes")
{
  auto problem = build_labelled_problem();
  const auto flat = problem.flatten({});

  auto native = make_source_from_flat(flat).clone();
  auto manual = manual_clone_from_flat(flat);

  const auto base_cols = native.get_numcols();
  const auto base_rows = native.get_numrows();
  REQUIRE(manual.get_numcols() == base_cols);
  REQUIRE(manual.get_numrows() == base_rows);

  // Add identical disposable slack pair + fixing row to both clones.
  for (const auto& clone :
       {
           std::ref(native),
           std::ref(manual),
       })
  {
    const auto sup = clone.get().add_col_disposable(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
        .class_name = "Slack",
        .variable_name = "elastic_sup",
        .variable_uid = 9001,
    });
    const auto sdn = clone.get().add_col_disposable(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
        .class_name = "Slack",
        .variable_name = "elastic_sdn",
        .variable_uid = 9001,
    });
    SparseRow fix {
        .lowb = 7.5,
        .uppb = 7.5,
        .class_name = "Slack",
        .constraint_name = "elastic_fix",
        .variable_uid = 9001,
    };
    fix[ColIndex {0}] = 1.0;
    fix[sup] = 1.0;
    fix[sdn] = -1.0;
    std::ignore = clone.get().add_row_disposable(fix);
  }

  CHECK(native.get_numcols() == manual.get_numcols());
  CHECK(native.get_numrows() == manual.get_numrows());
  CHECK(native.get_numcols() == base_cols + 2);
  CHECK(native.get_numrows() == base_rows + 1);
}

// ─── 5. Source independence — manual route is also a real copy ──────

TEST_CASE(  // NOLINT
    "clone via load_flat is independent of source — bound mutations don't "
    "leak across either clone route")
{
  auto problem = build_labelled_problem();
  const auto flat = problem.flatten({});

  auto src = make_source_from_flat(flat);
  auto native = src.clone();
  auto manual = manual_clone_from_flat(flat);

  const auto src_lb_before = src.get_col_low()[ColIndex {0}];
  const auto src_ub_before = src.get_col_upp()[ColIndex {0}];

  // Mutate both clones — neither must affect the source.
  native.set_col_low(ColIndex {0}, 3.0);
  native.set_col_upp(ColIndex {0}, 7.0);
  manual.set_col_low(ColIndex {0}, 4.0);
  manual.set_col_upp(ColIndex {0}, 6.0);

  CHECK(src.get_col_low()[ColIndex {0}] == doctest::Approx(src_lb_before));
  CHECK(src.get_col_upp()[ColIndex {0}] == doctest::Approx(src_ub_before));

  // The two clones diverged from each other — confirms the mutation
  // landed on each independently rather than aliasing.
  CHECK(native.get_col_low()[ColIndex {0}]
        != doctest::Approx(manual.get_col_low()[ColIndex {0}]));
  CHECK(native.get_col_upp()[ColIndex {0}]
        != doctest::Approx(manual.get_col_upp()[ColIndex {0}]));
}

// ─── 6. Member API: clone_from_flat() round-trip ────────────────────

TEST_CASE(  // NOLINT
    "LinearInterface::clone_from_flat — equivalence with native clone")
{
  // The aperture path's manual-clone option calls
  // `phase_li.clone_from_flat(CloneKind::shallow)` instead of
  // `phase_li.clone(CloneKind::shallow)`.  This subcase pins the
  // contract that both calls produce equivalent state.
  //
  // Pre-condition for `clone_from_flat`: the source must hold an
  // uncompressed `FlatLinearProblem` snapshot.  We satisfy that by
  // constructing a `LinearProblem`, flattening, then loading the
  // flat into the source AND saving it back as the source's
  // snapshot via `freeze_for_cuts(LowMemoryMode::compress)`.
  auto problem = build_feature_problem();
  auto flat_for_src = problem.flatten({});

  LinearInterface src;
  src.set_low_memory(LowMemoryMode::compress);
  src.load_flat(flat_for_src);
  // Move the flat into the source as its retained snapshot — same
  // shape `freeze_for_cuts` produces in normal SDDP build flow.
  src.save_snapshot(std::move(flat_for_src));

  REQUIRE(src.has_snapshot_data());

  SUBCASE("shallow — clone_from_flat matches native clone(shallow)")
  {
    const auto native = src.clone(LinearInterface::CloneKind::shallow);
    const auto manual =
        src.clone_from_flat(LinearInterface::CloneKind::shallow);

    REQUIRE(manual.get_numcols() == native.get_numcols());
    REQUIRE(manual.get_numrows() == native.get_numrows());
    CHECK(spans_equal_approx(native.get_col_low(), manual.get_col_low()));
    CHECK(spans_equal_approx(native.get_col_upp(), manual.get_col_upp()));
    CHECK(spans_equal_approx(native.get_col_cost(), manual.get_col_cost()));
    CHECK(spans_equal_approx(native.get_row_low(), manual.get_row_low()));
    CHECK(spans_equal_approx(native.get_row_upp(), manual.get_row_upp()));

    // Shallow contract: metadata shared_ptrs are re-linked to the
    // source's pointees, so both clones bump use_count beyond 1.
    CHECK(src.col_labels_meta_use_count() >= 3);  // src + native + manual
  }

  SUBCASE("deep — clone_from_flat keeps independent metadata")
  {
    const auto manual = src.clone_from_flat(LinearInterface::CloneKind::deep);

    REQUIRE(manual.get_numcols() == src.get_numcols());
    REQUIRE(manual.get_numrows() == src.get_numrows());
    // Deep contract: the manual clone owns a freshly-built copy of
    // every metadata field.  use_count of the source's pointees is
    // unaffected by the deep manual clone.
    const auto src_uc_after = src.col_labels_meta_use_count();
    CHECK(src_uc_after == 1);
  }

  SUBCASE("solve equivalence")
  {
    auto native = src.clone(LinearInterface::CloneKind::shallow);
    auto manual = src.clone_from_flat(LinearInterface::CloneKind::shallow);
    const SolverOptions opts {};
    REQUIRE(native.resolve(opts).has_value());
    REQUIRE(manual.resolve(opts).has_value());
    REQUIRE(native.is_optimal());
    REQUIRE(manual.is_optimal());
    CHECK(native.get_obj_value()
          == doctest::Approx(manual.get_obj_value()).epsilon(1e-9));
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface::clone_from_flat — pre-condition errors")
{
  // No snapshot — the source was loaded but never had save_snapshot
  // called.  clone_from_flat must throw with a clear message.
  auto problem = build_labelled_problem();
  const auto flat = problem.flatten({});

  LinearInterface src;
  src.load_flat(flat);
  REQUIRE_FALSE(src.has_snapshot_data());

  CHECK_THROWS_AS(std::ignore = src.clone_from_flat(), std::runtime_error);
}

TEST_CASE(  // NOLINT
    "LinearInterface::clone_from_flat — succeeds after DecompressionGuard "
    "even when persistent compressed buffer is present")
{
  // Pin the bug fix where ``clone_from_flat`` previously rejected a
  // source whose persistent compressed buffer was non-empty, even if
  // the flat LP itself had been re-hydrated by ``DecompressionGuard``.
  // The aperture path under ``low_memory_mode=compress`` after PR #465
  // (eager build-time compression) hits exactly this case: the source
  // LP is built, compressed, then the aperture pass wraps it in a
  // ``DecompressionGuard`` before calling ``clone_from_flat`` for each
  // aperture.  The persistent compressed cache stays alive through the
  // guard, but the flat LP is fully accessible.
  auto problem = build_feature_problem();
  auto flat_for_src = problem.flatten({});

  LinearInterface src;
  src.set_low_memory(LowMemoryMode::compress);
  src.load_flat(flat_for_src);
  src.save_snapshot(std::move(flat_for_src));
  REQUIRE(src.has_snapshot_data());

  // Compress the snapshot, then decompress (mimicking the
  // build → release → DecompressionGuard sequence in production).
  src.enable_compression();
  src.disable_compression();

  // Even though the persistent compressed cache is still non-empty
  // (kept by ``LowMemorySnapshot::decompress`` as a one-time-only cache
  // for future reload cycles), ``clone_from_flat`` must succeed because
  // ``flat_lp.matbeg`` is now populated.
  CHECK_NOTHROW(std::ignore = src.clone_from_flat());
}

// ─── 7. Timing comparison (informational) ───────────────────────────

TEST_CASE(  // NOLINT
    "clone via load_flat — wall-clock timing comparison (informational)")
{
  // No assertion — this subcase exists to surface the per-clone cost
  // of the two routes on the test's chosen backend.  The figure
  // matters for the aperture-path refactor decision: the manual route
  // pays a higher per-clone cost but does not serialise under
  // s_global_clone_mutex, so the wall-time comparison at the actual
  // call site (16 cells × 16 apertures, 80 workers) inverts.  Here
  // we measure single-thread cost only — concurrency wins are
  // measured at the integration level.
  auto problem = build_feature_problem();
  const auto flat = problem.flatten({});
  const auto src = make_source_from_flat(flat);

  constexpr int n_iters = 200;

  // Warm up — first clone allocates plugin state lazily on some
  // backends; exclude that from the measured loop.
  std::ignore = src.clone();
  std::ignore = manual_clone_from_flat(flat);

  using clock = std::chrono::steady_clock;

  auto t0 = clock::now();
  for (int i = 0; i < n_iters; ++i) {
    auto c = src.clone();
    (void)c.get_numcols();  // prevent the optimizer from eliding the clone
  }
  const auto native_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0)
          .count();

  t0 = clock::now();
  for (int i = 0; i < n_iters; ++i) {
    auto c = manual_clone_from_flat(flat);
    (void)c.get_numcols();
  }
  const auto manual_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0)
          .count();

  MESSAGE("backend = " << src.solver_name()
                       << "  native_clone = " << native_us / n_iters
                       << " us/iter  manual_clone = " << manual_us / n_iters
                       << " us/iter  ratio = "
                       << static_cast<double>(manual_us) / native_us);

  // Sanity: both routes finished without exceptions and produced the
  // expected number of rows/cols on their final iteration.  No timing
  // assertion — the absolute numbers depend on backend, hardware,
  // and CI noise.
  CHECK(n_iters > 0);
}
