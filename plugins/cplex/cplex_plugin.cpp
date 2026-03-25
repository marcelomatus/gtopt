/**
 * @file      cplex_plugin.cpp
 * @brief     Plugin entry point for CPLEX solver backend
 * @date      Tue Mar 25 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include "cplex_solver_backend.hpp"

namespace
{

const char* const k_solver_names[] = {
    "cplex",
    nullptr,
};

}  // namespace

extern "C"
{
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
}
