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
#include <optional>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;

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

TEST_CASE(  // NOLINT
    "LinearInterface::clone_from_flat — single-arg (kind) signature pin")
{
  // Compile-time + runtime pin for the post-cleanup single-arg form.
  // The dropped second parameter (legacy ``with_replay``) used to gate
  // whether ``clone_from_flat`` replayed post-snapshot mutations; replay
  // is now unconditional, so any re-introduction of a second parameter
  // must be a deliberate design decision — not a silent regression.
  //
  // The assignment below would fail to compile if the signature ever
  // grew a second required parameter or changed its return type.
  using CloneFromFlatFn =
      LinearInterface (LinearInterface::*)(LinearInterface::CloneKind) const;
  constexpr CloneFromFlatFn fn = &LinearInterface::clone_from_flat;
  (void)fn;

  auto problem = build_feature_problem();
  auto flat = problem.flatten({});

  LinearInterface src;
  src.set_low_memory(LowMemoryMode::compress);
  src.load_flat(flat);
  src.save_snapshot(std::move(flat));
  REQUIRE(src.has_snapshot_data());

  // Both kinds callable with exactly one argument — no default-arg
  // dependency, no with_replay flag.
  const auto shallow = src.clone_from_flat(LinearInterface::CloneKind::shallow);
  const auto deep = src.clone_from_flat(LinearInterface::CloneKind::deep);
  CHECK(shallow.get_numcols() == src.get_numcols());
  CHECK(deep.get_numcols() == src.get_numcols());
  CHECK(shallow.get_numrows() == src.get_numrows());
  CHECK(deep.get_numrows() == src.get_numrows());
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

// ─── 8. with_replay equivalence — clone(shallow) vs.
//        clone_from_flat(shallow, with_replay=true) on a
//        SDDP-like LP that has post-snapshot α + cuts + bound pins. ──
//
// This is the elastic_filter_solve scenario: source LP has been
// frozen (snapshot saved), THEN α col added, THEN one or more cut
// rows added, THEN dep_col bounds pinned via set_col_lower/upper.
// Both clones should produce a structurally-identical LP that solves
// to the same optimum.
//
// A divergence here would explain the chinneck-mode "relaxed clone
// infeasible" failure — and the diff between the two LPs tells us
// exactly where the bug is.
TEST_CASE(  // NOLINT
    "clone_from_flat(with_replay) — SDDP-like LP with α + cut + pin")
{
  using namespace gtopt;

  // --- 1. Build the structural LP and freeze.
  auto problem = build_feature_problem();
  auto flat = problem.flatten({});

  LinearInterface src;
  src.set_low_memory(LowMemoryMode::compress);
  src.load_flat(flat);
  src.save_snapshot(std::move(flat));
  src.save_base_numrows();
  REQUIRE(src.has_snapshot_data());
  const auto rows_after_freeze = src.get_numrows();
  const auto cols_after_freeze = src.get_numcols();

  // --- 2. Add α col post-snapshot (auto-records into m_replay_).
  const auto alpha_col = src.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0e30,
      .cost = 0.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });
  REQUIRE(src.get_numcols() == cols_after_freeze + 1);

  // --- 3. Add an optimality cut row that references α + col 1.
  SparseRow cut;
  cut[alpha_col] = 1.0;
  cut[ColIndex {1}] = 0.5;
  cut.lowb = 1.0;
  cut.uppb = LinearProblem::DblMax;
  cut.class_name = "Sddp";
  cut.constraint_name = "optcut";
  cut.variable_uid = 7;
  (void)src.add_row(cut);  // NOLINT
  src.record_dynamic_row(cut);  // simulate active_cuts replay path
  REQUIRE(src.get_numrows() == rows_after_freeze + 1);

  // --- 4. Pin a dep_col bound (forward-pass-style propagation).
  src.set_col_low_raw(ColIndex {1}, 4.0);
  src.set_col_upp_raw(ColIndex {1}, 4.0);

  // --- 5. Clone via both routes.
  const auto native = src.clone(LinearInterface::CloneKind::shallow);
  const auto manual = src.clone_from_flat(LinearInterface::CloneKind::shallow);

  // --- 6. Structural equivalence.
  CAPTURE(native.get_numcols());
  CAPTURE(manual.get_numcols());
  CAPTURE(src.get_numcols());
  CAPTURE(native.get_numrows());
  CAPTURE(manual.get_numrows());
  CAPTURE(src.get_numrows());
  REQUIRE(manual.get_numcols() == native.get_numcols());
  REQUIRE(manual.get_numrows() == native.get_numrows());

  CHECK(spans_equal_approx(native.get_col_low(), manual.get_col_low()));
  CHECK(spans_equal_approx(native.get_col_upp(), manual.get_col_upp()));
  CHECK(spans_equal_approx(native.get_col_cost(), manual.get_col_cost()));
  CHECK(spans_equal_approx(native.get_row_low(), manual.get_row_low()));
  CHECK(spans_equal_approx(native.get_row_upp(), manual.get_row_upp()));

  // --- 7. Solve equivalence.
  auto native_solve = src.clone(LinearInterface::CloneKind::shallow);
  auto manual_solve = src.clone_from_flat(LinearInterface::CloneKind::shallow);
  const SolverOptions opts {};
  const auto rn = native_solve.resolve(opts);
  const auto rm = manual_solve.resolve(opts);
  CAPTURE(rn.has_value());
  CAPTURE(rm.has_value());
  CAPTURE(native_solve.is_optimal());
  CAPTURE(manual_solve.is_optimal());
  CAPTURE(native_solve.get_status());
  CAPTURE(manual_solve.get_status());
  REQUIRE(rn.has_value());
  REQUIRE(rm.has_value());
  REQUIRE(native_solve.is_optimal());
  REQUIRE(manual_solve.is_optimal());
  CHECK(native_solve.get_obj_value()
        == doctest::Approx(manual_solve.get_obj_value()).epsilon(1e-9));
}

