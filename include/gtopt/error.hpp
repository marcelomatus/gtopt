#pragma once

#include <map>
#include <string>

namespace gtopt
{

enum class ErrorCode
{
  Success = 0,
  SolverError,
  InternalError,
  InvalidInput,
  FileIOError
};

struct Error
{
  ErrorCode code;
  std::string message;
};

}  // namespace gtopt
