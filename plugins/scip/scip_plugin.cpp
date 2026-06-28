/**
 * @file      scip_plugin.cpp
 * @brief     Plugin entry point for the SCIP solver backend
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include <gtopt/solver_backend.hpp>

#include "scip_solver_backend.hpp"

namespace
{

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
const char* const k_solver_names[] = {
    "scip",
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
  return "scip";
}

const char* const* gtopt_solver_names()  // NOLINT
{
  return k_solver_names;  // NOLINT
}

gtopt::SolverBackend* gtopt_create_backend(  // NOLINT
    const char* solver_name)
{
  if (std::strcmp(solver_name, "scip") == 0) {
    return new gtopt::ScipSolverBackend();  // NOLINT
  }
  return nullptr;
}

double gtopt_solver_infinity(const char* solver_name)  // NOLINT
{
  if (std::strcmp(solver_name, "scip") == 0) {
    return gtopt::ScipSolverBackend::plugin_infinity();
  }
  return 0.0;
}
}
