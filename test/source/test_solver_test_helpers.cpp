// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_solver_test_helpers.cpp
 * @brief     Unit tests for the shared solver-test helpers
 * @copyright BSD-3-Clause
 *
 * The helpers in solver_test_helpers.hpp encode the skip policy every
 * all-solver sweep relies on (license flakes skip, real failures fail
 * loudly) — misclassification there would silently weaken hundreds of
 * tests, so the classification itself is pinned here.
 */

#include <stdexcept>
#include <tuple>

#include <doctest/doctest.h>
#include <gtopt/solver_registry.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

TEST_CASE("is_license_failure classifies solver start failures")  // NOLINT
{
  using solver_test::is_license_failure;

  // Real messages observed from licensed backends in this suite.
  CHECK(is_license_failure(
      "MindOpt: MDOstartenv failed (rc=-10: License is not valid.)"));
  CHECK(is_license_failure("Gurobi: no license file found."));
  CHECK(is_license_failure("license expired"));

  // Non-license failures must NOT be classified as skippable.
  CHECK_FALSE(is_license_failure("cuopt: cuOptSolve failed with status 3"));
  CHECK_FALSE(is_license_failure("CPLEX: out of memory"));
  CHECK_FALSE(is_license_failure(""));

  // The exception overload reads what().
  const std::runtime_error ex("MindOpt: License is not valid.");
  CHECK(is_license_failure(ex));
}

TEST_CASE("run_or_skip_license runs, skips, or propagates")  // NOLINT
{
  using solver_test::run_or_skip_license;

  SUBCASE("body that completes reports ran == true")
  {
    bool ran = false;
    CHECK(run_or_skip_license([&] { ran = true; }));
    CHECK(ran);
  }

  SUBCASE("license failure reports ran == false (skip)")
  {
    CHECK_FALSE(run_or_skip_license(
        [] { throw std::runtime_error("License is not valid"); }));
  }

  SUBCASE("any other exception propagates — real failures stay loud")
  {
    CHECK_THROWS_AS(std::ignore = run_or_skip_license(
                        [] { throw std::runtime_error("disk full"); }),
                    std::runtime_error);
  }
}

TEST_CASE("exact_mip_solvers excludes cuopt and LP-only backends")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& name : solver_test::exact_mip_solvers()) {
    CAPTURE(name);
    // cuOpt is excluded by policy (thread-unsafe B&B logger,
    // non-bit-exact GPU branch-and-bound); clp is LP-only.
    CHECK(name != "cuopt");
    CHECK(name != "clp");
  }
}
