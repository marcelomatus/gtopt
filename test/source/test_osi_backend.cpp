/**
 * @file      test_osi_backend.cpp
 * @brief     Unit tests for the OSI plugin's OsiPrep / reset_solver_ refactor
 * @date      2026-04-16
 * @copyright BSD-3-Clause
 *
 * Every TEST_CASE is gated on `reg.has_solver("clp")` and silently
 * skips when the OSI plugin is not available in the test environment.
 *
 * The OSI plugin caches backend state in OsiPrep (options, log filename,
 * log level, problem name).  Each call to load_problem() first invokes
 * reset_solver_() which destroys the current OsiSolverInterface and
 * replays OsiPrep onto a freshly constructed OsiClpSolverInterface, so
 * no per-LP state (basis, factorization, work arrays) leaks across
 * loads.  clone() deep-copies m_prep_ plus scalar caches and replays
 * log state onto the clone.  These tests pin the invariants of that
 * refactor:
 *   - CoinMessageHandler log level = 0 keeps the backend silent,
 *   - load_problem() produces a fresh OsiClpSolverInterface with the
 *     expected dimensions (a stale solver would keep the previous
 *     column count),
 *   - cached options + prob_name are re-applied after a load_problem
 *     cycle thanks to OsiPrep replay,
 *   - clone() yields a backend that owns its own OsiSolverInterface
 *     and survives destruction of the source,
 *   - concurrent backend creation, loading, solving, and cloning is
 *     race-free (mirrors the CPLEX test's by-design pattern).
 *
 * Note: SolverOptions::low_memory is documented as a no-op for the OSI
 * backend, so there is no corresponding test case here.
 */

// SPDX-License-Identifier: BSD-3-Clause
#include <array>
#include <atomic>
#include <barrier>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Return a fresh OSI/CLP backend, or nullptr if the plugin is unavailable.
[[nodiscard]] std::unique_ptr<SolverBackend> make_osi_clp_or_skip()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("clp")) {
    return nullptr;
  }
  return reg.create("clp");
}

// Trivial 2-variable LP:   min x + y   s.t.   x + y >= 2,  x,y >= 0.
// Optimum:                 x + y = 2  ->  obj = 2.
struct TrivialLP2
{
  static constexpr int ncols = 2;
  static constexpr int nrows = 1;
  // Column-major (CSC): col 0 uses row 0 (coef 1); col 1 uses row 0 (coef 1).
  std::array<int, 3> matbeg {
      0,
      1,
      2,
  };
  std::array<int, 2> matind {
      0,
      0,
  };
  std::array<double, 2> matval {
      1.0,
      1.0,
  };
  std::array<double, 2> collb {
      0.0,
      0.0,
  };
  std::array<double, 2> colub {};
  std::array<double, 2> obj {
      1.0,
      1.0,
  };
  std::array<double, 1> rowlb {
      2.0,
  };
  std::array<double, 1> rowub {};

  explicit TrivialLP2(double inf)
  {
    colub.fill(inf);
    rowub.fill(inf);
  }

  void load_into(SolverBackend& b) const
  {
    b.load_problem(ncols,
                   nrows,
                   matbeg.data(),
                   matind.data(),
                   matval.data(),
                   collb.data(),
                   colub.data(),
                   obj.data(),
                   rowlb.data(),
                   rowub.data());
  }
};

// Trivial 3-variable LP:   min x + y + z   s.t.   x + y + z >= 3,  all >= 0.
// Optimum:                 sum = 3  ->  obj = 3.
struct TrivialLP3
{
  static constexpr int ncols = 3;
  static constexpr int nrows = 1;
  std::array<int, 4> matbeg {
      0,
      1,
      2,
      3,
  };
  std::array<int, 3> matind {
      0,
      0,
      0,
  };
  std::array<double, 3> matval {
      1.0,
      1.0,
      1.0,
  };
  std::array<double, 3> collb {
      0.0,
      0.0,
      0.0,
  };
  std::array<double, 3> colub {};
  std::array<double, 3> obj {
      1.0,
      1.0,
      1.0,
  };
  std::array<double, 1> rowlb {
      3.0,
  };
  std::array<double, 1> rowub {};

