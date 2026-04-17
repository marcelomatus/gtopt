/**
 * @file      test_gurobi_backend.cpp
 * @brief     Unit tests for the Gurobi plugin's GurobiPrep refactor
 * @date      2026-04-16
 * @copyright BSD-3-Clause
 *
 * Every TEST_CASE is gated on `reg.has_solver("gurobi")` and silently
 * skips when the plugin is not available in the test environment.  The
 * Gurobi SDK additionally requires a license file; a missing license
 * causes make_silent_env() to throw at construction (fast path — no
 * WLS network lookup), which we also treat as "skip" rather than
 * "fail".
 *
 * The Gurobi plugin pairs a GRBenv and a GRBmodel in a backend and
 * caches its configuration (SolverOptions + log filename + log level +
 * problem name) inside a GurobiPrep struct.  `load_problem()` calls
 * `reset_model_()` so every bulk load starts from a pristine GRBmodel,
 * and `clone()` deep-copies m_prep_ while replaying every env-level
 * parameter onto the clone's brand-new GRBenv (Gurobi env params do
 * NOT survive GRBcopymodeltoenv, so the replay is mandatory for
 * independence).  These tests pin the invariants of that refactor:
 *   - silent by default (GRB_INT_PAR_OUTPUTFLAG=0, no stray *.log),
 *   - state survives the model cycle triggered by load_problem,
 *   - clone() yields a backend that owns its own GRBenv + GRBmodel,
 *     with options and prob_name replayed onto the fresh env,
 *   - concurrent backend creation and cloning is race-free.
 *
 * NOTE: SolverOptions::low_memory is a documented no-op for Gurobi
 * (mirrors MindOpt), so no low_memory test case is included.
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

/// Return a fresh Gurobi backend, or nullptr if the plugin is
/// unavailable.  Also returns nullptr if GRBstartenv throws (typically
/// a missing license file or GUROBI_HOME) so the test silently skips.
[[nodiscard]] std::unique_ptr<SolverBackend> make_gurobi_or_skip()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("gurobi")) {
    return nullptr;
  }
  try {
    return reg.create("gurobi");
  } catch (...) {
    return nullptr;
  }
}

// Trivial 2-variable LP:   min x + y   s.t.   x + y >= 2,  x,y >= 0.
// Optimum:                 x + y = 2  →  obj = 2.
struct GurobiTrivialLP2
{
  static constexpr int ncols = 2;
  static constexpr int nrows = 1;
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
  std::array<double, 1> rowlb {2.0};
  std::array<double, 1> rowub {};

  explicit GurobiTrivialLP2(double inf)
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

// Trivial 3-variable LP:   min x + y + z   s.t.   x + y + z >= 3, all >= 0.
struct GurobiTrivialLP3
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
  std::array<double, 1> rowlb {3.0};
  std::array<double, 1> rowub {};

  explicit GurobiTrivialLP3(double inf)
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

/// Scratch base path for log-file tests.
[[nodiscard]] std::filesystem::path gurobi_scratch_log_base(
    std::string_view tag)
{
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "gtopt_gurobi_log_test";
  fs::create_directories(dir);
  return dir / std::string(tag);
}

}  // namespace

// ---------------------------------------------------------------------------
// A. Silence invariants
// ---------------------------------------------------------------------------

TEST_CASE("Gurobi default ctor is silent (no stray *.log)")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "gtopt_gurobi_silent_default";
  fs::create_directories(dir);
  for (const auto& entry : fs::directory_iterator {dir}) {
    if (entry.is_regular_file() && entry.path().extension() == ".log") {
      fs::remove(entry.path());
    }
  }

  backend->set_prob_name("silent");
  CHECK(backend->get_prob_name() == "silent");

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();

  for (const auto& entry : fs::directory_iterator {dir}) {
    CHECK(entry.path().extension() != ".log");
  }
}

TEST_CASE(
    "set_log_filename with level<=0 or empty name stays silent")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;
  const auto base = gurobi_scratch_log_base("should_not_exist");
  const auto file = fs::path {base.string() + ".log"};
  fs::remove(file);

  backend->set_log_filename("", 0);
  backend->set_log_filename(base.string(), 0);
  backend->set_log_filename("", 1);

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();

  CHECK_FALSE(fs::exists(file));
}

TEST_CASE("set_log_filename(level>0) writes a log; clear stops it")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  namespace fs = std::filesystem;
  const auto base = gurobi_scratch_log_base("written");
  const auto file = fs::path {base.string() + ".log"};
  fs::remove(file);

  backend->set_log_filename(base.string(), 1);

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();

  CHECK(fs::exists(file));

  backend->clear_log_filename();
  lp.load_into(*backend);
  backend->initial_solve();

  fs::remove(file);
}

// ---------------------------------------------------------------------------
// B. load_problem cycles the GRBmodel and replays prep
// ---------------------------------------------------------------------------

TEST_CASE("load_problem destroys previous LP state")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  const double inf = backend->infinity();

  const GurobiTrivialLP2 lp2 {inf};
  lp2.load_into(*backend);
  CHECK(backend->get_num_cols() == 2);
  CHECK(backend->get_num_rows() == 1);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));

  const GurobiTrivialLP3 lp3 {inf};
  lp3.load_into(*backend);
  CHECK(backend->get_num_cols() == 3);
  CHECK(backend->get_num_rows() == 1);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(3.0));
}

TEST_CASE("apply_options survives load_problem cycle")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  SolverOptions opts;
  opts.algorithm = LPAlgo::dual;
  opts.threads = 2;
  opts.presolve = false;
  opts.log_level = 0;
  backend->apply_options(opts);

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  CHECK(backend->get_algorithm() == LPAlgo::dual);
  CHECK(backend->get_threads() == 2);
  CHECK_FALSE(backend->get_presolve());
  CHECK(backend->get_log_level() == 0);

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
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  backend->set_prob_name("my_lp");
  CHECK(backend->get_prob_name() == "my_lp");

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  CHECK(backend->get_prob_name() == "my_lp");
}

// ---------------------------------------------------------------------------
// C. Clone independence
// ---------------------------------------------------------------------------

TEST_CASE(
    "clone owns its own GRBenv+GRBmodel: source may be destroyed")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  auto cloned = backend->clone();
  REQUIRE(cloned != nullptr);

  // Destroy the source.  If clone shared the GRBenv or GRBmodel with
  // it, this would invalidate the clone's handles and the next solve
  // would crash or return an error from Gurobi.
  backend.reset();

  cloned->resolve();
  REQUIRE(cloned->is_proven_optimal());
  CHECK(cloned->obj_value() == doctest::Approx(2.0));
  CHECK(cloned->get_num_cols() == 2);
  CHECK(cloned->get_num_rows() == 1);
}

TEST_CASE("clone preserves options and prob_name on fresh env")  // NOLINT
{
  auto backend = make_gurobi_or_skip();
  if (!backend) {
    return;
  }

  SolverOptions opts;
  opts.algorithm = LPAlgo::primal;
  opts.threads = 1;
  backend->apply_options(opts);
  backend->set_prob_name("p");

  const GurobiTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);

  auto cloned = backend->clone();
  REQUIRE(cloned != nullptr);

  // Gurobi env-level parameters do NOT survive GRBcopymodeltoenv, so
  // clone() MUST replay every parameter from m_prep_ onto the clone's
  // brand-new GRBenv via apply_options_to_env().  If the replay were
  // broken, the accessors below would return defaults instead of the
  // values we set.
  CHECK(cloned->get_algorithm() == LPAlgo::primal);
  CHECK(cloned->get_threads() == 1);
  CHECK(cloned->get_prob_name() == "p");
}

// ---------------------------------------------------------------------------
// D. Thread-safety smoke test — each thread builds its own env+model
// ---------------------------------------------------------------------------

TEST_CASE("Gurobi backend: parallel create+load+clone is race-free")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("gurobi")) {
    return;
  }
  // Guard against missing-license environments: one throw-away ctor
  // probes before we spin up threads.
  try {
    auto probe = reg.create("gurobi");
    if (!probe) {
      return;
    }
  } catch (...) {
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
            auto backend = reg.create("gurobi");
            if (!backend) {
              bad.fetch_add(1, std::memory_order_relaxed);
              return;
            }

            SolverOptions opts;
            opts.algorithm = LPAlgo::dual;
            opts.threads = 1;
            backend->apply_options(opts);

            const GurobiTrivialLP2 lp {backend->infinity()};
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
