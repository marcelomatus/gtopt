/**
 * @file      cuopt_plugin.cpp
 * @brief     Plugin entry point for the NVIDIA cuOpt (GPU) solver backend
 * @date      Tue Jun 10 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cstring>

#include <gtopt/solver_backend.hpp>

#include "cuopt_solver_backend.hpp"

namespace
{

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
const char* const k_solver_names[] = {
    "cuopt",
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
  return "cuopt";
}

const char* const* gtopt_solver_names()  // NOLINT
{
  return k_solver_names;  // NOLINT
}

gtopt::SolverBackend* gtopt_create_backend(  // NOLINT
    const char* solver_name)
{
  if (std::strcmp(solver_name, "cuopt") == 0) {
    return new gtopt::CuOptSolverBackend();  // NOLINT
  }
  return nullptr;
}

double gtopt_solver_infinity(const char* solver_name)  // NOLINT
{
  if (std::strcmp(solver_name, "cuopt") == 0) {
    return gtopt::CuOptSolverBackend::plugin_infinity();
  }
  return 0.0;
}
}
