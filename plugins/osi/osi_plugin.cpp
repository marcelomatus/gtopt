/**
 * @file      osi_plugin.cpp
 * @brief     Plugin entry point for OSI solver backends (CLP/CBC)
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include "osi_solver_backend.hpp"

namespace
{

// Null-terminated array of solver names this plugin provides
const char* const k_solver_names[] = {
    // NOLINT
    "clp",
#ifdef GTOPT_OSI_HAS_CBC
    "cbc",
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
  return nullptr;
}
}
