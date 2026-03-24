/**
 * @file      osi_plugin.cpp
 * @brief     Plugin entry point for OSI solver backends (CLP/CBC/CPLEX)
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>
#include <stdexcept>

#include "osi_solver_backend.hpp"

// NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)

namespace
{

// Null-terminated array of solver names this plugin provides
const char* const k_solver_names[] = {
    // NOLINT
    "clp",
#ifdef GTOPT_OSI_HAS_CBC
    "cbc",
#endif
#ifdef GTOPT_OSI_HAS_CPX
    "cplex",
#endif
    nullptr,
};

}  // namespace

extern "C"
{
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
#ifdef GTOPT_OSI_HAS_CPX
  if (std::strcmp(solver_name, "cplex") == 0) {
    return new OsiSolverBackend(  // NOLINT
        OsiSolverBackend::OsiSolverType::cplex);
  }
#endif
  return nullptr;
}
}

// NOLINTEND(cppcoreguidelines-pro-type-const-cast)
