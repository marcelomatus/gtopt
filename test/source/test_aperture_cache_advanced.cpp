// SPDX-License-Identifier: BSD-3-Clause

/**
 * @file      test_aperture_cache_advanced.hpp
 * @brief     Advanced tests for ApertureDataCache: multi-stage, bulk loading
 * @date      2026-03-22
 */

#include <doctest/doctest.h>
#include <gtopt/aperture_data_cache.hpp>

#include "aperture_data_cache.hpp"

using namespace gtopt;
using aperture_test_helpers::TmpDir;
using aperture_test_helpers::write_test_parquet;

namespace
{

using namespace gtopt;

TEST_CASE("ApertureDataCache multi-stage multi-block lookup")  // NOLINT
{
  using namespace gtopt;

  const TmpDir tmp("test_aperture_cache_multistage");

  // 3 stages × 2 blocks × 2 scenarios
  write_test_parquet(tmp.path / "Flow" / "RIVER1.parquet",
                     {
                         0,
                         0,
                         1,
                         1,
                         2,
                         2,
                     },
                     {
                         0,
                         1,
                         0,
                         1,
                         0,
                         1,
                     },
                     {
                         {
                             1,
                             {
                                 10.0,
                                 11.0,
                                 20.0,
                                 21.0,
                                 30.0,
                                 31.0,
                             },
                         },
                         {
                             2,
                             {
                                 110.0,
                                 111.0,
                                 120.0,
                                 121.0,
                                 130.0,
                                 131.0,
                             },
                         },
                     });

  const ApertureDataCache cache(tmp.path);
  CHECK_FALSE(cache.empty());

  // Spot-check various (scenario, stage, block) combinations
  CHECK(cache
            .lookup("Flow",
                    "RIVER1",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(0),
                    make_uid<Block>(0))
            .value_or(0.0)
        == doctest::Approx(10.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER1",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(1),
                    make_uid<Block>(1))
            .value_or(0.0)
        == doctest::Approx(21.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER1",
                    make_uid<Scenario>(2),
                    make_uid<Stage>(2),
                    make_uid<Block>(0))
            .value_or(0.0)
        == doctest::Approx(130.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER1",
                    make_uid<Scenario>(2),
                    make_uid<Stage>(2),
                    make_uid<Block>(1))
            .value_or(0.0)
        == doctest::Approx(131.0));
}

TEST_CASE("ApertureDataCache many scenarios bulk loading")  // NOLINT
{
  using namespace gtopt;

  const TmpDir tmp("test_aperture_cache_bulk");

  // 10 scenarios × 5 stages × 2 blocks = 100 entries per element
  std::vector<int32_t> stages;
  std::vector<int32_t> blocks;
  for (int s = 0; s < 5; ++s) {
    for (int b = 0; b < 2; ++b) {
      stages.push_back(s);
      blocks.push_back(b);
    }
  }

  std::vector<std::pair<int, std::vector<double>>> uid_cols;
  for (int sc = 1; sc <= 10; ++sc) {
    std::vector<double> values;
    for (int s = 0; s < 5; ++s) {
      for (int b = 0; b < 2; ++b) {
        // Unique value: sc * 1000 + s * 10 + b
        values.push_back(static_cast<double>((sc * 1000) + (s * 10) + b));
      }
    }
    uid_cols.emplace_back(sc, std::move(values));
  }

  write_test_parquet(
      tmp.path / "Generator" / "GEN_BIG.parquet", stages, blocks, uid_cols);

  const ApertureDataCache cache(tmp.path);
  CHECK_FALSE(cache.empty());

  SUBCASE("scenario_uids has all 10")
  {
    const auto uids = cache.scenario_uids();
    CHECK(uids.size() == 10);
  }

  SUBCASE("spot-check first scenario")
  {
    // sc=1, s=0, b=0 → 1000
    CHECK(cache
              .lookup("Generator",
                      "GEN_BIG",
                      make_uid<Scenario>(1),
                      make_uid<Stage>(0),
                      make_uid<Block>(0))
              .value_or(0.0)
          == doctest::Approx(1000.0));
  }

  SUBCASE("spot-check last scenario last entry")
  {
    // sc=10, s=4, b=1 → 10041
    CHECK(cache
              .lookup("Generator",
                      "GEN_BIG",
                      make_uid<Scenario>(10),
                      make_uid<Stage>(4),
                      make_uid<Block>(1))
              .value_or(0.0)
          == doctest::Approx(10041.0));
  }

  SUBCASE("spot-check middle entry")
  {
    // sc=5, s=2, b=1 → 5021
    CHECK(cache
              .lookup("Generator",
                      "GEN_BIG",
                      make_uid<Scenario>(5),
                      make_uid<Stage>(2),
                      make_uid<Block>(1))
              .value_or(0.0)
          == doctest::Approx(5021.0));
  }
}

TEST_CASE("ApertureDataCache multiple elements in same class")  // NOLINT
{
  using namespace gtopt;

  const TmpDir tmp("test_aperture_cache_multi_elem");

  write_test_parquet(tmp.path / "Flow" / "RIVER_A.parquet",
                     {
                         0,
                     },
                     {
                         0,
                     },
                     {
                         {
                             1,
                             {
                                 100.0,
                             },
                         },
                     });

  write_test_parquet(tmp.path / "Flow" / "RIVER_B.parquet",
                     {
                         0,
                     },
                     {
                         0,
                     },
                     {
                         {
                             1,
                             {
                                 200.0,
                             },
                         },
                     });

  write_test_parquet(tmp.path / "Flow" / "RIVER_C.parquet",
                     {
                         0,
                     },
                     {
                         0,
                     },
                     {
                         {
                             1,
                             {
                                 300.0,
                             },
                         },
                     });

  const ApertureDataCache cache(tmp.path);
  CHECK_FALSE(cache.empty());

  CHECK(cache
            .lookup("Flow",
                    "RIVER_A",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(0),
                    make_uid<Block>(0))
            .value_or(0.0)
        == doctest::Approx(100.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER_B",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(0),
                    make_uid<Block>(0))
            .value_or(0.0)
        == doctest::Approx(200.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER_C",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(0),
                    make_uid<Block>(0))
            .value_or(0.0)
        == doctest::Approx(300.0));

  // Cross-element lookup should fail
  CHECK_FALSE(cache
                  .lookup("Flow",
                          "RIVER_A",
                          make_uid<Scenario>(1),
                          make_uid<Stage>(0),
                          make_uid<Block>(1))
                  .has_value());
}

}  // namespace

