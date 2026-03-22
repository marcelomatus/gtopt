/**
 * @file      test_sddp_monitor.hpp
 * @brief     Unit tests for the SDDP monitoring API
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. SDDPStatusSnapshot default construction
 *  2. SDDPIterationResult default construction
 *  3. write_sddp_api_status produces valid JSON with expected keys
 *  4. write_sddp_api_status handles empty results vector
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/sddp_monitor.hpp>
#include <gtopt/solver_monitor.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── SDDPStatusSnapshot tests ───────────────────────────────────────────────

TEST_CASE("SDDPStatusSnapshot default construction")  // NOLINT
{
  const SDDPStatusSnapshot snap {};

  CHECK(snap.iteration == 0);
  CHECK(snap.gap == doctest::Approx(0.0));
  CHECK(snap.lower_bound == doctest::Approx(0.0));
  CHECK(snap.upper_bound == doctest::Approx(0.0));
  CHECK_FALSE(snap.converged);
  CHECK(snap.max_iterations == 0);
  CHECK(snap.min_iterations == 0);
}

TEST_CASE("SDDPStatusSnapshot with custom values")  // NOLINT
{
  const SDDPStatusSnapshot snap {
      .iteration = 5,
      .gap = 0.01,
      .lower_bound = 1000.0,
      .upper_bound = 1010.0,
      .converged = true,
      .max_iterations = 100,
      .min_iterations = 2,
  };

  CHECK(snap.iteration == 5);
  CHECK(snap.gap == doctest::Approx(0.01));
  CHECK(snap.lower_bound == doctest::Approx(1000.0));
  CHECK(snap.upper_bound == doctest::Approx(1010.0));
  CHECK(snap.converged);
  CHECK(snap.max_iterations == 100);
  CHECK(snap.min_iterations == 2);
}

// ─── SDDPIterationResult default construction ───────────────────────────────

TEST_CASE("SDDPIterationResult default construction")  // NOLINT
{
  const SDDPIterationResult result {};

  CHECK(result.iteration == 0);
  CHECK(result.lower_bound == doctest::Approx(0.0));
  CHECK(result.upper_bound == doctest::Approx(0.0));
  CHECK(result.gap == doctest::Approx(0.0));
  CHECK_FALSE(result.converged);
  CHECK(result.cuts_added == 0);
  CHECK_FALSE(result.feasibility_issue);
  CHECK(result.forward_pass_s == doctest::Approx(0.0));
  CHECK(result.backward_pass_s == doctest::Approx(0.0));
  CHECK(result.iteration_s == doctest::Approx(0.0));
  CHECK(result.infeasible_cuts_added == 0);
  CHECK(result.scene_upper_bounds.empty());
  CHECK(result.scene_lower_bounds.empty());
}

// ─── write_sddp_api_status tests ────────────────────────────────────────────

TEST_CASE("write_sddp_api_status produces valid JSON")  // NOLINT
{
  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_sddp_status.json")
          .string();

  const SDDPStatusSnapshot snap {
      .iteration = 3,
      .gap = 0.05,
      .lower_bound = 950.0,
      .upper_bound = 1000.0,
      .converged = false,
      .max_iterations = 100,
      .min_iterations = 2,
  };

  std::vector<SDDPIterationResult> results;
  results.push_back(SDDPIterationResult {
      .iteration = IterationIndex {1},
      .lower_bound = 800.0,
      .upper_bound = 1200.0,
      .gap = 0.33,
      .converged = false,
      .cuts_added = 2,
      .feasibility_issue = false,
      .forward_pass_s = 0.5,
      .backward_pass_s = 0.3,
      .iteration_s = 0.8,
      .infeasible_cuts_added = 0,
      .scene_upper_bounds =
          {
              1200.0,
          },
      .scene_lower_bounds =
          {
              800.0,
          },
  });
  results.push_back(SDDPIterationResult {
      .iteration = IterationIndex {2},
      .lower_bound = 900.0,
      .upper_bound = 1050.0,
      .gap = 0.14,
      .converged = false,
      .cuts_added = 2,
      .feasibility_issue = false,
      .forward_pass_s = 0.4,
      .backward_pass_s = 0.2,
      .iteration_s = 0.6,
      .infeasible_cuts_added = 0,
      .scene_upper_bounds =
          {
              1050.0,
          },
      .scene_lower_bounds =
          {
              900.0,
          },
  });

  // Default-constructed SolverMonitor (no background thread started)
  SolverMonitor monitor;

  write_sddp_api_status(tmp_file, results, 2.5, snap, monitor);
  CHECK(std::filesystem::exists(tmp_file));

  // Read the file content
  std::ifstream ifs(tmp_file);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  const auto content = oss.str();

  // Verify key JSON fields are present
  CHECK(content.find("\"version\": 1") != std::string::npos);
  CHECK(content.find("\"iteration\": 3") != std::string::npos);
  CHECK(content.find("\"gap\": 0.05") != std::string::npos);
  CHECK(content.find("\"converged\": false") != std::string::npos);
  CHECK(content.find("\"max_iterations\": 100") != std::string::npos);
  CHECK(content.find("\"min_iterations\": 2") != std::string::npos);
  CHECK(content.find("\"lower_bound\": 950.0") != std::string::npos);
  CHECK(content.find("\"upper_bound\": 1000.0") != std::string::npos);
  CHECK(content.find("\"status\": \"running\"") != std::string::npos);

  // Verify history array is present with iteration entries
  CHECK(content.find("\"history\"") != std::string::npos);
  CHECK(content.find("\"iteration\": 1") != std::string::npos);
  CHECK(content.find("\"iteration\": 2") != std::string::npos);

  // Verify realtime monitoring block is present
  CHECK(content.find("\"realtime\"") != std::string::npos);

  // Clean up
  std::filesystem::remove(tmp_file);
}

TEST_CASE("write_sddp_api_status handles empty results")  // NOLINT
{
  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_sddp_status_empty.json")
                            .string();

  const SDDPStatusSnapshot snap {
      .iteration = 0,
      .converged = false,
      .max_iterations = 50,
  };

  const std::vector<SDDPIterationResult> results;
  SolverMonitor monitor;

  write_sddp_api_status(tmp_file, results, 0.0, snap, monitor);
  CHECK(std::filesystem::exists(tmp_file));

  std::ifstream ifs(tmp_file);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  const auto content = oss.str();

  // With iteration=0, status should be "initializing"
  CHECK(content.find("\"status\": \"initializing\"") != std::string::npos);
  CHECK(content.find("\"history\": [") != std::string::npos);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "write_sddp_api_status shows converged status when "
    "converged")  // NOLINT
{
  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_sddp_status_converged.json")
                            .string();

  const SDDPStatusSnapshot snap {
      .iteration = 10,
      .gap = 1e-5,
      .lower_bound = 999.99,
      .upper_bound = 1000.0,
      .converged = true,
      .max_iterations = 100,
      .min_iterations = 2,
  };

  const std::vector<SDDPIterationResult> results;
  SolverMonitor monitor;

  write_sddp_api_status(tmp_file, results, 5.0, snap, monitor);

  std::ifstream ifs(tmp_file);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  const auto content = oss.str();

  CHECK(content.find("\"status\": \"converged\"") != std::string::npos);
  CHECK(content.find("\"converged\": true") != std::string::npos);

  std::filesystem::remove(tmp_file);
}
