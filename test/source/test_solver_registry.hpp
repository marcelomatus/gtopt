/**
 * @file      test_solver_registry.hpp
 * @brief     Unit tests for SolverRegistry and classify_error_exit_code
 * @date      2026-03-27
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── SolverRegistry singleton ───────────────────────────────────────────────

TEST_CASE("SolverRegistry instance returns same object")  // NOLINT
{
  auto& reg1 = SolverRegistry::instance();
  auto& reg2 = SolverRegistry::instance();
  CHECK(&reg1 == &reg2);
}

TEST_CASE("SolverRegistry has at least one solver")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto solvers = reg.available_solvers();
  CHECK_FALSE(solvers.empty());
}

TEST_CASE("SolverRegistry has_solver for known solvers")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  // At least one of these should be available in the test environment
  const bool has_any = reg.has_solver("clp") || reg.has_solver("cbc")
      || reg.has_solver("highs") || reg.has_solver("cplex");
  CHECK(has_any);
}

TEST_CASE("SolverRegistry has_solver returns false for unknown")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  CHECK_FALSE(reg.has_solver("nonexistent_solver_xyz"));
}

TEST_CASE("SolverRegistry default_solver returns a valid name")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto name = reg.default_solver();
  CHECK_FALSE(name.empty());
  CHECK(reg.has_solver(name));
}

TEST_CASE("SolverRegistry create produces a backend")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto name = reg.default_solver();
  auto backend = reg.create(name);
  CHECK(backend != nullptr);
}

TEST_CASE("SolverRegistry create throws for unknown solver")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  CHECK_THROWS_AS((void)reg.create("nonexistent_solver_xyz"),
                  std::runtime_error);
}

TEST_CASE("SolverRegistry searched_directories is not empty")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  CHECK_FALSE(reg.searched_directories().empty());
}

TEST_CASE("SolverRegistry load_errors returns a vector")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  // May or may not have errors — just verify it's callable
  (void)reg.load_errors();
}

TEST_CASE(  // NOLINT
    "SolverRegistry discover_plugins with nonexistent dir is safe")
{
  auto& reg = SolverRegistry::instance();
  const auto dirs_before = reg.searched_directories().size();
  reg.discover_plugins("/tmp/nonexistent_plugin_dir_xyz_12345");
  CHECK(reg.searched_directories().size() == dirs_before + 1);
}

TEST_CASE(  // NOLINT
    "SolverRegistry load_plugin with nonexistent file returns false")
{
  auto& reg = SolverRegistry::instance();
  CHECK_FALSE(reg.load_plugin("/tmp/nonexistent_plugin.so"));
  CHECK_FALSE(reg.load_errors().empty());
}

// ─── classify_error_exit_code ───────────────────────────────────────────────

TEST_CASE("classify_error_exit_code input errors return 2")  // NOLINT
{
  CHECK(classify_error_exit_code("File not found") == 2);
  CHECK(classify_error_exit_code("does not exist") == 2);
  CHECK(classify_error_exit_code("Cannot open file") == 2);
  CHECK(classify_error_exit_code("Failed to parse") == 2);
  CHECK(classify_error_exit_code("Invalid parameter") == 2);
  CHECK(classify_error_exit_code("JSON error") == 2);
}

TEST_CASE("classify_error_exit_code internal errors return 3")  // NOLINT
{
  CHECK(classify_error_exit_code("Solver crashed") == 3);
  CHECK(classify_error_exit_code("Segmentation fault") == 3);
  CHECK(classify_error_exit_code("Unknown error") == 3);
  CHECK(classify_error_exit_code("") == 3);
}
