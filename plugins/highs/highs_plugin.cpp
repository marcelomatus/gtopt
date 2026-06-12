/**
 * @file      highs_plugin.cpp
 * @brief     Plugin entry point for HiGHS solver backend
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include <gtopt/solver_backend.hpp>

#include "highs_solver_backend.hpp"

namespace
{

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
const char* const k_solver_names[] = {
    "highs",
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
  return "highs";
}

const char* const* gtopt_solver_names()  // NOLINT
{
  return k_solver_names;  // NOLINT
}

gtopt::SolverBackend* gtopt_create_backend(  // NOLINT
    const char* solver_name)
{
  if (std::strcmp(solver_name, "highs") == 0) {
    return new gtopt::HighsSolverBackend();  // NOLINT
  }
  return nullptr;
}

// Plugin-level infinity query (no instance allocated).  Shares the
// `kHighsInf = 1e+30` constant with `HighsSolverBackend::infinity()`.
double gtopt_solver_infinity(const char* solver_name)  // NOLINT
{
  if (std::strcmp(solver_name, "highs") == 0) {
    return gtopt::HighsSolverBackend::plugin_infinity();
  }
  return 0.0;
}
}
