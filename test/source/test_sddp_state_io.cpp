/**
 * @file      test_sddp_state_io.hpp
 * @brief     Unit tests for SDDP state variable I/O (save only)
 * @date      Mon Mar 24 2026
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. save_state_csv writes a valid CSV with header and data
 *  2. save_state_csv with unsolved LP writes header only (no data rows)
 *
 * The CSV is a pure tabular file written via Arrow's CSV writer; the
 * producing iteration is identified by the surrounding log and on-disk
 * mtime, not by an in-file marker.
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_state_io.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

auto make_state_test_dir(const std::string& test_name) -> std::filesystem::path
{
  auto dir = std::filesystem::temp_directory_path()
      / ("gtopt_test_state_io_" + test_name);
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

// ─── save_state_csv tests ───────────────────────────────────────────────────

TEST_CASE("save_state_csv writes valid CSV after SDDP solve")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Run a few SDDP iterations to get optimal solutions
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto dir = make_state_test_dir("save_csv");
  const auto filepath = (dir / "state.csv").string();

  auto save_result = save_state_csv(planning_lp, filepath);
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(filepath));

  // Verify file structure
  std::ifstream ifs(filepath);
  std::string line;

  // First line: CSV header (pure tabular CSV — no leading comment).
  std::getline(ifs, line);
  CHECK(line == R"("name","phase","scene","value","rcost")");

  // Should have at least one data line (efin, eini, sini columns)
  std::getline(ifs, line);
  CHECK_FALSE(line.empty());

  std::filesystem::remove_all(dir);
}

TEST_CASE(  // NOLINT
    "save_state_csv with unsolved LP writes header but no data rows")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto dir = make_state_test_dir("unsolved");
  const auto filepath = (dir / "state_unsolved.csv").string();

  auto save_result = save_state_csv(planning_lp, filepath);
  REQUIRE(save_result.has_value());

  // Count data lines (skip header).  Arrow quotes column names, so the
  // header begins with a double-quote.
  std::ifstream ifs(filepath);
  std::string line;
  int data_lines = 0;
  while (std::getline(ifs, line)) {
    if (!line.empty() && !line.starts_with('"')) {
      ++data_lines;
    }
  }
  CHECK(data_lines == 0);

  std::filesystem::remove_all(dir);
}
