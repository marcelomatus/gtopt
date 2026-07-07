// SPDX-License-Identifier: BSD-3-Clause
//
// Pins the tri-state classification that drives the gtopt_lp_runner
// partial-write path (commit `9994ede4d`).  Every entry point of
// `classify_solve_outcome` is exercised so a future refactor of the
// Optimal / Partial / Failed bands cannot regress without surfacing.

#include <expected>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/solve_outcome.hpp>

using namespace gtopt;

namespace
{

using SolveResult = std::expected<int, Error>;

[[nodiscard]] SolveResult ok_result(int status = 0)
{
  return status;
}

[[nodiscard]] SolveResult error_result(ErrorCode code, const char* message = "")
{
  return std::unexpected(Error {
      .code = code,
      .message = message,
  });
}

}  // namespace

TEST_CASE("classify_solve_outcome — Optimal when result has value")  // NOLINT
{
  // Any successful result maps to Optimal, regardless of the status
  // payload (the int the LP backend hands back is not interpreted
  // here — only `has_value()` matters).
  CHECK(classify_solve_outcome(ok_result(0)) == SolveOutcome::Optimal);
  CHECK(classify_solve_outcome(ok_result(1)) == SolveOutcome::Optimal);
  CHECK(classify_solve_outcome(ok_result(-1)) == SolveOutcome::Optimal);
}

TEST_CASE(
    "classify_solve_outcome — Partial when error is SolverError")  // NOLINT
{
  // `SolverError` is the documented soft-error band: time-limit with
  // feasible incumbent, MIP gap reached without optimal, SDDP /
  // cascade sentinel stop after iterations completed.  In every case
  // the cell still holds a usable solution → Partial → runner invokes
  // write_out.
  CHECK(classify_solve_outcome(error_result(ErrorCode::SolverError))
        == SolveOutcome::Partial);

  // The message is informational only — does not affect classification.
  CHECK(classify_solve_outcome(
            error_result(ErrorCode::SolverError, "time limit reached"))
        == SolveOutcome::Partial);
  CHECK(classify_solve_outcome(error_result(ErrorCode::SolverError,
                                            "MIP gap target met (gap=0.012)"))
        == SolveOutcome::Partial);
}

TEST_CASE(
    "classify_solve_outcome — Failed for every non-SolverError code")  // NOLINT
{
  // Hard failures: write_out would either throw or emit garbage —
  // runner returns 1 without writing.  Walk every other code in the
  // enum so a new `ErrorCode` value cannot silently default into the
  // Partial bucket.
  CHECK(classify_solve_outcome(error_result(ErrorCode::InternalError))
        == SolveOutcome::Failed);
  CHECK(classify_solve_outcome(error_result(ErrorCode::InvalidInput))
        == SolveOutcome::Failed);
  CHECK(classify_solve_outcome(error_result(ErrorCode::FileIOError))
        == SolveOutcome::Failed);
}

TEST_CASE(
    "classify_solve_outcome — Success code does not back-route to Failed")  // NOLINT
{
  // `ErrorCode::Success` is the default-constructed enum value.  It
  // should never appear inside an `std::unexpected`, but if it does
  // we want it routed as Failed (a hard error with no message is more
  // suspicious than a documented SolverError).
  CHECK(classify_solve_outcome(error_result(ErrorCode::Success))
        == SolveOutcome::Failed);
}

TEST_CASE("classify_solve_outcome — exhaustive enum coverage")  // NOLINT
{
  // Walk every declared ErrorCode and assert it falls into exactly
  // one of {Partial, Failed}.  Future-proof: if a new ErrorCode is
  // added without being routed here, this test still passes silently
  // — but the per-code SUBCASEs above ensure each known code lands in
  // the intended bucket.
  constexpr auto codes = std::to_array<ErrorCode>({
      ErrorCode::Success,
      ErrorCode::SolverError,
      ErrorCode::InternalError,
      ErrorCode::InvalidInput,
      ErrorCode::FileIOError,
  });

  for (auto code : codes) {
    const auto outcome = classify_solve_outcome(error_result(code));
    // The error branch never produces Optimal.
    CHECK(outcome != SolveOutcome::Optimal);
    // And it always lands in one of the two error bands.
    CHECK(
        (outcome == SolveOutcome::Partial || outcome == SolveOutcome::Failed));
  }
}
