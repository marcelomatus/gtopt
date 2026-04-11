// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <filesystem>
#include <fstream>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/aperture_data_cache.hpp>
#include <parquet/arrow/writer.h>

namespace aperture_test_helpers
{

/// RAII helper: create a temporary directory tree, remove on destruction.
struct TmpDir
{
  std::filesystem::path path;

  explicit TmpDir(const std::string& name)
      : path(std::filesystem::temp_directory_path() / name)
  {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }

  ~TmpDir() { std::filesystem::remove_all(path); }

  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;
  TmpDir(TmpDir&&) = delete;
  TmpDir& operator=(TmpDir&&) = delete;
};

/// Write a test parquet file with stage, block, and uid:N columns.
inline void write_test_parquet(
    const std::filesystem::path& filepath,
    const std::vector<int32_t>& stages,
    const std::vector<int32_t>& blocks,
    const std::vector<std::pair<int, std::vector<double>>>& uid_columns)
{
  std::filesystem::create_directories(filepath.parent_path());

  // Build stage and block arrays
  arrow::Int32Builder stage_builder;
  arrow::Int32Builder block_builder;
  REQUIRE(stage_builder.AppendValues(stages).ok());
  REQUIRE(block_builder.AppendValues(blocks).ok());

  std::shared_ptr<arrow::Array> stage_array;
  std::shared_ptr<arrow::Array> block_array;
  REQUIRE(stage_builder.Finish(&stage_array).ok());
  REQUIRE(block_builder.Finish(&block_array).ok());

  // Build schema and value columns
  arrow::FieldVector fields;
  fields.push_back(arrow::field("stage", arrow::int32()));
  fields.push_back(arrow::field("block", arrow::int32()));

  arrow::ArrayVector columns;
  columns.push_back(stage_array);
  columns.push_back(block_array);

  for (const auto& [uid, values] : uid_columns) {
    const auto col_name = std::format("uid:{}", uid);
    fields.push_back(arrow::field(col_name, arrow::float64()));

    arrow::DoubleBuilder val_builder;
    REQUIRE(val_builder.AppendValues(values).ok());
    std::shared_ptr<arrow::Array> val_array;
    REQUIRE(val_builder.Finish(&val_array).ok());
    columns.push_back(val_array);
  }

  auto schema = arrow::schema(fields);
  auto table = arrow::Table::Make(schema, columns);

  auto output_result = arrow::io::FileOutputStream::Open(filepath.string());
  REQUIRE(output_result.ok());
  const auto& output = *output_result;

  REQUIRE(
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), output)
          .ok());
  REQUIRE(output->Close().ok());
}

}  // namespace aperture_test_helpers

TEST_CASE("ApertureDataCache default construction")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ApertureDataCache cache;
  CHECK(cache.empty());
  CHECK(cache.scenario_uids().empty());
}

TEST_CASE("ApertureDataCache nonexistent directory")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ApertureDataCache cache(std::filesystem::temp_directory_path()
                                / "nonexistent_aperture_cache_dir_xyz");
  CHECK(cache.empty());
  CHECK(cache.scenario_uids().empty());
}

