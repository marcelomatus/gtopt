/**
 * @file      test_lp_stats.hpp
 * @brief     Unit tests for ScenePhaseLPStats and log_lp_stats_summary
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/lp_stats.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ScenePhaseLPStats coeff_ratio")  // NOLINT
{
  SUBCASE("normal ratio")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 1000.0;
    stats.stats_min_abs = 0.01;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(stats.coeff_ratio() == doctest::Approx(100000.0));
  }

  SUBCASE("equal min and max returns 1.0")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 5.0;
    stats.stats_min_abs = 5.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 0;
    stats.stats_nnz = 10;

    CHECK(stats.coeff_ratio() == doctest::Approx(1.0));
  }

  SUBCASE("zero min_abs returns 1.0")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 100.0;
    stats.stats_min_abs = 0.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(stats.coeff_ratio() == doctest::Approx(1.0));
  }

  SUBCASE("negative min_col returns 1.0")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 100.0;
    stats.stats_min_abs = 1.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = -1;
    stats.stats_nnz = 10;

    CHECK(stats.coeff_ratio() == doctest::Approx(1.0));
  }

  SUBCASE("zero nnz returns 1.0")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 100.0;
    stats.stats_min_abs = 1.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 0;

    CHECK(stats.coeff_ratio() == doctest::Approx(1.0));
  }

  SUBCASE("max double min_abs returns 1.0")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 100.0;
    stats.stats_min_abs = std::numeric_limits<double>::max();
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(stats.coeff_ratio() == doctest::Approx(1.0));
  }
}

TEST_CASE("ScenePhaseLPStats quality_label")  // NOLINT
{
  SUBCASE("excellent - ratio <= 1e3")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 100.0;
    stats.stats_min_abs = 1.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(std::string_view(stats.quality_label()) == "excellent");
  }

  SUBCASE("good - ratio <= 1e5")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 10000.0;
    stats.stats_min_abs = 1.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(std::string_view(stats.quality_label()) == "good");
  }

  SUBCASE("fair - ratio <= 1e7")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 1e6;
    stats.stats_min_abs = 1.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(std::string_view(stats.quality_label()) == "fair");
  }

  SUBCASE("poor - ratio > 1e7")
  {
    ScenePhaseLPStats stats;
    stats.stats_max_abs = 1e9;
    stats.stats_min_abs = 1.0;
    stats.stats_max_col = 0;
    stats.stats_min_col = 1;
    stats.stats_nnz = 10;

    CHECK(std::string_view(stats.quality_label()) == "poor");
  }

  SUBCASE("default stats - ratio 1.0 is excellent")
  {
    const ScenePhaseLPStats stats;
    CHECK(std::string_view(stats.quality_label()) == "excellent");
  }
}

TEST_CASE("log_lp_stats_summary - empty entries")  // NOLINT
{
  // Should not crash with empty input
  const std::vector<ScenePhaseLPStats> empty;
  log_lp_stats_summary(empty);
}

TEST_CASE("log_lp_stats_summary - single entry")  // NOLINT
{
  std::vector<ScenePhaseLPStats> entries;
  entries.push_back({
      .scene_uid = 1,
      .phase_uid = 1,
      .num_vars = 100,
      .num_constraints = 50,
      .stats_nnz = 500,
      .stats_zeroed = 0,
      .stats_max_abs = 1000.0,
      .stats_min_abs = 1.0,
      .stats_max_col = 10,
      .stats_min_col = 20,
      .stats_max_col_name = "gen_p",
      .stats_min_col_name = "theta",
  });
  // Should log without error
  log_lp_stats_summary(entries);
}

TEST_CASE("log_lp_stats_summary - above threshold triggers detail")  // NOLINT
{
  std::vector<ScenePhaseLPStats> entries;
  entries.push_back({
      .scene_uid = 1,
      .phase_uid = 1,
      .num_vars = 100,
      .num_constraints = 50,
      .stats_nnz = 500,
      .stats_zeroed = 5,
      .stats_max_abs = 1e9,
      .stats_min_abs = 0.001,
      .stats_max_col = 10,
      .stats_min_col = 20,
      .stats_max_col_name = "gen_p",
      .stats_min_col_name = "theta",
  });
  // Should log detailed table (ratio > 1e7)
  log_lp_stats_summary(entries, 1e7);
}

TEST_CASE(  // NOLINT
    "log_lp_stats_summary - empty col names in detail logging")
{
  std::vector<ScenePhaseLPStats> entries;
  entries.push_back({
      .scene_uid = 1,
      .phase_uid = 1,
      .num_vars = 100,
      .num_constraints = 50,
      .stats_nnz = 500,
      .stats_zeroed = 0,
      .stats_max_abs = 1e10,
      .stats_min_abs = 0.0001,
      .stats_max_col = 5,
      .stats_min_col = 15,
      // empty names — exercises the branch without column names
  });
  // Ratio = 1e14 > 1e7 → triggers detailed output with empty col names
  log_lp_stats_summary(entries, 1e7);
}

TEST_CASE(  // NOLINT
    "log_lp_stats_summary - zeroed entries in summary line")
{
  std::vector<ScenePhaseLPStats> entries;
  entries.push_back({
      .scene_uid = 1,
      .phase_uid = 1,
      .num_vars = 50,
      .num_constraints = 25,
      .stats_nnz = 200,
      .stats_zeroed = 3,
      .stats_max_abs = 10.0,
      .stats_min_abs = 1.0,
      .stats_max_col = 0,
      .stats_min_col = 1,
      .stats_max_col_name = "x",
      .stats_min_col_name = "y",
  });
  // Ratio = 10 < 1e7 → summary line path, but with stats_zeroed > 0
  log_lp_stats_summary(entries, 1e7);
}

TEST_CASE(  // NOLINT
    "log_lp_stats_summary - multiple entries with global max update")
{
  std::vector<ScenePhaseLPStats> entries;
  entries.push_back({
      .scene_uid = 1,
      .phase_uid = 1,
      .num_vars = 50,
      .num_constraints = 25,
      .stats_nnz = 100,
      .stats_max_abs = 100.0,
      .stats_min_abs = 1.0,
      .stats_max_col = 0,
      .stats_min_col = 1,
      .stats_max_col_name = "a",
      .stats_min_col_name = "b",
  });
  entries.push_back({
      .scene_uid = 1,
      .phase_uid = 2,
      .num_vars = 50,
      .num_constraints = 25,
      .stats_nnz = 100,
      .stats_max_abs = 500.0,  // higher max
      .stats_min_abs = 0.5,  // lower min
      .stats_max_col = 2,
      .stats_min_col = 3,
      .stats_max_col_name = "c",
      .stats_min_col_name = "d",
  });
  // Global ratio = 500/0.5 = 1000 < 1e7 → summary line
  log_lp_stats_summary(entries, 1e7);
}

TEST_CASE(  // NOLINT
    "log_lp_stats_summary - detail with zeroed and multiple entries")
{
  std::vector<ScenePhaseLPStats> entries;
  entries.push_back({
      .scene_uid = 0,
      .phase_uid = 0,
      .num_vars = 50,
      .num_constraints = 25,
      .stats_nnz = 100,
      .stats_zeroed = 2,
      .stats_max_abs = 1e9,
      .stats_min_abs = 0.001,
      .stats_max_col = 0,
      .stats_min_col = 1,
      .stats_max_col_name = "x",
      .stats_min_col_name = "y",
  });
  entries.push_back({
      .scene_uid = 0,
      .phase_uid = 1,
      .num_vars = 50,
      .num_constraints = 25,
      .stats_nnz = 100,
      .stats_zeroed = 1,
      .stats_max_abs = 1e10,
      .stats_min_abs = 0.0001,
      .stats_max_col = 2,
      .stats_min_col = 3,
  });
  // Ratio > 1e7 → detail path, with global zeroed > 0
  log_lp_stats_summary(entries, 1e7);
}
