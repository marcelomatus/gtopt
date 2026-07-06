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

#include <string>

#include <gtopt/solver_registry.hpp>

namespace gtopt::solver_test
{

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

}  // namespace gtopt::solver_test
