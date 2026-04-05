/**
 * @file      test_sddp_state_io.hpp
 * @brief     Unit tests for SDDP state variable I/O (save/load round-trip)
 * @date      Mon Mar 24 2026
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. save_state_csv writes a valid CSV with header and data
 *  2. save_state_csv / load_state_csv round-trip preserves warm solutions
 *  3. load_state_csv with nonexistent file returns error
 *  4. save_state_csv with unsolved LP writes header only (no data rows)
 *  5. load_state_csv with empty file is a no-op
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

  auto save_result = save_state_csv(planning_lp, filepath, IterationIndex {2});
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(filepath));

  // Verify file structure
  std::ifstream ifs(filepath);
  std::string line;

  // First line: iteration comment
  std::getline(ifs, line);
  CHECK(line.starts_with("# iteration="));

  // Second line: CSV header
  std::getline(ifs, line);
  CHECK(line == "name,phase,scene,value,rcost");

  // Should have at least one data line (efin, eini, sini columns)
  std::getline(ifs, line);
  CHECK_FALSE(line.empty());

  std::filesystem::remove_all(dir);
}

TEST_CASE(  // NOLINT
    "save_state_csv / load_state_csv round-trip preserves warm solutions")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Solve to get optimal column values
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto dir = make_state_test_dir("round_trip");
  const auto filepath = (dir / "state_rt.csv").string();

  // Save state
  auto save_result = save_state_csv(planning_lp, filepath, IterationIndex {0});
  REQUIRE(save_result.has_value());

  // Create a fresh PlanningLP (unsolved) and load the saved state
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  auto load_result = load_state_csv(planning_lp2, filepath);
  REQUIRE(load_result.has_value());

  // Verify that warm solutions were injected: the LinearInterface should
  // have non-empty warm_col_sol for at least one (scene, phase)
  bool found_warm = false;
  const auto& sim = planning_lp2.simulation();
  for (auto&& [si, _sc] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, _ph] : enumerate<PhaseIndex>(sim.phases())) {
      const auto& li = planning_lp2.system(si, pi).linear_interface();
      if (!li.warm_col_sol().empty()) {
        found_warm = true;
        break;
      }
    }
    if (found_warm) {
      break;
    }
  }
  CHECK(found_warm);

  std::filesystem::remove_all(dir);
}

TEST_CASE("load_state_csv with nonexistent file returns error")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  auto result = load_state_csv(planning_lp, "/tmp/nonexistent_state_xyz.csv");
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::FileIOError);
}

TEST_CASE(  // NOLINT
    "save_state_csv with unsolved LP writes header but no data rows")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto dir = make_state_test_dir("unsolved");
  const auto filepath = (dir / "state_unsolved.csv").string();

  auto save_result = save_state_csv(planning_lp, filepath, IterationIndex {0});
  REQUIRE(save_result.has_value());

  // Count data lines (skip comment and header)
  std::ifstream ifs(filepath);
  std::string line;
  int data_lines = 0;
  while (std::getline(ifs, line)) {
    if (!line.empty() && !line.starts_with('#') && !line.starts_with("name,")) {
      ++data_lines;
    }
  }
  CHECK(data_lines == 0);

  std::filesystem::remove_all(dir);
}

TEST_CASE(  // NOLINT
    "load_state_csv with empty/header-only file is a no-op")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto dir = make_state_test_dir("empty_load");
  const auto filepath = (dir / "state_empty.csv").string();

  // Write a file with only a header
  {
    std::ofstream ofs(filepath);
    ofs << "name,phase,scene,value,rcost\n";
  }

  auto load_result = load_state_csv(planning_lp, filepath);
  REQUIRE(load_result.has_value());

  // No warm solutions should be injected
  const auto& sim = planning_lp.simulation();
  for (auto&& [si, _sc] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, _ph] : enumerate<PhaseIndex>(sim.phases())) {
      const auto& li = planning_lp.system(si, pi).linear_interface();
      CHECK(li.warm_col_sol().empty());
    }
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE(  // NOLINT
    "load_state_csv handles Windows \\r\\n line endings")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Solve to get optimal column values
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto dir = make_state_test_dir("crlf");
  const auto unix_file = (dir / "state_unix.csv").string();
  const auto dos_file = (dir / "state_dos.csv").string();

  // Save state (Unix line endings)
  auto save_result = save_state_csv(planning_lp, unix_file, IterationIndex {0});
  REQUIRE(save_result.has_value());

  // Convert to DOS line endings
  {
    std::ifstream ifs(unix_file);
    std::ofstream ofs(dos_file, std::ios::binary);
    std::string line;
    while (std::getline(ifs, line)) {
      ofs << line << "\r\n";
    }
  }

  // Load the DOS file into a fresh PlanningLP
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  auto load_result = load_state_csv(planning_lp2, dos_file);
  REQUIRE(load_result.has_value());

  // Verify warm solutions were loaded despite \r\n endings
  bool found_warm = false;
  const auto& sim = planning_lp2.simulation();
  for (auto&& [si, _sc] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, _ph] : enumerate<PhaseIndex>(sim.phases())) {
      const auto& li = planning_lp2.system(si, pi).linear_interface();
      if (!li.warm_col_sol().empty()) {
        found_warm = true;
        break;
      }
    }
    if (found_warm) {
      break;
    }
  }
  CHECK(found_warm);

  std::filesystem::remove_all(dir);
}