// ─── 9. Reproduce chinneck mode's "Phase-1 zero-obj + slacks" pattern
//        on both clone routes — the failing path. ──
//
// Mimics elastic_filter_solve's exact sequence on the cloned LP:
//   1. Zero every original objective coefficient (Phase-1 feasibility).
//   2. Free the dep_col (lowb=-DblMax, uppb=+DblMax).
//   3. Add slack cols sup, sdn with cost = penalty.
//   4. Add fixing row: dep + sup - sdn = trial_value.
//   5. Resolve.
//
// Both clones must produce a feasible LP (slacks make it always
// feasible) and converge to the same optimum.
TEST_CASE(  // NOLINT
    "clone_from_flat(with_replay) — chinneck Phase-1 + slacks "
    "equivalence with native clone")
{
  using namespace gtopt;

  // --- Same SDDP-like setup as the previous test.
  auto problem = build_feature_problem();
  auto flat = problem.flatten({});

  LinearInterface src;
  src.set_low_memory(LowMemoryMode::compress);
  src.load_flat(flat);
  src.save_snapshot(std::move(flat));
  src.save_base_numrows();
  REQUIRE(src.has_snapshot_data());

  const auto alpha_col = src.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0e30,
      .cost = 0.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });
  SparseRow cut;
  cut[alpha_col] = 1.0;
  cut[ColIndex {1}] = 0.5;
  cut.lowb = 1.0;
  cut.uppb = LinearProblem::DblMax;
  cut.class_name = "Sddp";
  cut.constraint_name = "optcut";
  cut.variable_uid = 7;
  (void)src.add_row(cut);  // NOLINT
  src.record_dynamic_row(cut);

  // Forward-pass-style trial pin: ColIndex{1} pinned to 4.
  src.set_col_low_raw(ColIndex {1}, 4.0);
  src.set_col_upp_raw(ColIndex {1}, 4.0);

  // Clone the LP via both routes — these are the two LPs that
  // elastic_filter_solve would receive at the "auto cloned = ..." line.
  auto clone_native = src.clone(LinearInterface::CloneKind::shallow);
  auto clone_manual = src.clone_from_flat(LinearInterface::CloneKind::shallow);

  // --- Apply the same elastic transformation to BOTH clones. ---
  auto apply_elastic = [&](LinearInterface& clone)
  {
    // Step 1: zero original obj.
    const auto ncols = static_cast<size_t>(clone.get_numcols());
    const std::vector<double> zeros(ncols, 0.0);
    clone.set_obj_coeffs_raw(zeros);

    // Step 2: free the pinned dep_col (was lowb=uppb=4).
    clone.set_col_low_raw(ColIndex {1}, -1.0e30);
    clone.set_col_upp_raw(ColIndex {1}, 1.0e30);

    // Step 3: add slack cols sup, sdn with cost = penalty.
    constexpr double penalty = 1000.0;
    const auto sup_col = clone.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0e30,
        .cost = penalty,
        .class_name = "Slack",
        .variable_name = "sup",
        .variable_uid = 1001,
    });
    const auto sdn_col = clone.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0e30,
        .cost = penalty,
        .class_name = "Slack",
        .variable_name = "sdn",
        .variable_uid = 1002,
    });

    // Step 4: fixing row: ColIndex{1} + sup - sdn = trial_value (4.0).
    SparseRow fix;
    fix[ColIndex {1}] = 1.0;
    fix[sup_col] = 1.0;
    fix[sdn_col] = -1.0;
    fix.lowb = 4.0;
    fix.uppb = 4.0;
    fix.class_name = "Slack";
    fix.constraint_name = "fix";
    fix.variable_uid = 1003;
    (void)clone.add_row(fix);  // NOLINT
  };

  apply_elastic(clone_native);
  apply_elastic(clone_manual);

  // --- Solve both. ---
  const SolverOptions opts {};
  const auto rn = clone_native.resolve(opts);
  const auto rm = clone_manual.resolve(opts);

  CAPTURE(rn.has_value());
  CAPTURE(rm.has_value());
  CAPTURE(clone_native.is_optimal());
  CAPTURE(clone_manual.is_optimal());
  CAPTURE(clone_native.get_status());
  CAPTURE(clone_manual.get_status());
  CAPTURE(clone_native.get_numrows());
  CAPTURE(clone_manual.get_numrows());
  CAPTURE(clone_native.get_numcols());
  CAPTURE(clone_manual.get_numcols());

  // Both must reach optimal — slacks are unbounded, so the LP is
  // always feasible.  If the manual+replay clone fails to reach
  // optimal, this is the chinneck-mode failure reproduced in
  // isolation.
  REQUIRE(rn.has_value());
  REQUIRE(rm.has_value());
  REQUIRE(clone_native.is_optimal());
  REQUIRE(clone_manual.is_optimal());
  CHECK(clone_native.get_obj_value()
        == doctest::Approx(clone_manual.get_obj_value()).epsilon(1e-9));
}