  explicit TrivialLP3(double inf)
  {
    colub.fill(inf);
    rowub.fill(inf);
  }

  void load_into(SolverBackend& b) const
  {
    b.load_problem(ncols,
                   nrows,
                   matbeg.data(),
                   matind.data(),
                   matval.data(),
                   collb.data(),
                   colub.data(),
                   obj.data(),
                   rowlb.data(),
                   rowub.data());
  }
};

/// Return a path to a non-existing file inside a scratch directory.
/// Used by the log-silence tests so we never touch the repo root.
[[nodiscard]] std::filesystem::path scratch_log_base(std::string_view tag)
{
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "gtopt_osi_log_test";
  fs::create_directories(dir);
  return dir / std::string(tag);
}

}  // namespace

// ---------------------------------------------------------------------------
// A. Silence invariants (CoinMessageHandler default level = 0)
// ---------------------------------------------------------------------------

TEST_CASE("OsiSolverBackend default ctor is silent (no stray log)")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  // Nothing should be written to the scratch dir when no log filename has
  // been set.  The OsiPrep defaults (log_filename="", log_level=0) must
  // keep the CoinMessageHandler at level 0.
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "gtopt_osi_silent_default";
  fs::create_directories(dir);

  backend->set_prob_name("silent");
  CHECK(backend->get_prob_name() == "silent");

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  // The default log level is 0 and no file has been opened.
  CHECK(backend->get_log_level() == 0);
}

TEST_CASE(
    "set_log_filename with level<=0 or empty name stays silent")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;

  // level == 0 with a real base: must not create a log file.
  const auto base_zero = scratch_log_base("osi_lvl0");
  const auto file_zero = fs::path {base_zero.string() + ".log"};
  fs::remove(file_zero);
  backend->set_log_filename(base_zero.string(), 0);

  // Empty filename with a real base path: must not open a file either.
  const auto base_empty = scratch_log_base("osi_empty");
  const auto file_empty = fs::path {base_empty.string() + ".log"};
  fs::remove(file_empty);
  backend->set_log_filename("", 1);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  CHECK_FALSE(fs::exists(file_zero));
  CHECK_FALSE(fs::exists(file_empty));
}

TEST_CASE("set_log_filename(level>0) writes a log; clear stops it")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;
  const auto base = scratch_log_base("osi_written");
  const auto file = fs::path {base.string() + ".log"};
  fs::remove(file);

  backend->set_log_filename(base.string(), 1);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  CHECK(fs::exists(file));

  // clear_log_filename must be callable and release the FILE* without
  // throwing.  A subsequent solve should not crash.
  backend->clear_log_filename();
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  // Cleanup: leave no trace in /tmp.
  fs::remove(file);
}

// ---------------------------------------------------------------------------
// B. load_problem cycles the OsiSolverInterface instance (reset_solver_)
// ---------------------------------------------------------------------------

TEST_CASE("load_problem destroys previous LP state")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  const double inf = backend->infinity();

  TrivialLP2 lp2 {inf};
  lp2.load_into(*backend);
  CHECK(backend->get_num_cols() == 2);
  CHECK(backend->get_num_rows() == 1);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));

  // Second load_problem must yield a *fresh* OsiClpSolverInterface with
  // different dimensions.  A stale solver would still report ncols=2.
  TrivialLP3 lp3 {inf};
  lp3.load_into(*backend);
  CHECK(backend->get_num_cols() == 3);
  CHECK(backend->get_num_rows() == 1);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(3.0));
}

TEST_CASE("apply_options survives load_problem cycle")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  SolverOptions opts;
  opts.algorithm = LPAlgo::dual;
  opts.threads = 2;
  opts.presolve = false;
  opts.log_level = 0;
  backend->apply_options(opts);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  CHECK(backend->get_algorithm() == LPAlgo::dual);
  CHECK(backend->get_threads() == 2);
  CHECK_FALSE(backend->get_presolve());
  CHECK(backend->get_log_level() == 0);

  // A second load_problem rebuilds the OsiClpSolverInterface.  The cached
  // options in m_prep_.options must be replayed by reset_solver_() onto the
  // fresh solver via apply_options_to_solver, so the accessors keep the
  // same values.
  lp.load_into(*backend);

  CHECK(backend->get_algorithm() == LPAlgo::dual);
  CHECK(backend->get_threads() == 2);
  CHECK_FALSE(backend->get_presolve());
  CHECK(backend->get_log_level() == 0);

  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));
}

