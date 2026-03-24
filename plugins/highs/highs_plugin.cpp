/**
 * @file      highs_plugin.cpp
 * @brief     Plugin entry point for HiGHS solver backend
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include "highs_solver_backend.hpp"

namespace
{

const char* const k_solver_names[] = {
    "highs",
    nullptr,
};

}  // namespace

extern "C"
{
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
}
