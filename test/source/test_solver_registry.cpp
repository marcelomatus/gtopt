/**
 * @file      test_solver_registry.hpp
 * @brief     Unit tests for SolverRegistry and classify_error_exit_code
 * @date      2026-03-27
 * @copyright BSD-3-Clause
 */

#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── SolverRegistry singleton ───────────────────────────────────────────────

TEST_CASE("SolverRegistry instance returns same object")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg1 = SolverRegistry::instance();
  auto& reg2 = SolverRegistry::instance();
  CHECK(&reg1 == &reg2);
}

TEST_CASE("SolverRegistry has at least one solver")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  const auto solvers = reg.available_solvers();
  CHECK_FALSE(solvers.empty());
}

TEST_CASE("SolverRegistry has_solver for known solvers")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  // At least one of these should be available in the test environment
  const bool has_any = reg.has_solver("clp") || reg.has_solver("cbc")
      || reg.has_solver("highs") || reg.has_solver("cplex");
  CHECK(has_any);
}

TEST_CASE("SolverRegistry has_solver returns false for unknown")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  CHECK_FALSE(reg.has_solver("nonexistent_solver_xyz"));
}

TEST_CASE("SolverRegistry default_solver returns a valid name")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  const auto name = reg.default_solver();
  CHECK_FALSE(name.empty());
  CHECK(reg.has_solver(name));
}

TEST_CASE("SolverRegistry create produces a backend")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  const auto name = reg.default_solver();
  auto backend = reg.create(name);
  CHECK(backend != nullptr);
}

TEST_CASE("SolverRegistry create throws for unknown solver")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  CHECK_THROWS_AS((void)reg.create("nonexistent_solver_xyz"),
                  std::runtime_error);
}

TEST_CASE("SolverRegistry searched_directories is not empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  CHECK_FALSE(reg.searched_directories().empty());
}

TEST_CASE("SolverRegistry load_errors returns a vector")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  // May or may not have errors — just verify it's callable
  (void)reg.load_errors();
}

TEST_CASE(  // NOLINT
    "SolverRegistry discover_plugins with nonexistent dir is safe")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  const auto dirs_before = reg.searched_directories().size();
  reg.discover_plugins("/tmp/nonexistent_plugin_dir_xyz_12345");
  CHECK(reg.searched_directories().size() == dirs_before + 1);
}

TEST_CASE(  // NOLINT
    "SolverRegistry load_plugin with nonexistent file returns false")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  CHECK_FALSE(reg.load_plugin("/tmp/nonexistent_plugin.so"));
  CHECK_FALSE(reg.load_errors().empty());
}

TEST_CASE("SolverRegistry supports_mip per backend")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  // Pure-LP backends report false; B&B-capable backends report true.
  if (reg.has_solver("clp")) {
    CHECK_FALSE(reg.supports_mip("clp"));
  }
  if (reg.has_solver("cbc")) {
    CHECK(reg.supports_mip("cbc"));
  }
  if (reg.has_solver("cplex")) {
    CHECK(reg.supports_mip("cplex"));
  }
  if (reg.has_solver("highs")) {
    CHECK(reg.supports_mip("highs"));
  }
  if (reg.has_solver("mindopt")) {
    CHECK(reg.supports_mip("mindopt"));
  }

  CHECK_FALSE(reg.supports_mip("nonexistent_solver_xyz"));
}

TEST_CASE("SolverRegistry has_mip_solver returns true when MIP available")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  // The test environment ships at least one MIP-capable backend.
  CHECK(reg.has_mip_solver());
}

// ─── SolverRegistry additional coverage ────────────────────────────────────

TEST_CASE(  // NOLINT
    "SolverRegistry create all available solvers")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  const auto solvers = reg.available_solvers();
  for (const auto& name : solvers) {
    auto backend = reg.create(name);
    REQUIRE(backend != nullptr);
    // Exercise basic backend accessors
    CHECK_FALSE(backend->solver_name().empty());
    CHECK(backend->infinity() >= 1e20);
  }
}

TEST_CASE(  // NOLINT
    "SolverRegistry load_plugin with non-so file returns false")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  // Create a temporary text file (not a shared library)
  const auto tmp_path =
      std::filesystem::temp_directory_path() / "fake_plugin.so";
  {
    std::ofstream ofs(tmp_path);
    ofs << "not a shared library";
  }
  const auto errors_before = reg.load_errors().size();
  CHECK_FALSE(reg.load_plugin(tmp_path));
  CHECK(reg.load_errors().size() > errors_before);
  std::filesystem::remove(tmp_path);
}