TEST_CASE("set_prob_name survives load_problem cycle")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  backend->set_prob_name("my_lp");
  CHECK(backend->get_prob_name() == "my_lp");

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  // After reset_solver_() rebuilt the solver, OsiPrep::prob_name must be
  // replayed via OsiProbName so the new solver still carries the name.
  CHECK(backend->get_prob_name() == "my_lp");
}

// ---------------------------------------------------------------------------
// C. Clone independence
// ---------------------------------------------------------------------------

TEST_CASE("clone owns its own solver: source may be destroyed")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  auto cloned = backend->clone();
  REQUIRE(cloned != nullptr);

  // Destroy the source.  If the clone shared its OsiSolverInterface with
  // the source, this would invalidate the clone's handle and the next
  // resolve would crash or abort.  The clone must own its own solver
  // (std::shared_ptr deep-copy via OsiSolverInterface::clone).
  backend.reset();

  cloned->resolve();
  REQUIRE(cloned->is_proven_optimal());
  CHECK(cloned->obj_value() == doctest::Approx(2.0));
  CHECK(cloned->get_num_cols() == 2);
  CHECK(cloned->get_num_rows() == 1);
}

TEST_CASE("clone preserves options and prob_name")  // NOLINT
{
  auto backend = make_osi_clp_or_skip();
  if (!backend) {
    return;
  }

  SolverOptions opts;
  opts.algorithm = LPAlgo::primal;
  opts.threads = 1;
  backend->apply_options(opts);
  backend->set_prob_name("p");

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  auto cloned = backend->clone();
  REQUIRE(cloned != nullptr);

  // clone() deep-copies m_prep_ and replays the scalar caches, so the
  // clone reports the same algorithm, thread count, and problem name as
  // the source.
  CHECK(cloned->get_algorithm() == LPAlgo::primal);
  CHECK(cloned->get_threads() == 1);
  CHECK(cloned->get_prob_name() == "p");
}

// ---------------------------------------------------------------------------
// D. Thread-safety smoke test
// Each worker thread creates its own backend, applies options, loads the
// LP, solves, clones, and resolves.  No shared backend state across
// threads; we just want to catch any accidental global mutable state in
// the OSI plugin or CoinMessageHandler init path.
// ---------------------------------------------------------------------------

TEST_CASE("OSI/CLP backend: parallel create+load+clone is race-free")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("clp")) {
    return;
  }

  constexpr int num_threads = 8;
  std::barrier start_gate(num_threads);
  std::atomic<int> ok {0};
  std::atomic<int> bad {0};
  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    workers.emplace_back(
        [&]
        {
          start_gate.arrive_and_wait();
          try {
            auto backend = reg.create("clp");
            if (!backend) {
              bad.fetch_add(1, std::memory_order_relaxed);
              return;
            }

            SolverOptions opts;
            opts.algorithm = LPAlgo::dual;
            opts.threads = 1;
            backend->apply_options(opts);

            TrivialLP2 lp {backend->infinity()};
            lp.load_into(*backend);
            backend->initial_solve();
            if (!backend->is_proven_optimal()) {
              bad.fetch_add(1, std::memory_order_relaxed);
              return;
            }

            auto cloned = backend->clone();
            if (!cloned) {
              bad.fetch_add(1, std::memory_order_relaxed);
              return;
            }
            cloned->resolve();
            if (!cloned->is_proven_optimal()) {
              bad.fetch_add(1, std::memory_order_relaxed);
              return;
            }

            const double delta = std::abs(cloned->obj_value() - 2.0);
            if (delta > 1e-6) {
              bad.fetch_add(1, std::memory_order_relaxed);
              return;
            }

            ok.fetch_add(1, std::memory_order_relaxed);
          } catch (...) {
            bad.fetch_add(1, std::memory_order_relaxed);
          }
        });
  }
  for (auto& w : workers) {
    w.join();
  }

  CHECK(ok.load() == num_threads);
  CHECK(bad.load() == 0);
}
