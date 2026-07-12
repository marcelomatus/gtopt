// SPDX-License-Identifier: BSD-3-Clause
#include <algorithm>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/solution_csv.hpp>

using namespace gtopt;

namespace
{
/// Count comma-separated fields in one CSV record, stripping a trailing
/// newline first.  Empty record → 0 fields.
std::size_t count_csv_fields(std::string_view line)
{
  if (!line.empty() && line.back() == '\n') {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return 0;
  }
  return static_cast<std::size_t>(std::ranges::count(line, ',')) + 1;
}

SolutionRow make_row(int status)
{
  return SolutionRow {
      .scene_uid = make_uid<Scene>(3),
      .phase_uid = make_uid<Phase>(7),
      .status = status,
      .obj_value = 1234.5,
      .kappa = 2.0,
      .max_kappa = 9.0,
      .gap = 0.01,
      .gap_change = 0.5,
      .solve_ticks = 100.0,
      .solve_time_s = 0.25,
      .solve_calls = 4,
      .infeasible_count = 0,
  };
}
}  // namespace

TEST_CASE("solution.csv header and row field counts stay in sync")  // NOLINT
{
  // This is the drift guard: adding a SolutionRow field without updating both
  // the header and the format string would break this invariant.
  const auto header_fields = count_csv_fields(solution_csv_header());
  const auto row_fields = count_csv_fields(solution_csv_line(make_row(0)));

  CHECK(header_fields == 13);
  CHECK(row_fields == header_fields);
}

TEST_CASE("solution_status_name maps CLP status codes")  // NOLINT
{
  CHECK(solution_status_name(0) == "optimal");
  CHECK(solution_status_name(1) == "infeasible");
  CHECK(solution_status_name(2) == "unbounded");
  CHECK(solution_status_name(3) == "iteration_limit");
  CHECK(solution_status_name(4) == "error");

  SUBCASE("out-of-range and the never-solved sentinel report unknown")
  {
    CHECK(solution_status_name(-1) == "unknown");
    CHECK(solution_status_name(5) == "unknown");
    CHECK(solution_status_name(99) == "unknown");
  }
}

TEST_CASE(
    "solution_csv_line emits the status name in the 4th column")  // NOLINT
{
  const auto line = solution_csv_line(make_row(0));

  // Columns: scene,phase,status,status_name,...
  auto first = line.find(',');
  auto second = line.find(',', first + 1);
  auto third = line.find(',', second + 1);
  auto fourth = line.find(',', third + 1);
  const auto status_name_field = line.substr(third + 1, fourth - (third + 1));

  CHECK(status_name_field == "optimal");
  CHECK(line.back() == '\n');  // trailing newline preserved

  SUBCASE("a never-solved cell renders status -1 as unknown")
  {
    const auto bad = solution_csv_line(make_row(-1));
    CHECK(bad.find(",unknown,") != std::string::npos);
  }
}
