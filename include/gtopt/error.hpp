#pragma once

#include <map>
#include <string>
#include <system_error>

namespace gtopt {

enum class ErrorCode {
    Success = 0,
    SolverError,
    InternalError,
    InvalidInput,
    FileIOError
};

struct Error {
    ErrorCode code;
    std::string message;
    std::map<std::string, std::string> context;
};

} // namespace gtopt