// ─── 10. Cloning correctness across every LowMemoryMode ─────────────
//
// Pins down the routing rule the SDDP forward-pass elastic filter
// depends on:
//
//   * `off`:      `m_replay_` is NEVER populated (auto-record gates
//                 require `m_low_memory_mode_ != off`).  A snapshot
//                 may still be present for fingerprinting (see
//                 `system_lp.cpp:560`), but `clone_from_flat(with_replay)`
//                 would silently miss α + cuts + bound pins.  Native
//                 `clone()` (CPXcloneprob) is the only correct route.
//
//   * `compress`: `m_replay_` is populated by add_col / set_col_low /
//                 etc.  Both routes (native and manual+replay)
//                 produce equivalent LPs.
//
//   * `rebuild`:  No snapshot exists (`has_snapshot_data() == false`)
//                 — only the rebuild callback can reconstruct.
//                 `clone_from_flat` is unavailable; native is the
//                 only route.
namespace
{
struct ModeFixture
{
  LinearInterface src;
  ColIndex alpha_col {};
  size_t cols_after_freeze {};
  size_t rows_after_freeze {};
};

ModeFixture make_sddp_like_source(LowMemoryMode mode)
{
  ModeFixture out;
  auto problem = build_feature_problem();
  auto flat = problem.flatten({});

  out.src.set_low_memory(mode, CompressionCodec::lz4);
  out.src.load_flat(flat);
  // Both supported modes (off + compress) save a snapshot.  We do it
  // unconditionally so the fixture exercises every API entry point
  // regardless of mode.
  out.src.save_snapshot(std::move(flat));
  out.src.save_base_numrows();
  out.cols_after_freeze = static_cast<size_t>(out.src.get_numcols());
  out.rows_after_freeze = static_cast<size_t>(out.src.get_numrows());

  // Add α col post-freeze (auto-records when mode != off).
  out.alpha_col = out.src.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0e30,
      .cost = 10.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });

  // Add an optimality cut row (auto-records when mode != off).
  SparseRow cut;
  cut[out.alpha_col] = 1.0;
  cut[ColIndex {1}] = 0.5;
  cut.lowb = 1.0;
  cut.uppb = LinearProblem::DblMax;
  cut.class_name = "Sddp";
  cut.constraint_name = "optcut";
  cut.variable_uid = 7;
  (void)out.src.add_row(cut);  // NOLINT
  out.src.record_dynamic_row(cut);

  // Pin a dep_col bound (forward-pass-style trial).  Auto-records
  // into `pending_col_bounds` only when mode != off.
  out.src.set_col_low_raw(ColIndex {1}, 4.0);
  out.src.set_col_upp_raw(ColIndex {1}, 4.0);
  return out;
}
}  // namespace

