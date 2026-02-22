#include <utility>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>

using namespace gtopt;

TEST_SUITE("Error")
{
  TEST_CASE("ErrorCode default values")
  {
    CHECK(std::to_underlying(ErrorCode::Success) == 0);
    CHECK(std::to_underlying(ErrorCode::SolverError) == 1);
    CHECK(std::to_underlying(ErrorCode::InternalError) == 2);
    CHECK(std::to_underlying(ErrorCode::InvalidInput) == 3);
    CHECK(std::to_underlying(ErrorCode::FileIOError) == 4);
  }

  TEST_CASE("Error default construction")
  {
    const Error err {};
    CHECK(err.code == ErrorCode::Success);
    CHECK(err.message.empty());
    CHECK(err.status == 0);
  }

  TEST_CASE("Error with values")
  {
    const Error err {
        .code = ErrorCode::SolverError,
        .message = "solver failed",
        .status = -1,
    };

    CHECK(err.code == ErrorCode::SolverError);
    CHECK(err.message == "solver failed");
    CHECK(err.status == -1);
  }

  TEST_CASE("Error copy semantics")
  {
    const Error err1 {
        .code = ErrorCode::InvalidInput,
        .message = "bad input",
        .status = 42,
    };

    const Error err2 =
        err1;  // NOLINT(performance-unnecessary-copy-initialization)
    CHECK(err2.code == ErrorCode::InvalidInput);
    CHECK(err2.message == "bad input");
    CHECK(err2.status == 42);
  }

  TEST_CASE("Error is_success and is_error")
  {
    const Error success_err {};
    CHECK(success_err.is_success());
    CHECK_FALSE(success_err.is_error());

    const Error solver_err {
        .code = ErrorCode::SolverError,
        .message = "failed",
    };
    CHECK_FALSE(solver_err.is_success());
    CHECK(solver_err.is_error());

    const Error io_err {
        .code = ErrorCode::FileIOError,
        .message = "disk full",
        .status = -1,
    };
    CHECK_FALSE(io_err.is_success());
    CHECK(io_err.is_error());
  }
}
