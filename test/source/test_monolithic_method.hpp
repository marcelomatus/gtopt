/**
 * @file      test_monolithic_method.hpp
 * @brief     Unit tests for MonolithicMethod::solve() and related paths
 * @date      2026-03-30
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. MonolithicMethod default construction and member defaults
 *  2. MonolithicMethod single-bus solve with kappa tracking
 *  3. Kappa threshold warning with KappaWarningMode::warn
 *  4. Kappa threshold with KappaWarningMode::none (skip check)
 *  5. Probability validation integration
 *  6. MonolithicMethod with lp_debug enabled
 *  7. Multi-scenario monolithic solve
 *  8. SolveMode enum entries
 */

#pragma once

#include <filesystem>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/validate_planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build a minimal single-bus, single-stage planning for monolithic
/// tests.
auto make_monolithic_test_planning(
    std::optional<KappaWarningMode> kappa_mode = std::nullopt,
    std::optional<double> kappa_threshold = std::nullopt) -> Planning
{
  Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
  };

  if (kappa_mode.has_value()) {
    simulation.kappa_warning = *kappa_mode;
  }
  if (kappa_threshold.has_value()) {
    simulation.kappa_threshold = *kappa_threshold;
  }

  const System system = {
      .name = "monolithic_test",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = 50.0,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 200.0,
              },
          },
  };

  PlanningOptions options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = system,
  };
}

/// Build a multi-scenario planning for parallel scene solving.
auto make_multi_scenario_planning() -> Planning
{
  Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.6,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.4,
              },
          },
  };

  const System system = {
      .name = "multi_scenario_mono",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = 80.0,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 15.0,
                  .capacity = 300.0,
              },
          },
  };

  PlanningOptions options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = system,
  };
}

}  // namespace

// ---------------------------------------------------------------
// MonolithicMethod default construction
// ---------------------------------------------------------------

TEST_CASE("MonolithicMethod - default member values")  // NOLINT
{
  const MonolithicMethod method;

  CHECK_FALSE(method.enable_api);
  CHECK(method.api_status_file.empty());
  CHECK(method.api_update_interval == std::chrono::milliseconds {500});
  CHECK_FALSE(method.lp_debug);
  CHECK(method.lp_debug_directory.empty());
  CHECK(method.lp_debug_compression.empty());
  CHECK(method.solve_mode == SolveMode::monolithic);
  CHECK(method.boundary_cuts_file.empty());
  CHECK(method.boundary_cuts_mode == BoundaryCutsMode::separated);
  CHECK(method.boundary_max_iterations == 0);
}

// ---------------------------------------------------------------
// Kappa monitoring - KappaWarningMode::warn
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "MonolithicMethod - kappa tracking with warn mode")
{
  auto planning = make_monolithic_test_planning(KappaWarningMode::warn, 1e9);
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);

  const auto& summary = planning_lp.sddp_summary();
  CHECK(summary.max_kappa >= 0.0);
}

// ---------------------------------------------------------------
// Kappa monitoring - KappaWarningMode::none
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "MonolithicMethod - kappa_warning=none skips check")
{
  auto planning = make_monolithic_test_planning(KappaWarningMode::none, 1.0);
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);

  const auto& summary = planning_lp.sddp_summary();
  CHECK(summary.max_kappa == doctest::Approx(1.0));
}

// ---------------------------------------------------------------
// Kappa monitoring - KappaWarningMode::save_lp
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "MonolithicMethod - kappa_warning=save_lp with low threshold")
{
  auto planning = make_monolithic_test_planning(KappaWarningMode::save_lp, 1.0);
  PlanningLP planning_lp(std::move(planning));

  const auto lp_dir =
      (std::filesystem::temp_directory_path() / "mono_kappa_lp_test").string();

  MonolithicMethod solver;
  solver.lp_debug_directory = lp_dir;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);

  const auto& summary = planning_lp.sddp_summary();
  CHECK(summary.max_kappa >= 0.0);

  std::filesystem::remove_all(lp_dir);
}

// ---------------------------------------------------------------
// Probability validation integration
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "MonolithicMethod - probability validation (sum to 1)")
{
  auto planning = make_multi_scenario_planning();

  auto vr = validate_planning(planning);
  CHECK(vr.ok());

  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);
}

TEST_CASE(  // NOLINT
    "MonolithicMethod - probability rescaling for non-unit sums")
{
  auto planning = make_multi_scenario_planning();
  planning.simulation.scenario_array[0].probability_factor = 3.0;
  planning.simulation.scenario_array[1].probability_factor = 7.0;

  planning.simulation.probability_rescale = ProbabilityRescaleMode::build;

  auto vr = validate_planning(planning);
  CHECK(vr.ok());

  const auto p0 =
      planning.simulation.scenario_array[0].probability_factor.value_or(0.0);
  const auto p1 =
      planning.simulation.scenario_array[1].probability_factor.value_or(0.0);
  CHECK(p0 + p1 == doctest::Approx(1.0));
  CHECK(p0 == doctest::Approx(0.3));
  CHECK(p1 == doctest::Approx(0.7));

  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);
}

// ---------------------------------------------------------------
// MonolithicMethod with lp_debug enabled
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "MonolithicMethod - lp_debug writes LP files")
{
  auto planning = make_monolithic_test_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto lp_dir =
      (std::filesystem::temp_directory_path() / "mono_lp_debug_test").string();

  MonolithicMethod solver;
  solver.lp_debug = true;
  solver.lp_debug_directory = lp_dir;
  solver.lp_debug_compression = "uncompressed";

  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);

  CHECK(std::filesystem::exists(lp_dir));

  std::filesystem::remove_all(lp_dir);
}

// ---------------------------------------------------------------
// Multi-scenario monolithic solve
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "MonolithicMethod - multi-scenario parallel solve")
{
  auto planning = make_multi_scenario_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(result.value_or(0) >= 1);

  const auto& summary = planning_lp.sddp_summary();
  CHECK(summary.max_kappa >= 0.0);
}

// ---------------------------------------------------------------
// SolveMode enum entries
// ---------------------------------------------------------------

TEST_CASE("SolveMode enum_entries - all entries present")  // NOLINT
{
  const auto entries = enum_entries(SolveMode {});

  CHECK(entries.size() == 2);
  CHECK(entries[0].name == "monolithic");
  CHECK(entries[0].value == SolveMode::monolithic);
  CHECK(entries[1].name == "sequential");
  CHECK(entries[1].value == SolveMode::sequential);
}

TEST_CASE("SolveMode enum_from_name round-trip")  // NOLINT
{
  const auto mono = enum_from_name<SolveMode>("monolithic");
  CHECK(mono == SolveMode::monolithic);

  const auto seq = enum_from_name<SolveMode>("sequential");
  CHECK(seq == SolveMode::sequential);
}
