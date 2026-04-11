// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_planning_options.cpp
 * @brief     Unit tests for SDDP/planning mode parsing, factory functions,
 *            and solver infrastructure
 * @date      2026-04-05
 */

#include <cmath>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/validate_planning.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("parse_cut_sharing_mode")  // NOLINT
{
  CHECK(parse_cut_sharing_mode("none") == CutSharingMode::none);
  CHECK(parse_cut_sharing_mode("expected") == CutSharingMode::expected);
  CHECK(parse_cut_sharing_mode("accumulate") == CutSharingMode::accumulate);
  CHECK(parse_cut_sharing_mode("max") == CutSharingMode::max);
  // Unknown defaults to none (matching SDDPOptions default)
  CHECK(parse_cut_sharing_mode("unknown") == CutSharingMode::none);
}

TEST_CASE("parse_elastic_filter_mode")  // NOLINT
{
  // Canonical names (underscore)
  CHECK(parse_elastic_filter_mode("single_cut")
        == ElasticFilterMode::single_cut);
  CHECK(parse_elastic_filter_mode("multi_cut") == ElasticFilterMode::multi_cut);
  CHECK(parse_elastic_filter_mode("backpropagate")
        == ElasticFilterMode::backpropagate);
  // Backward-compat alias ("cut" falls through to single_cut default)
  CHECK(parse_elastic_filter_mode("cut") == ElasticFilterMode::single_cut);
  // Unknown string also falls through to single_cut
  CHECK(parse_elastic_filter_mode("unknown") == ElasticFilterMode::single_cut);
}

// ─── Solver interface tests ─────────────────────────────────────────────────

TEST_CASE("MonolithicMethod - solves single-phase problem")  // NOLINT
{
  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("MonolithicMethod - solves 3-phase problem")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("make_planning_method factory - monolithic")  // NOLINT
{
  const PlanningOptionsLP options_lp;
  auto solver = make_planning_method(options_lp);
  REQUIRE(solver != nullptr);

  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  auto result = solver->solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("make_planning_method factory - sddp")  // NOLINT
{
  PlanningOptions opts;
  opts.method = MethodType::sddp;
  const PlanningOptionsLP options_lp(std::move(opts));
  auto solver = make_planning_method(options_lp);
  REQUIRE(solver != nullptr);
}

TEST_CASE("PlanningLP::resolve uses method option")  // NOLINT
{
  auto planning = make_single_phase_planning();
  // Default method is "monolithic"
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("PlanningOptions method and sddp_cut_sharing_mode")  // NOLINT
{
  PlanningOptions opts;
  opts.method = MethodType::sddp;
  opts.sddp_options.cut_sharing_mode = CutSharingMode::expected;

  const PlanningOptionsLP options_lp(std::move(opts));
  CHECK(options_lp.method_type_enum() == MethodType::sddp);
  CHECK(options_lp.sddp_cut_sharing_mode() == "expected");
}

TEST_CASE("PlanningOptions method defaults")  // NOLINT
{
  const PlanningOptionsLP options_lp;
  CHECK(options_lp.method_type_enum() == MethodType::monolithic);
  CHECK(options_lp.sddp_cut_sharing_mode() == "none");
}

TEST_CASE("PlanningOptions top-level method")  // NOLINT
{
  PlanningOptions opts;
  opts.method = MethodType::sddp;

  const PlanningOptionsLP options_lp(std::move(opts));
  CHECK(options_lp.method_type_enum() == MethodType::sddp);
}

TEST_CASE("PlanningOptions method from JSON top-level field")  // NOLINT
{
  // Verify that "method": "sddp" in the top-level options block is
  // correctly parsed — this is the only supported way to select the solver.
  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "sddp"
    }
  }
  )json";

  const auto planning =
      daw::json::from_json<Planning>(json_str);  // NOLINT(misc-include-cleaner)
  const PlanningOptionsLP options_lp(planning.options);
  CHECK(options_lp.method_type_enum() == MethodType::sddp);
}

// ─── Solver infrastructure tests ────────────────────────────────────────────

TEST_CASE("SDDPMethod API - monitoring API stop-request file")  // NOLINT
{
  // Verify that the solver stops gracefully when the monitoring API
  // stop-request file (sddp_stop_request.json) is created in the tmp dir.
  const auto tmp_dir =
      std::filesystem::temp_directory_path() / "test_sddp_api_stop_request";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto stop_request_path = tmp_dir / sddp_file::stop_request;

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 100;
  sddp_opts.convergence_tol = 1e-12;  // very tight — won't converge in 2
  sddp_opts.api_stop_request_file = stop_request_path.string();

  SDDPMethod sddp(planning_lp, sddp_opts);

  // Create the stop-request file after the first iteration via callback
  sddp.set_iteration_callback(
      [&stop_request_path](const SDDPIterationResult& r) -> bool
      {
        if (r.iteration_index >= 1) {
          std::ofstream ofs(stop_request_path);
          ofs << R"({"stop_requested":true})" << '\n';
        }
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // Should stop after ≤ 2 iterations + 1 final forward pass
  CHECK(results->size() <= 3);

  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("make_solver_work_pool creates a working pool")  // NOLINT
{
  auto pool = make_solver_work_pool();
  REQUIRE(pool != nullptr);

  // Submit a simple task and verify it executes
  auto fut = pool->submit([] { return 42; });
  REQUIRE(fut.has_value());
  CHECK(fut->get() == 42);

  // Check statistics are available
  const auto stats = pool->get_statistics();
  CHECK(stats.tasks_submitted >= 1);
}

TEST_CASE("make_solver_work_pool with custom cpu_factor")  // NOLINT
{
  // Use a small cpu_factor to verify it parameterises correctly
  auto pool = make_solver_work_pool(0.5);
  REQUIRE(pool != nullptr);

  auto fut = pool->submit([] { return 7; });
  REQUIRE(fut.has_value());
  CHECK(fut->get() == 7);
}

TEST_CASE("SDDPIterationResult contains timing information")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-3;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // Every iteration should have non-negative timing
  for (const auto& ir : *results) {
    CHECK(ir.forward_pass_s >= 0.0);
    CHECK(ir.backward_pass_s >= 0.0);
    CHECK(ir.iteration_s >= 0.0);
    // iteration_s should be >= forward + backward
    CHECK(ir.iteration_s
          >= doctest::Approx(ir.forward_pass_s + ir.backward_pass_s)
                 .epsilon(0.01));
  }
}

TEST_CASE("SDDPMethod API - status file contains timing fields")  // NOLINT
{
  const auto tmp_dir =
      std::filesystem::temp_directory_path() / "test_sddp_timing_status";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto status_file = (tmp_dir / "solver_status.json").string();

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = true;
  sddp_opts.api_status_file = status_file;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // The status file should exist and contain timing fields
  CHECK(std::filesystem::exists(status_file));
  if (std::filesystem::exists(status_file)) {
    std::ifstream ifs(status_file);
    const std::string content(std::istreambuf_iterator<char>(ifs), {});
    CHECK(content.find("forward_pass_s") != std::string::npos);
    CHECK(content.find("backward_pass_s") != std::string::npos);
    CHECK(content.find("iteration_s") != std::string::npos);
    CHECK(content.find("elapsed_s") != std::string::npos);
    CHECK(content.find("realtime") != std::string::npos);
  }

  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("MonolithicMethod uses work pool from factory")  // NOLINT
{
  // Verify that MonolithicMethod works correctly after the refactoring
  // to use make_solver_work_pool()
  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("MonolithicMethod with 3-phase uses work pool")  // NOLINT
{
  // Verify multi-phase monolithic solving after refactoring
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}