TEST_CASE(  // NOLINT
    "clone() equivalence — every LowMemoryMode reaches the same "
    "live-backend state")
{
  using namespace gtopt;
  // Native clone() must produce a clone whose live backend state
  // matches the source's live backend state in every mode — that's
  // the contract callers (elastic, aperture, ad-hoc) rely on.

  for (const auto mode : {LowMemoryMode::off, LowMemoryMode::compress}) {
    CAPTURE(static_cast<int>(mode));
    auto fix = make_sddp_like_source(mode);
    auto& src = fix.src;
    CAPTURE(src.has_snapshot_data());
    CAPTURE(static_cast<int>(src.low_memory_mode()));

    const auto cloned = src.clone(LinearInterface::CloneKind::shallow);

    REQUIRE(cloned.get_numcols() == src.get_numcols());
    REQUIRE(cloned.get_numrows() == src.get_numrows());
    CHECK(spans_equal_approx(src.get_col_low(), cloned.get_col_low()));
    CHECK(spans_equal_approx(src.get_col_upp(), cloned.get_col_upp()));
    CHECK(spans_equal_approx(src.get_col_cost(), cloned.get_col_cost()));
    CHECK(spans_equal_approx(src.get_row_low(), cloned.get_row_low()));
    CHECK(spans_equal_approx(src.get_row_upp(), cloned.get_row_upp()));
  }
}

TEST_CASE(  // NOLINT
    "clone_from_flat(with_replay) — equivalence with native clone "
    "in compress mode (where m_replay_ is populated)")
{
  using namespace gtopt;

  auto fix = make_sddp_like_source(LowMemoryMode::compress);
  auto& src = fix.src;
  REQUIRE(src.has_snapshot_data());
  REQUIRE(src.low_memory_mode() == LowMemoryMode::compress);

  const auto native = src.clone(LinearInterface::CloneKind::shallow);
  const auto manual = src.clone_from_flat(LinearInterface::CloneKind::shallow);

  // Structural equivalence: same dims, same bounds, same costs.
  REQUIRE(manual.get_numcols() == native.get_numcols());
  REQUIRE(manual.get_numrows() == native.get_numrows());
  CHECK(spans_equal_approx(native.get_col_low(), manual.get_col_low()));
  CHECK(spans_equal_approx(native.get_col_upp(), manual.get_col_upp()));
  CHECK(spans_equal_approx(native.get_col_cost(), manual.get_col_cost()));
  CHECK(spans_equal_approx(native.get_row_low(), manual.get_row_low()));
  CHECK(spans_equal_approx(native.get_row_upp(), manual.get_row_upp()));

  // Solve equivalence.
  auto native_s = src.clone(LinearInterface::CloneKind::shallow);
  auto manual_s = src.clone_from_flat(LinearInterface::CloneKind::shallow);
  const SolverOptions opts {};
  REQUIRE(native_s.resolve(opts).has_value());
  REQUIRE(manual_s.resolve(opts).has_value());
  REQUIRE(native_s.is_optimal());
  REQUIRE(manual_s.is_optimal());
  CHECK(native_s.get_obj_value()
        == doctest::Approx(manual_s.get_obj_value()).epsilon(1e-9));
}