TEST_CASE(  // NOLINT
    "SolverRegistry discover_plugins with empty dir adds to searched")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  const auto tmp_dir =
      std::filesystem::temp_directory_path() / "gtopt_empty_plugin_dir";
  std::filesystem::create_directories(tmp_dir);
  const auto dirs_before = reg.searched_directories().size();
  reg.discover_plugins(tmp_dir);
  CHECK(reg.searched_directories().size() == dirs_before + 1);
  std::filesystem::remove(tmp_dir);
}

TEST_CASE(  // NOLINT
    "SolverRegistry create throws with descriptive message")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  bool caught = false;
  try {
    (void)reg.create("totally_fake_solver_42");
  } catch (const std::runtime_error& ex) {
    caught = true;
    const std::string msg = ex.what();
    // Message should mention the requested solver name
    CHECK(msg.find("totally_fake_solver_42") != std::string::npos);
    // Message should mention available solvers
    CHECK(msg.find("Available") != std::string::npos);
  }
  CHECK(caught);
}

TEST_CASE(  // NOLINT
    "SolverRegistry default_solver is in available_solvers list")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  const auto def = std::string(reg.default_solver());
  const auto solvers = reg.available_solvers();
  const bool found = std::ranges::find(solvers, def) != solvers.end();
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "SolverRegistry available_solvers returns consistent results")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  const auto solvers1 = reg.available_solvers();
  const auto solvers2 = reg.available_solvers();
  CHECK(solvers1 == solvers2);
}

// ─── classify_error_exit_code ───────────────────────────────────────────────

TEST_CASE("classify_error_exit_code input errors return 2")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(classify_error_exit_code("File not found") == 2);
  CHECK(classify_error_exit_code("does not exist") == 2);
  CHECK(classify_error_exit_code("Cannot open file") == 2);
  CHECK(classify_error_exit_code("Failed to parse") == 2);
  CHECK(classify_error_exit_code("Invalid parameter") == 2);
  CHECK(classify_error_exit_code("JSON error") == 2);
}

TEST_CASE("classify_error_exit_code internal errors return 3")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(classify_error_exit_code("Solver crashed") == 3);
  CHECK(classify_error_exit_code("Segmentation fault") == 3);
  CHECK(classify_error_exit_code("Unknown error") == 3);
  CHECK(classify_error_exit_code("") == 3);
}

// `SolverRegistry::create` used to hold its internal recursive mutex
// across the entire `plugin.create_fn()` call, which serialized the
// `PlanningLP` parallel LP build — every (scene, phase) cell funneled
// through one lock while its backend was constructed.  The refactor
// (source/solver_registry.cpp:443-501) releases the lock after the
// plugin lookup and only then calls `plugin.create_fn`.  This test
// pins the regression: N threads call `create()` simultaneously and
// every thread must get a non-null backend without deadlocking.  We
// don't measure parallelism here (CI boxes have variable core counts
// and plugin startup cost) — we test correctness of the lock release
// under concurrent load, which a regression would break via a
// deadlock if the wrong scope were re-acquired or a data race
// otherwise.
TEST_CASE(
    "SolverRegistry::create is thread-safe under parallel load")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  // Pick any available solver — CI may have clp/cbc/highs/cplex.  The
  // point is to exercise the parallel `create()` path; the identity
  // of the backend doesn't matter.
  const auto solvers = reg.available_solvers();
  REQUIRE_FALSE(solvers.empty());
  const std::string solver_name = solvers.front();

  constexpr int num_threads = 16;
  constexpr int creates_per_thread = 4;

  std::barrier start_gate(num_threads);
  std::vector<std::thread> workers;
  std::atomic<int> success_count {0};
  std::atomic<int> null_count {0};
  std::atomic<int> exception_count {0};

  workers.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    workers.emplace_back(
        [&]
        {
          // All threads start their first `create()` call together so
          // they race through the mutex at the same time.  A pre-fix
          // regression would still complete (it would just serialize
          // on the lock), so this is a correctness — not a timing —
          // test.
          start_gate.arrive_and_wait();
          for (int j = 0; j < creates_per_thread; ++j) {
            try {
              auto backend = reg.create(solver_name);
              if (backend) {
                success_count.fetch_add(1, std::memory_order_relaxed);
              } else {
                null_count.fetch_add(1, std::memory_order_relaxed);
              }
            } catch (...) {
              exception_count.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
  }
  for (auto& w : workers) {
    w.join();
  }

  // Every single create() call must succeed — no exceptions, no
  // null backends, no deadlocks (join would hang).
  CHECK(success_count.load() == num_threads * creates_per_thread);
  CHECK(null_count.load() == 0);
  CHECK(exception_count.load() == 0);
}
