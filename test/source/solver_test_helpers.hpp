// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file solver_test_helpers.hpp
 * @brief Test-only helpers for adapting assertions to the active solver.
 *
 * gtopt's unit tests run against whatever backend `GTOPT_SOLVER` selects.
 * Most backends are exact simplex/barrier solvers, but cuOpt is a GPU
 * first-order (PDLP/concurrent) solver: it returns interior points rather
 * than exact vertex duals and exposes no basis, and its branch-and-bound is
 * not bit-exact.  These helpers let tests (a) skip precise dual/basis
 * assertions under a first-order default solver and (b) pin an available
 * exact MIP solver for unit-commitment fixtures instead of hard-coding one.
 */
#pragma once

#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/solver_registry.hpp>

namespace gtopt::solver_test
{

/// True when a solver failure message indicates a license / licensed-
/// environment problem (e.g. MindOpt `MDOstartenv rc=-10: License is not
/// valid`, Gurobi `no license file found`).  Licensed backends can vanish
/// transiently even after plugin validation succeeded (license-daemon
/// hiccups under parallel test load), so all-solver sweeps treat this as
/// "skip that solver", never "fail the test".  Anything else must still
/// fail loudly.
[[nodiscard]] inline bool is_license_failure(std::string_view message)
{
  return message.contains("icense");
}

[[nodiscard]] inline bool is_license_failure(const std::exception& ex)
{
  return is_license_failure(std::string_view {ex.what()});
}

/// True when the process-default solver (honouring `GTOPT_SOLVER`) is a GPU
/// first-order solver — currently only cuOpt.  Such solvers do not return
/// exact simplex vertex duals or a basis, so tests that assert on precise
/// dual/basis values must skip under them.
[[nodiscard]] inline bool default_is_first_order()
{
  return SolverRegistry::instance().default_solver() == "cuopt";
}

/// First available EXACT MIP solver by priority, or "" if none is loaded.
/// cuOpt is excluded on purpose — its GPU branch-and-bound is not bit-exact,
/// so unit-commitment fixtures that need deterministic integer results pin
/// one of these regardless of the ambient `GTOPT_SOLVER`.
[[nodiscard]] inline std::string first_mip_solver()
{
  auto& reg = SolverRegistry::instance();
  for (const auto* name :
       {"cplex", "highs", "scip", "mindopt", "cbc", "gurobi"})
  {
    if (reg.supports_mip(name)) {
      return name;
    }
  }
  return {};
}

/// Run `body` for solver `name`; returns true when it ran to completion,
/// false when it was skipped because the backend failed to START with a
/// license error (see is_license_failure).  Any other exception — including
/// doctest assertion failures — propagates.
template<typename Body>
[[nodiscard]] inline bool run_or_skip_license(Body&& body)
{
  try {
    std::forward<Body>(body)();
    return true;
  } catch (const std::exception& e) {
    if (!is_license_failure(e)) {
      throw;
    }
    return false;
  }
}

/// Loaded MIP-capable solvers that are safe for generic branch-and-bound
/// sweeps.  cuOpt is excluded on two grounds: (a) its B&B logs from its
/// OpenMP worker threads through a non-thread-safe logger — observed
/// SIGSEGV inside libcuopt (`logger_t::print_format` during
/// `variable_bounds_t` construction), unfixable from gtopt; (b) its GPU
/// branch-and-bound is not bit-exact (same reason it is excluded from
/// `first_mip_solver()`).
[[nodiscard]] inline std::vector<std::string> exact_mip_solvers()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  std::vector<std::string> result;
  for (const auto& name : reg.available_solvers()) {
    if (name != "cuopt" && reg.supports_mip(name)) {
      result.push_back(name);
    }
  }
  return result;
}

}  // namespace gtopt::solver_test