// ---------------------------------------------------------------------------
// LONG-layout aperture file (stage, block, uid, value — the shape plp2gtopt
// emits): must load via the long-direct fast path (no wide pivot) and
// resolve per-(scenario, stage, block) lookups exactly like the wide shape.
// ---------------------------------------------------------------------------
TEST_CASE("ApertureDataCache long-layout file loads via direct path")  // NOLINT
{
  using namespace gtopt;
  using aperture_test_helpers::write_test_parquet_long;

  const TmpDir tmp("test_aperture_cache_long_direct");

  // 2 stages × 2 blocks × 2 scenarios, fully dense: 8 long rows.
  write_test_parquet_long(tmp.path / "Flow" / "RIVER_L.parquet",
                          {0, 0, 1, 1, 0, 0, 1, 1},  // stage
                          {0, 1, 0, 1, 0, 1, 0, 1},  // block
                          {1, 1, 1, 1, 2, 2, 2, 2},  // scenario uid
                          {10.0, 11.0, 20.0, 21.0, 110.0, 111.0, 120.0, 121.0});

  const ApertureDataCache cache(tmp.path);
  CHECK_FALSE(cache.empty());

  CHECK(cache
            .lookup("Flow",
                    "RIVER_L",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(0),
                    make_uid<Block>(0))
            .value_or(-1.0)
        == doctest::Approx(10.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER_L",
                    make_uid<Scenario>(1),
                    make_uid<Stage>(1),
                    make_uid<Block>(1))
            .value_or(-1.0)
        == doctest::Approx(21.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER_L",
                    make_uid<Scenario>(2),
                    make_uid<Stage>(0),
                    make_uid<Block>(1))
            .value_or(-1.0)
        == doctest::Approx(111.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER_L",
                    make_uid<Scenario>(2),
                    make_uid<Stage>(1),
                    make_uid<Block>(0))
            .value_or(-1.0)
        == doctest::Approx(120.0));

  // A (scenario, stage, block) with no long row: absent, not zero.
  CHECK_FALSE(cache
                  .lookup("Flow",
                          "RIVER_L",
                          make_uid<Scenario>(3),
                          make_uid<Stage>(0),
                          make_uid<Block>(0))
                  .has_value());
}

// ---------------------------------------------------------------------------
// A SPARSE long aperture file (rows dropped for some cells) must expose
// exactly the present rows — the long-direct map is row-for-row.
// ---------------------------------------------------------------------------
TEST_CASE("ApertureDataCache long-layout sparse rows")  // NOLINT
{
  using namespace gtopt;
  using aperture_test_helpers::write_test_parquet_long;

  const TmpDir tmp("test_aperture_cache_long_sparse");

  // Scenario 7 only has stage 1 rows; scenario 8 only block 0 rows.
  write_test_parquet_long(tmp.path / "Flow" / "RIVER_S.parquet",
                          {1, 1, 0, 1},  // stage
                          {0, 1, 0, 0},  // block
                          {7, 7, 8, 8},  // scenario uid
                          {70.0, 71.0, 80.0, 81.0});

  const ApertureDataCache cache(tmp.path);
  CHECK_FALSE(cache.empty());

  CHECK(cache
            .lookup("Flow",
                    "RIVER_S",
                    make_uid<Scenario>(7),
                    make_uid<Stage>(1),
                    make_uid<Block>(1))
            .value_or(-1.0)
        == doctest::Approx(71.0));
  CHECK(cache
            .lookup("Flow",
                    "RIVER_S",
                    make_uid<Scenario>(8),
                    make_uid<Stage>(0),
                    make_uid<Block>(0))
            .value_or(-1.0)
        == doctest::Approx(80.0));
  // Dropped cell: absent.
  CHECK_FALSE(cache
                  .lookup("Flow",
                          "RIVER_S",
                          make_uid<Scenario>(7),
                          make_uid<Stage>(0),
                          make_uid<Block>(0))
                  .has_value());
}
