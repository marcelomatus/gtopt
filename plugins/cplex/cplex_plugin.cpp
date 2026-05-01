/**
 * @file      cplex_plugin.cpp
 * @brief     Plugin entry point for CPLEX solver backend
 * @date      Tue Mar 25 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include <gtopt/solver_backend.hpp>

#include "cplex_solver_backend.hpp"

namespace
{

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
const char* const k_solver_names[] = {
    "cplex",
    nullptr,
};

}  // namespace

extern "C"
{
int gtopt_plugin_abi_version()  // NOLINT
{
  return gtopt::k_solver_abi_version;
}

const char* gtopt_plugin_name()  // NOLINT
{
  return "cplex";
}

const char* const* gtopt_solver_names()  // NOLINT
{
  return k_solver_names;  // NOLINT
}

gtopt::SolverBackend* gtopt_create_backend(  // NOLINT
    const char* solver_name)
{
  if (std::strcmp(solver_name, "cplex") == 0) {
    return new gtopt::CplexSolverBackend();  // NOLINT
  }
  return nullptr;
}

// Plugin-level infinity query (no SolverBackend instance allocated).
// Delegates to `CplexSolverBackend::plugin_infinity()` so the value
// is single-source-of-truth shared with the instance method
// `CplexSolverBackend::infinity()`.  CPLEX uses CPX_INFBOUND = 1e+20.
double gtopt_solver_infinity(const char* solver_name)  // NOLINT
{
  if (std::strcmp(solver_name, "cplex") == 0) {
    return gtopt::CplexSolverBackend::plugin_infinity();
  }
  return 0.0;
}
}
