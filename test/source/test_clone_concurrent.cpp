// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_clone_concurrent.cpp
 * @brief     Multi-threaded torture test for shallow clones — mirrors
 *            the SDDP aperture pattern.
 *
 * Spawns N worker threads.  Each thread loops M times: take a
 * shallow clone of a shared source LP, mutate the clone's bounds and
 * objective, solve the clone, verify the objective matches the
 * expected value for the chosen bounds.
 *
 * Catches:
 *   - Data races in the shared metadata read path that single-threaded
 *     tests would miss.
 *   - Cross-clone state leakage (one clone's bound mutation leaking
 *     into another clone).
 *   - Backend cloning thread-safety regressions if the global
 *     `s_global_clone_mutex` (sddp_aperture.cpp:194) is bypassed by
 *     a future change.
 *
 * NOTE: this test does NOT lock around `clone()` itself — the project
 * convention is that the caller serialises backend clones, not the
 * LinearInterface API.  Here we lock manually with a `std::mutex`
 * around the `clone()` call (mirroring the aperture pattern).
 */

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Tiny labelled LP, identical shape to the source LPs in the
// aperture path: a few production cols + one row.  Distinct from
// other test files' helpers via the unique function name.
LinearInterface make_concurrent_test_lp()
{
  LinearInterface li;
  std::ignore = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 1,
  });
  std::ignore = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 2,
  });
  SparseRow r {
      .lowb = 0.0,
      .uppb = 50.0,  // sum constraint: p1 + p2 ≤ 50
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = 1,
  };
  r[ColIndex {0}] = 1.0;
  r[ColIndex {1}] = 1.0;
  std::ignore = li.add_row(r);
  return li;
}

}  // namespace

TEST_CASE("Many shallow clones solve concurrently without cross-talk")
{
  // The source LP is a tiny min-cost LP: minimize p1 + 2*p2 subject
  // to p1 + p2 ≤ 50, p1,p2 ≥ 0.  Optimum without further bounds:
  // p1 = 0, p2 = 0, obj = 0 (cost is positive, so push to zero).
  // We tighten p1's lower bound on each clone to force a non-trivial
  // optimum: p1 = lb, p2 = 0, obj = lb.
  auto src = make_concurrent_test_lp();

  constexpr int n_threads = 8;
  constexpr int iters_per_thread = 50;

  std::mutex clone_mutex;  // mirrors sddp_aperture.cpp:194 contract
  std::atomic<int> failures {0};

  auto worker = [&](int thread_id)
  {
    for (int i = 0; i < iters_per_thread; ++i) {
      // The lower bound is unique per (thread, iteration) so a stale
      // shared backend would visibly produce the wrong objective.
      // Wrap into [1, 49] so the LP stays feasible (row UB is 50).
      const double lb =
          1.0 + static_cast<double>(((thread_id * iters_per_thread) + i) % 49);

      // Serialise backend clone (project convention).
      auto cloned = [&]
      {
        const std::scoped_lock lock(clone_mutex);
        return src.clone(LinearInterface::CloneKind::shallow);
      }();

      cloned.set_col_low(ColIndex {0}, lb);

      const auto solver_opts = SolverOptions {};
      const auto result = cloned.resolve(solver_opts);
      if (!result.has_value() || !cloned.is_optimal()) {
        ++failures;
        continue;
      }
      const double expected = lb;  // p1 = lb, p2 = 0
      const double got = cloned.get_obj_value();
      if (std::abs(got - expected) > 1e-6) {
        ++failures;
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(n_threads);
  for (int t = 0; t < n_threads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto& th : threads) {
    th.join();
  }

  CHECK(failures.load() == 0);
  // After all clones destroyed, source's use_count must drop back to 1.
  CHECK(src.col_labels_meta_use_count() == 1);
  CHECK(src.col_meta_index_use_count() == 1);
}

TEST_CASE("Concurrent disposable adds on independent shallow clones")
{
  // Each thread takes its own shallow clone, calls add_col_disposable
  // with a thread-unique uid, solves, verifies the new col landed
  // (numcols increased).  Verifies that per-clone-local extras are
  // properly isolated under concurrency.
  auto src = make_concurrent_test_lp();

  constexpr int n_threads = 8;
  std::mutex clone_mutex;
  std::atomic<int> failures {0};

  auto worker = [&](int thread_id)
  {
    auto cloned = [&]
    {
      const std::scoped_lock lock(clone_mutex);
      return src.clone(LinearInterface::CloneKind::shallow);
    }();

    const auto initial_cols = cloned.get_numcols();
    std::ignore = cloned.add_col_disposable(SparseCol {
        .uppb = 1.0,
        .class_name = "Disp",
        .variable_name = "x",
        .variable_uid = static_cast<Uid>(1000 + thread_id),
    });
    if (cloned.get_numcols() != initial_cols + 1) {
      ++failures;
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(n_threads);
  for (int t = 0; t < n_threads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto& th : threads) {
    th.join();
  }

  CHECK(failures.load() == 0);
  // Source's col count must be unchanged — disposables stay on the
  // clone only, never touch the source.
  CHECK(src.get_numcols() == 2);
  CHECK(src.col_labels_meta_use_count() == 1);
}
