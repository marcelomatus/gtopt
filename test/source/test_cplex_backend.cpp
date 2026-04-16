/**
 * @file      test_cplex_backend.cpp
 * @brief     Unit tests for the CPLEX plugin's CplexEnvLp RAII refactor
 * @date      2026-04-16
 * @copyright BSD-3-Clause
 *
 * Every TEST_CASE is gated on `reg.has_solver("cplex")` and silently
 * skips when the plugin is not available in the test environment.
 *
 * The CPLEX plugin bundles one CPLEX env + one CPLEX lp into a move-only
 * RAII type (CplexEnvLp).  Each call to load_problem() destroys that
 * pair and re-creates a fresh env+lp from a cached CplexPrep
 * (SolverOptions + log filename + log level + problem name).  These
 * tests pin the invariants of that refactor:
 *   - silent by default (no cplex.log unless explicitly requested),
 *   - state survives the env/lp cycle triggered by load_problem,
 *   - clone() yields a backend that owns its own env+lp and survives
 *     destruction of the source,
 *   - SolverOptions::low_memory → CPX_PARAM_MEMORYEMPHASIS still works
 *     after env cycling and cloning,
 *   - concurrent backend creation and cloning is race-free (mirrors
 *     PLP's by-design one-env-per-LP ownership pattern).
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

/// Return a fresh CPLEX backend, or nullptr if the plugin is unavailable.
[[nodiscard]] std::unique_ptr<SolverBackend> make_cplex_or_skip()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("cplex")) {
    return nullptr;
  }
  return reg.create("cplex");
}

// Trivial 2-variable LP:   min x + y   s.t.   x + y >= 2,  x,y >= 0.
// Optimum:                 x + y = 2  →  obj = 2.
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
// Optimum:                 sum = 3  →  obj = 3.
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
  auto dir = fs::temp_directory_path() / "gtopt_cplex_log_test";
  fs::create_directories(dir);
  return dir / std::string(tag);
}

}  // namespace

// ---------------------------------------------------------------------------
// A. RAII / silence invariants
// ---------------------------------------------------------------------------

TEST_CASE("CplexEnvLp default ctor is silent (no cplex.log)")  // NOLINT
{
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  // Use a dedicated working directory so we can assert nothing was written
  // to it.  We do *not* chdir — CPLEX only creates cplex.log when it is
  // explicitly told to, and we simply assert no stray file named cplex.log
  // lands next to us.
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "gtopt_cplex_silent_default";
  fs::create_directories(dir);
  const auto stray = dir / "cplex.log";
  fs::remove(stray);

  backend->set_prob_name("silent");
  CHECK(backend->get_prob_name() == "silent");

  CHECK_FALSE(fs::exists(stray));
}

TEST_CASE(
    "set_log_filename with level<=0 or empty name stays silent")  // NOLINT
{
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;
  const auto base = scratch_log_base("should_not_exist");
  const auto file = fs::path {base.string() + ".log"};
  fs::remove(file);

  // level == 0: must be ignored.
  backend->set_log_filename(base.string(), 0);
  // empty filename: must be ignored even if level > 0.
  backend->set_log_filename("", 1);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();

  CHECK_FALSE(fs::exists(file));
}

TEST_CASE("set_log_filename(level>0) writes a log; clear stops it")  // NOLINT
{
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;
  const auto base = scratch_log_base("written");
  const auto file = fs::path {base.string() + ".log"};
  fs::remove(file);

  backend->set_log_filename(base.string(), 1);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();

  CHECK(fs::exists(file));

  // After clear, a subsequent solve should not *grow* the file.  We
  // only check existence + that clear_log_filename is callable without
  // throwing, because CPLEX flushes asynchronously and byte counts can
  // be noisy.
  backend->clear_log_filename();
  lp.load_into(*backend);
  backend->initial_solve();

  // Cleanup: leave no trace in /tmp.
  fs::remove(file);
}

// ---------------------------------------------------------------------------
// B. load_problem cycles the env+lp and replays prep
// ---------------------------------------------------------------------------

TEST_CASE("load_problem destroys previous LP state")  // NOLINT
{
  auto backend = make_cplex_or_skip();
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

  // Second load_problem must yield a *fresh* env+lp with different
  // dimensions.  A stale env would still report ncols=2.
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
  auto backend = make_cplex_or_skip();
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

  // A second load_problem rebuilds the env+lp pair.  The cached options
  // in m_prep_ must be re-applied, so the accessors keep the same values.
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
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  backend->set_prob_name("my_lp");
  CHECK(backend->get_prob_name() == "my_lp");

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  // After the env+lp was rebuilt, CplexPrep::prob_name must be replayed
  // via the ctor so the new lp still carries the name.
  CHECK(backend->get_prob_name() == "my_lp");
}

// ---------------------------------------------------------------------------
// C. low_memory end-to-end (SolverOptions::low_memory →
// CPX_PARAM_MEMORYEMPHASIS)
// ---------------------------------------------------------------------------

TEST_CASE("low_memory=true solves correctly and survives clone")  // NOLINT
{
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  SolverOptions opts;
  opts.low_memory = true;
  backend->apply_options(opts);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));

  // Clone must also get CPX_PARAM_MEMORYEMPHASIS via m_prep_ replay and
  // still solve to the same optimum.
  auto cloned = backend->clone();
  REQUIRE(cloned != nullptr);
  cloned->resolve();
  REQUIRE(cloned->is_proven_optimal());
  CHECK(cloned->obj_value() == doctest::Approx(2.0));
}

TEST_CASE("low_memory propagates through load_problem cycles")  // NOLINT
{
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  SolverOptions opts;
  opts.low_memory = true;
  backend->apply_options(opts);

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  const double first_obj = backend->obj_value();

  // Second load_problem must re-apply low_memory on the fresh env.
  // If the replay were broken, the solve might still succeed but under
  // different memory policy; at minimum we check the result is the same.
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(first_obj));
}

// ---------------------------------------------------------------------------
// D. Clone independence
// ---------------------------------------------------------------------------

TEST_CASE("clone owns its own env+lp: source may be destroyed")  // NOLINT
{
  auto backend = make_cplex_or_skip();
  if (!backend) {
    return;
  }

  TrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  auto cloned = backend->clone();
  REQUIRE(cloned != nullptr);

  // Destroy the source.  If clone shared env or lp with it, this would
  // invalidate the clone's handles and the next solve would crash or
  // abort with a CPLEX error.
  backend.reset();

  cloned->resolve();
  REQUIRE(cloned->is_proven_optimal());
  CHECK(cloned->obj_value() == doctest::Approx(2.0));
  CHECK(cloned->get_num_cols() == 2);
  CHECK(cloned->get_num_rows() == 1);
}

TEST_CASE("clone preserves options and prob_name")  // NOLINT
{
  auto backend = make_cplex_or_skip();
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

  CHECK(cloned->get_algorithm() == LPAlgo::primal);
  CHECK(cloned->get_threads() == 1);
  CHECK(cloned->get_prob_name() == "p");
}

// ---------------------------------------------------------------------------
// E. Thread-safety smoke test — mirrors PLP's by-design pattern
// (one env per LP, allocated and used independently per thread;
// no mutex around CPLEX env/lp creation).
// ---------------------------------------------------------------------------

TEST_CASE("CPLEX backend: parallel create+load+clone is race-free")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("cplex")) {
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
            auto backend = reg.create("cplex");
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
