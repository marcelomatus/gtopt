/**
 * @file      osi_plugin.cpp
 * @brief     Plugin entry point for OSI solver backends (CLP/CBC)
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include <gtopt/solver_backend.hpp>

#include "osi_solver_backend.hpp"

namespace
{

// Null-terminated array of solver names this plugin provides
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
const char* const k_solver_names[] = {
    "clp",
#ifdef GTOPT_OSI_HAS_CBC
    "cbc",
#endif
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
  return "osi";
}

const char* const* gtopt_solver_names()  // NOLINT
{
  return k_solver_names;  // NOLINT
}

gtopt::SolverBackend* gtopt_create_backend(  // NOLINT
    const char* solver_name)
{
  using gtopt::OsiSolverBackend;

  if (std::strcmp(solver_name, "clp") == 0) {
    return new OsiSolverBackend(  // NOLINT
        OsiSolverBackend::OsiSolverType::clp);
  }
#ifdef GTOPT_OSI_HAS_CBC
  if (std::strcmp(solver_name, "cbc") == 0) {
    return new OsiSolverBackend(  // NOLINT
        OsiSolverBackend::OsiSolverType::cbc);
  }
#endif
  return nullptr;
}

// Plugin-level infinity query (no instance allocated).  COIN-OR's
// `OsiSolverInterface::getInfinity()` returns 1e+30 for both CLP
// and CBC; `OsiSolverBackend::plugin_infinity()` reports the same
// constant so callers using `is_infinity(v, plugin_infinity)` see
// identical behaviour against an instance-level call.
double gtopt_solver_infinity(const char* solver_name)  // NOLINT
{
  if (std::strcmp(solver_name, "clp") == 0
#ifdef GTOPT_OSI_HAS_CBC
      || std::strcmp(solver_name, "cbc") == 0
#endif
  )
  {
    return gtopt::OsiSolverBackend::plugin_infinity();
  }
  return 0.0;
}
}