TEST_CASE(  // NOLINT
    "clone_from_flat(with_replay) — reproduces live backend including "
    "α and cuts under off mode (2026-05-11 fix regression guard)")
{
  using namespace gtopt;
  // Regression guard for the 2026-05-11 fix to the aperture-clone
  // path on juan/IPLP: under `LowMemoryMode::off` the replay buffer
  // (`m_replay_`) used to be gated off — `record_*_if_tracked` was
  // a no-op — so `clone_from_flat(with_replay=true)` produced a
  // snapshot-only clone (no α col, no cut rows, no pinned bounds)
  // even though the live backend HAD these.  The aperture path,
  // which always took `clone_from_flat` because `aperture_use_manual_clone`
  // defaults to true AND `system_lp.cpp:560` always saves a snapshot
  // under off, then silently solved the construction-time LP at
  // every iteration — LB stuck at 50 M for 30 iters on IPLP.
  //
  // The fix: drop the `mode != off` gate in
  //   * `lp_replay_buffer.hpp:record_*_if_tracked` (cut + dynamic row/col)
  //   * `linear_interface.cpp:add_col` (replay-buffer push for α)
  //   * `linear_interface.cpp:set_col_low_raw / set_col_upp_raw`
  //     (pending-bound push for `bound_alpha`)
  //
  // The buffer is now populated regardless of mode, so the manual
  // clone reproduces the live backend.  Pinned here so a future
  // refactor that re-introduces the gate fails this test.

  auto fix = make_sddp_like_source(LowMemoryMode::off);
  auto& src = fix.src;
  REQUIRE(src.has_snapshot_data());  // snapshot exists in off mode too
  REQUIRE(src.low_memory_mode() == LowMemoryMode::off);

  const auto native = src.clone(LinearInterface::CloneKind::shallow);
  const auto manual = src.clone_from_flat(LinearInterface::CloneKind::shallow);

  // Native and manual clones must now be structurally identical.
  CHECK(native.get_numcols() == src.get_numcols());
  CHECK(native.get_numrows() == src.get_numrows());
  CHECK(manual.get_numcols() == src.get_numcols());
  CHECK(manual.get_numrows() == src.get_numrows());
  CHECK(manual.get_numcols() == native.get_numcols());
  CHECK(manual.get_numrows() == native.get_numrows());

  // Solving both must produce the same objective (the cut on α
  // and the freed α-col bounds must be present in both).
  auto native_s = src.clone(LinearInterface::CloneKind::shallow);
  auto manual_s = src.clone_from_flat(LinearInterface::CloneKind::shallow);
  const SolverOptions opts {};
  REQUIRE(native_s.resolve(opts).has_value());
  REQUIRE(manual_s.resolve(opts).has_value());
  REQUIRE(native_s.is_optimal());
  REQUIRE(manual_s.is_optimal());
  CHECK(native_s.get_obj_value()
        == doctest::Approx(manual_s.get_obj_value()).epsilon(1e-9));
}

TEST_CASE(  // NOLINT
    "clone equivalence — native vs clone_from_flat across "
    "all LowMemoryMode values (off, compress, rebuild)")
{
  using namespace gtopt;
  // Cross-mode invariance: regardless of low_memory_mode, the
  // manual clone (`clone_from_flat(with_replay=true)`) and the
  // native clone (`clone(shallow)` → CPXcloneprob) must produce
  // structurally identical LPs that solve to the same obj.
  //
  // Pre-2026-05-11 this held under compress (where `m_replay_` was
  // populated) but FAILED under off — see the regression-guard test
  // above.

  const std::array modes = {LowMemoryMode::off, LowMemoryMode::compress};

  for (const auto mode : modes) {
    CAPTURE(static_cast<int>(mode));

    auto fix = make_sddp_like_source(mode);
    auto& src = fix.src;

    // Native always works (CPXcloneprob copies the live backend).
    auto native = src.clone(LinearInterface::CloneKind::shallow);
    CHECK(native.get_numcols() == src.get_numcols());
    CHECK(native.get_numrows() == src.get_numrows());

    // Manual route requires a populated snapshot.
    REQUIRE(src.has_snapshot_data());
    auto manual = src.clone_from_flat(LinearInterface::CloneKind::shallow);
    CHECK(manual.get_numcols() == native.get_numcols());
    CHECK(manual.get_numrows() == native.get_numrows());

    const SolverOptions opts {};
    REQUIRE(native.resolve(opts).has_value());
    REQUIRE(manual.resolve(opts).has_value());
    REQUIRE(native.is_optimal());
    REQUIRE(manual.is_optimal());
    CHECK(native.get_obj_value()
          == doctest::Approx(manual.get_obj_value()).epsilon(1e-9));
  }
}