TEST_CASE("ApertureDataCache loads parquet files")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const aperture_test_helpers::TmpDir tmp("test_aperture_cache_load");

  // Create directory structure: aperture_dir/Flow/RAPEL.parquet
  const auto flow_dir = tmp.path / "Flow";
  std::filesystem::create_directories(flow_dir);

  // Two rows: stage 0 block 0, stage 0 block 1
  // Two scenario UIDs: 10 and 20
  aperture_test_helpers::write_test_parquet(flow_dir / "RAPEL.parquet",
                                            {
                                                0,
                                                0,
                                            },
                                            {
                                                0,
                                                1,
                                            },
                                            {
                                                {
                                                    10,
                                                    {
                                                        100.0,
                                                        200.0,
                                                    },
                                                },
                                                {
                                                    20,
                                                    {
                                                        300.0,
                                                        400.0,
                                                    },
                                                },
                                            });

  const ApertureDataCache cache(tmp.path);

  SUBCASE("cache is not empty")
  {
    CHECK_FALSE(cache.empty());
  }

  SUBCASE("lookup existing entry")
  {
    const auto val = cache.lookup(
        "Flow", "RAPEL", make_uid<Scenario>(10), StageUid {0}, BlockUid {0});
    REQUIRE(val.has_value());
    CHECK(val.value_or(0.0) == doctest::Approx(100.0));
  }

  SUBCASE("lookup second scenario")
  {
    const auto val = cache.lookup(
        "Flow", "RAPEL", make_uid<Scenario>(20), StageUid {0}, BlockUid {1});
    REQUIRE(val.has_value());
    CHECK(val.value_or(0.0) == doctest::Approx(400.0));
  }

  SUBCASE("lookup nonexistent class returns nullopt")
  {
    const auto val = cache.lookup(
        "Power", "RAPEL", make_uid<Scenario>(10), StageUid {0}, BlockUid {0});
    CHECK_FALSE(val.has_value());
  }

  SUBCASE("lookup nonexistent element returns nullopt")
  {
    const auto val = cache.lookup(
        "Flow", "MISSING", make_uid<Scenario>(10), StageUid {0}, BlockUid {0});
    CHECK_FALSE(val.has_value());
  }

  SUBCASE("lookup nonexistent scenario returns nullopt")
  {
    const auto val = cache.lookup(
        "Flow", "RAPEL", make_uid<Scenario>(99), StageUid {0}, BlockUid {0});
    CHECK_FALSE(val.has_value());
  }

  SUBCASE("lookup nonexistent stage returns nullopt")
  {
    const auto val = cache.lookup(
        "Flow", "RAPEL", make_uid<Scenario>(10), StageUid {5}, BlockUid {0});
    CHECK_FALSE(val.has_value());
  }

  SUBCASE("lookup nonexistent block returns nullopt")
  {
    const auto val = cache.lookup(
        "Flow", "RAPEL", make_uid<Scenario>(10), StageUid {0}, BlockUid {99});
    CHECK_FALSE(val.has_value());
  }

  SUBCASE("scenario_uids returns all loaded UIDs")
  {
    const auto uids = cache.scenario_uids();
    REQUIRE(uids.size() == 2);
    // UIDs are sorted (via std::set)
    CHECK(uids[0] == make_uid<Scenario>(10));
    CHECK(uids[1] == make_uid<Scenario>(20));
  }
}

TEST_CASE("ApertureDataCache multiple classes and elements")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const aperture_test_helpers::TmpDir tmp("test_aperture_cache_multi");

  // Create Flow/RAPEL.parquet
  const auto flow_dir = tmp.path / "Flow";
  aperture_test_helpers::write_test_parquet(flow_dir / "RAPEL.parquet",
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
                                                        10.0,
                                                    },
                                                },
                                            });

  // Create Generator/GEN1.parquet
  const auto gen_dir = tmp.path / "Generator";
  aperture_test_helpers::write_test_parquet(gen_dir / "GEN1.parquet",
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
                                                        20.0,
                                                    },
                                                },
                                            });

  const ApertureDataCache cache(tmp.path);
  CHECK_FALSE(cache.empty());

  SUBCASE("lookup Flow/RAPEL")
  {
    const auto val = cache.lookup(
        "Flow", "RAPEL", make_uid<Scenario>(1), StageUid {0}, BlockUid {0});
    REQUIRE(val.has_value());
    CHECK(val.value_or(0.0) == doctest::Approx(10.0));
  }

  SUBCASE("lookup Generator/GEN1")
  {
    const auto val = cache.lookup(
        "Generator", "GEN1", make_uid<Scenario>(1), StageUid {0}, BlockUid {0});
    REQUIRE(val.has_value());
    CHECK(val.value_or(0.0) == doctest::Approx(20.0));
  }
}

TEST_CASE("ApertureDataCache ignores non-parquet files")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const aperture_test_helpers::TmpDir tmp("test_aperture_cache_ignore");

  const auto flow_dir = tmp.path / "Flow";
  std::filesystem::create_directories(flow_dir);

  // Create a non-parquet file
  {
    std::ofstream f(flow_dir / "readme.txt");
    f << "not a parquet file";
  }

  const ApertureDataCache cache(tmp.path);
  CHECK(cache.empty());
}

TEST_CASE("ApertureDataCache ignores files at root level")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const aperture_test_helpers::TmpDir tmp("test_aperture_cache_root_file");

  // Create a parquet file directly in the root (not in a subdirectory)
  aperture_test_helpers::write_test_parquet(tmp.path / "direct.parquet",
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
                                                        10.0,
                                                    },
                                                },
                                            });

  const ApertureDataCache cache(tmp.path);
  // Root-level files are not class directories, so nothing should be loaded
  CHECK(cache.empty());
}

TEST_CASE("ApertureDataCache handles empty directory")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const aperture_test_helpers::TmpDir tmp("test_aperture_cache_empty_dir");

  // Create a class directory with no parquet files
  std::filesystem::create_directories(tmp.path / "Flow");

  const ApertureDataCache cache(tmp.path);
  CHECK(cache.empty());
}
