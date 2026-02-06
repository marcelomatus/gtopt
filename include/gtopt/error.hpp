/**
 * @file      error.hpp
 * @brief     Header of
 * @date      Fri Jun 27 18:54:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once
#include <cstdint>
#include <string>

namespace gtopt
{

enum class ErrorCode : uint8_t
{
  Success = 0,
  SolverError,
  InternalError,
  InvalidInput,
  FileIOError,
};

struct Error
{
  ErrorCode code;
  std::string message {};
  int status {0};  // Optional status code for additional context
};

}  // namespace gtopt