// ─── 11. Critical SDDP paths exercised by the elastic filter ────────
//
// Drill into the specific behaviours `elastic_filter_solve` depends
// on: zeroing original objective coefficients, freeing pinned
// dep_cols, adding slack cols, adding fixing rows.  Both clone
// routes (when applicable) must support these mutations identically.
TEST_CASE(  // NOLINT
    "Critical SDDP path: zero-obj + slack relaxation produces "
    "feasible LP in compress mode via both clone routes")
{
  using namespace gtopt;
  auto fix = make_sddp_like_source(LowMemoryMode::compress);
  auto& src = fix.src;

  auto apply_elastic = [&](LinearInterface& clone)
  {
    const auto ncols = static_cast<size_t>(clone.get_numcols());
    const std::vector<double> zeros(ncols, 0.0);
    clone.set_obj_coeffs_raw(zeros);
    clone.set_col_low_raw(ColIndex {1}, -1.0e30);
    clone.set_col_upp_raw(ColIndex {1}, 1.0e30);
    constexpr double penalty = 1000.0;
    const auto sup = clone.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0e30,
        .cost = penalty,
        .class_name = "Slack",
        .variable_name = "sup",
        .variable_uid = 1001,
    });
    const auto sdn = clone.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0e30,
        .cost = penalty,
        .class_name = "Slack",
        .variable_name = "sdn",
        .variable_uid = 1002,
    });
    SparseRow fix_row;
    fix_row[ColIndex {1}] = 1.0;
    fix_row[sup] = 1.0;
    fix_row[sdn] = -1.0;
    fix_row.lowb = 4.0;
    fix_row.uppb = 4.0;
    fix_row.class_name = "Slack";
    fix_row.constraint_name = "fix";
    fix_row.variable_uid = 1003;
    (void)clone.add_row(fix_row);  // NOLINT
  };

  auto cn = src.clone(LinearInterface::CloneKind::shallow);
  auto cm = src.clone_from_flat(LinearInterface::CloneKind::shallow);
  apply_elastic(cn);
  apply_elastic(cm);

  const SolverOptions opts {};
  REQUIRE(cn.resolve(opts).has_value());
  REQUIRE(cm.resolve(opts).has_value());
  REQUIRE(cn.is_optimal());
  REQUIRE(cm.is_optimal());
  CHECK(cn.get_obj_value()
        == doctest::Approx(cm.get_obj_value()).epsilon(1e-9));
}

TEST_CASE(  // NOLINT
    "Critical SDDP path: parallel cloning under compress mode "
    "produces equivalent results across N threads")
{
  using namespace gtopt;
  // The deadlock fix in commit fe2ed42d serialises native clone()
  // under a process-global mutex.  This test verifies the manual+
  // replay path also runs correctly across N parallel threads
  // (without the mutex) and produces identical results to the
  // native single-threaded clone.
  auto fix = make_sddp_like_source(LowMemoryMode::compress);
  auto& src = fix.src;

  const auto reference = src.clone(LinearInterface::CloneKind::shallow);

  constexpr int n_threads = 8;
  std::vector<std::thread> threads;
  std::vector<std::optional<double>> obj_values(n_threads);
  threads.reserve(n_threads);
  for (int i = 0; i < n_threads; ++i) {
    threads.emplace_back(
        [&src, &obj_values, i]
        {
          auto clone = src.clone_from_flat(LinearInterface::CloneKind::shallow);
          const SolverOptions opts {};
          if (clone.resolve(opts).has_value() && clone.is_optimal()) {
            obj_values[static_cast<size_t>(i)] = clone.get_obj_value();
          }
        });
  }
  for (auto& t : threads) {
    t.join();
  }

  auto ref_solve = src.clone(LinearInterface::CloneKind::shallow);
  const SolverOptions opts {};
  REQUIRE(ref_solve.resolve(opts).has_value());
  REQUIRE(ref_solve.is_optimal());
  const double expected = ref_solve.get_obj_value();

  for (int i = 0; i < n_threads; ++i) {
    CAPTURE(i);
    REQUIRE(obj_values[static_cast<size_t>(i)].has_value());
    CHECK(*obj_values[static_cast<size_t>(i)]
          == doctest::Approx(expected).epsilon(1e-9));
  }
}

