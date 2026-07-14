/**
 * @file      solver_backend.cpp
 * @brief     Non-inline helpers for the SolverBackend plugin interface
 * @date      Mon Jul 13 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Home of the process-environment reads used by the backend interface
 * (env reads live in a .cpp, never in a header).
 */

#include <cstdlib>

#include <gtopt/solver_backend.hpp>

namespace gtopt
{

bool test_force_fix_duals_bail() noexcept
{
  // TEST-ONLY (see the declaration in solver_backend.hpp): forces the
  // dual-recovery bail branch in `fix_mip_and_resolve_duals`.  Read
  // fresh on every call — a static latch would pin the value observed
  // on the first fix pass of the process, breaking tests that scope the
  // variable with setenv/unsetenv.  The read is on a cold path (once
  // per MIP dual-recovery pass), so the getenv cost is irrelevant.
  const char* const v = std::getenv("GTOPT_TEST_FORCE_SOS_BAIL");
  return v != nullptr && v[0] == '1';
}

}  // namespace gtopt