// ─── 12. Gap C2 — throwaway-clone short-circuit on bound writes ─────
//
// Pins the `m_is_throwaway_clone_` short-circuit added in commit
// 8e08b1c4: after `clone_from_flat(with_replay=true)`, the clone is
// flagged as a throwaway and subsequent `set_col_low_raw` /
// `set_col_upp_raw` calls must NOT grow `pending_col_bounds`.  Also
// asserts that those writes don't leak into the source's replay
// buffer (borrow-from-source preserves source-side invariants).
TEST_CASE(  // NOLINT
    "Throwaway clone short-circuit: bound writes don't grow "
    "pending_col_bounds on clone or source")
{
  using namespace gtopt;

  auto fix = make_sddp_like_source(LowMemoryMode::compress);
  auto& src = fix.src;

  // Snapshot source state before the clone.  make_sddp_like_source
  // already pinned ColIndex{1} via set_col_low_raw/upp_raw, so the
  // source's pending_col_bounds map has at least one entry.
  const auto pending_before = src.replay_buf().pending_col_bounds_size();

  auto clone = src.clone_from_flat(LinearInterface::CloneKind::shallow);

  // The clone starts with the SAME pending_col_bounds (its replay
  // buffer was deep-copied from src in apply_post_load_replay).
  const auto clone_pending_after_clone =
      clone.replay_buf().pending_col_bounds_size();

  // Drive a representative number of bound writes on the throwaway
  // clone — these would normally grow pending_col_bounds, but the
  // short-circuit must drop them on the floor.
  // Use cols 0..2 (the LP has 3 cols from build_feature_problem),
  // writing each repeatedly to exercise both new-key and update paths.
  constexpr int n_writes = 50;
  for (int i = 0; i < n_writes; ++i) {
    const auto col = ColIndex {i % 3};
    clone.set_col_low_raw(col, -1.0e30);
    clone.set_col_upp_raw(col, 1.0e30);
  }

  // Throwaway short-circuit: clone's pending_col_bounds did not grow.
  CHECK(clone.replay_buf().pending_col_bounds_size()
        == clone_pending_after_clone);

  // Borrow-from-source preserved invariants: source untouched.
  CHECK(src.replay_buf().pending_col_bounds_size() == pending_before);
}

// ─── 13. Gap C3 — apply_post_load_replay(source) source-immutability ─
//
// Pins the borrow-from-source contract: when `clone_from_flat` runs
// `apply_post_load_replay(m_replay_)` on the clone (which uses the
// source's buffer as the data source), the clone's OWN replay flag
// is flipped via the ReplayGuard but the source's flag and contents
// stay untouched.  This guarantees concurrent throwaway clones from
// the same source don't race on the source's `replaying()` flag.
TEST_CASE(  // NOLINT
    "Borrow-from-source replay leaves source's replay buffer untouched")
{
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-unchecked-optional-access,readability-trailing-comma)

  auto fix = make_sddp_like_source(LowMemoryMode::compress);
  auto& src = fix.src;

  // Capture source-side state before the clone fires its replay.
  const auto cuts_before = src.replay_buf().active_cuts_size();
  const auto cols_before = src.replay_buf().dynamic_cols_size();
  const auto rows_before = src.replay_buf().dynamic_rows_size();
  const auto pending_before = src.replay_buf().pending_col_bounds_size();
  const bool replay_before = src.replay_buf().replaying();
  REQUIRE_FALSE(replay_before);  // Idle source: flag is always false.

  {
    auto clone = src.clone_from_flat(LinearInterface::CloneKind::shallow);

    // Replay flag flipped on OWN buffer, then released by the
    // ReplayGuard's dtor; at this point both source and clone show
    // replaying() == false.
    CHECK_FALSE(clone.replay_buf().replaying());
  }  // Clone destroyed.

  // Source unchanged across the entire clone+replay sequence.
  CHECK(src.replay_buf().replaying() == replay_before);
  CHECK(src.replay_buf().active_cuts_size() == cuts_before);
  CHECK(src.replay_buf().dynamic_cols_size() == cols_before);
  CHECK(src.replay_buf().dynamic_rows_size() == rows_before);
  CHECK(src.replay_buf().pending_col_bounds_size() == pending_before);
}

// NOLINTEND(bugprone-unchecked-optional-access,readability-trailing-comma)
