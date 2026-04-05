/**
 * @file      test_input_traits.hpp
 * @brief     Unit tests for InputTraits access_sched / at_sched / optval_sched
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 *
 * Exercises the three visitor branches of InputTraits::access_sched:
 *   1. Scalar (value_type) — already tested elsewhere; light check here.
 *   2. Vector (vector_type) — already tested elsewhere; light check here.
 *   3. FileSched (Arrow/Parquet) — the main target: writes a Parquet file
 *      with UID columns and exercises the full Arrow code path including
 *      type casting (int32, double).
 *
 * Also covers:
 *   - optval_sched (FileSched path)
 *   - Error paths: null array, type mismatch
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Create the directory tree and write a Parquet file with the given schema
/// and arrays.  Returns the stem path (without .parquet extension).
auto write_schedule_parquet(const std::filesystem::path& input_dir,
                            std::string_view class_name,
                            std::string_view field_name,
                            const std::shared_ptr<arrow::Schema>& schema,
                            const arrow::ArrayVector& columns)
    -> std::filesystem::path
{
  auto dir = input_dir / class_name;
  std::filesystem::create_directories(dir);

  auto table = arrow::Table::Make(schema, columns);
  const auto stem = dir / field_name;
  const auto fname = stem.string() + ".parquet";
  auto ostream = arrow::io::FileOutputStream::Open(fname);
  REQUIRE(ostream.ok());
  REQUIRE(parquet::arrow::WriteTable(
              *table, arrow::default_memory_pool(), ostream.ValueOrDie(), 1024)
              .ok());
  REQUIRE(ostream.ValueOrDie()->Close().ok());
  return stem;
}

/// Build a one-scenario, two-stage, three-block Simulation that matches
/// the Parquet tables written by the test helpers.
auto make_test_simulation() -> Simulation
{
  return Simulation {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 2,
              },
              {
                  .uid = Uid {3},
                  .duration = 3,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
              {
                  .uid = Uid {2},
                  .first_block = 1,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 1.0,
              },
          },
  };
}

/// Write a double-typed Parquet schedule indexed by (stage, block).
/// Columns: "stage" (int32), "block" (int32), <col_name> (double).
/// Rows: (1,1,v0), (2,2,v1), (2,3,v2) matching make_test_simulation().
void write_tb_double_parquet(const std::filesystem::path& input_dir,
                             std::string_view class_name,
                             std::string_view field_name,
                             std::string_view col_name,
                             double v0,
                             double v1,
                             double v2)
{
  arrow::Int32Builder stage_b;
  REQUIRE(stage_b.AppendValues({1, 2, 2}).ok());
  std::shared_ptr<arrow::Array> stages;
  REQUIRE(stage_b.Finish(&stages).ok());

  arrow::Int32Builder block_b;
  REQUIRE(block_b.AppendValues({1, 2, 3}).ok());
  std::shared_ptr<arrow::Array> blocks;
  REQUIRE(block_b.Finish(&blocks).ok());

  arrow::DoubleBuilder val_b;
  REQUIRE(val_b.AppendValues({v0, v1, v2}).ok());
  std::shared_ptr<arrow::Array> vals;
  REQUIRE(val_b.Finish(&vals).ok());

  auto schema = arrow::schema({
      arrow::field("stage", arrow::int32()),
      arrow::field("block", arrow::int32()),
      arrow::field(std::string(col_name), arrow::float64()),
  });

  write_schedule_parquet(
      input_dir, class_name, field_name, schema, {stages, blocks, vals});
}

/// Write an int32-typed Parquet schedule indexed by (stage).
/// Columns: "stage" (int32), <col_name> (int32).
void write_t_int_parquet(const std::filesystem::path& input_dir,
                         std::string_view class_name,
                         std::string_view field_name,
                         std::string_view col_name,
                         int32_t v0,
                         int32_t v1)
{
  arrow::Int32Builder stage_b;
  REQUIRE(stage_b.AppendValues({1, 2}).ok());
  std::shared_ptr<arrow::Array> stages;
  REQUIRE(stage_b.Finish(&stages).ok());

  arrow::Int32Builder val_b;
  REQUIRE(val_b.AppendValues({v0, v1}).ok());
  std::shared_ptr<arrow::Array> vals;
  REQUIRE(val_b.Finish(&vals).ok());

  auto schema = arrow::schema({
      arrow::field("stage", arrow::int32()),
      arrow::field(std::string(col_name), arrow::int32()),
  });

  write_schedule_parquet(
      input_dir, class_name, field_name, schema, {stages, vals});
}

/// Write an STB (scenario, stage, block) double Parquet schedule.
/// 1 scenario x 2 stages x (1 + 2) blocks = 3 rows.
void write_stb_double_parquet(const std::filesystem::path& input_dir,
                              std::string_view class_name,
                              std::string_view field_name,
                              std::string_view col_name,
                              double v0,
                              double v1,
                              double v2)
{
  arrow::Int32Builder scenario_b;
  REQUIRE(scenario_b.AppendValues({1, 1, 1}).ok());
  std::shared_ptr<arrow::Array> scenarios;
  REQUIRE(scenario_b.Finish(&scenarios).ok());

  arrow::Int32Builder stage_b;
  REQUIRE(stage_b.AppendValues({1, 2, 2}).ok());
  std::shared_ptr<arrow::Array> stages;
  REQUIRE(stage_b.Finish(&stages).ok());

  arrow::Int32Builder block_b;
  REQUIRE(block_b.AppendValues({1, 2, 3}).ok());
  std::shared_ptr<arrow::Array> blocks;
  REQUIRE(block_b.Finish(&blocks).ok());

  arrow::DoubleBuilder val_b;
  REQUIRE(val_b.AppendValues({v0, v1, v2}).ok());
  std::shared_ptr<arrow::Array> vals;
  REQUIRE(val_b.Finish(&vals).ok());

  auto schema = arrow::schema({
      arrow::field("scenario", arrow::int32()),
      arrow::field("stage", arrow::int32()),
      arrow::field("block", arrow::int32()),
      arrow::field(std::string(col_name), arrow::float64()),
  });

  write_schedule_parquet(input_dir,
                         class_name,
                         field_name,
                         schema,
                         {scenarios, stages, blocks, vals});
}

}  // namespace

// ---------------------------------------------------------------------------
// FileSched path: TBRealSched (Stage, Block) with double Parquet
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched double - TBRealSched via Parquet")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_tb";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // Write Parquet: stage, block, uid:1 columns
  write_tb_double_parquet(
      input_dir, "TestGen", "gcost", "uid:1", 10.0, 20.0, 30.0);

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "gcost"};
  const TBRealFieldSched fsched {std::string("gcost")};
  const TBRealSched sched {ic, "TestGen", id, fsched};

  CHECK(sched.at(StageUid {1}, BlockUid {1}) == doctest::Approx(10.0));
  CHECK(sched.at(StageUid {2}, BlockUid {2}) == doctest::Approx(20.0));
  CHECK(sched.at(StageUid {2}, BlockUid {3}) == doctest::Approx(30.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// FileSched path: STBRealSched (Scenario, Stage, Block) with double Parquet
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched double - STBRealSched via Parquet")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_stb";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  write_stb_double_parquet(
      input_dir, "TestGen", "profile", "uid:1", 100.0, 200.0, 300.0);

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "profile"};
  const STBRealFieldSched fsched {std::string("profile")};
  const STBRealSched sched {ic, "TestGen", id, fsched};

  CHECK(sched.at(ScenarioUid {1}, StageUid {1}, BlockUid {1})
        == doctest::Approx(100.0));
  CHECK(sched.at(ScenarioUid {1}, StageUid {2}, BlockUid {2})
        == doctest::Approx(200.0));
  CHECK(sched.at(ScenarioUid {1}, StageUid {2}, BlockUid {3})
        == doctest::Approx(300.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// FileSched path: TRealSched (Stage only) with double Parquet
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched double - TRealSched via Parquet")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_t";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // Write a (stage, value) table
  {
    arrow::Int32Builder stage_b;
    REQUIRE(stage_b.AppendValues({1, 2}).ok());
    std::shared_ptr<arrow::Array> stages;
    REQUIRE(stage_b.Finish(&stages).ok());

    arrow::DoubleBuilder val_b;
    REQUIRE(val_b.AppendValues({42.5, 99.9}).ok());
    std::shared_ptr<arrow::Array> vals;
    REQUIRE(val_b.Finish(&vals).ok());

    auto schema = arrow::schema({
        arrow::field("stage", arrow::int32()),
        arrow::field("uid:1", arrow::float64()),
    });

    write_schedule_parquet(input_dir, "TestCap", "cap", schema, {stages, vals});
  }

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "cap"};
  const TRealFieldSched fsched {std::string("cap")};
  const TRealSched sched {ic, "TestCap", id, fsched};

  CHECK(sched.at(StageUid {1}) == doctest::Approx(42.5));
  CHECK(sched.at(StageUid {2}) == doctest::Approx(99.9));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// FileSched path: int32 type (ActiveSched = Schedule<IntBool, StageUid>)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched int32 - ActiveSched via Parquet")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_int";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  write_t_int_parquet(input_dir, "TestAct", "active", "uid:1", 1, 0);

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "active"};
  const IntBoolFieldSched fsched {std::string("active")};
  const ActiveSched sched {ic, "TestAct", id, fsched};

  CHECK(sched.at(StageUid {1}) == 1);
  CHECK(sched.at(StageUid {2}) == 0);

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// OptSchedule with FileSched (exercises optval_sched Arrow path)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched - OptSchedule via Parquet")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_opt";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  write_tb_double_parquet(
      input_dir, "TestOpt", "limit", "uid:1", 5.0, 15.0, 25.0);

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "limit"};
  const OptTBRealFieldSched fsched_opt {
      TBRealFieldSched {std::string("limit")},
  };
  const OptTBRealSched sched {ic, "TestOpt", id, fsched_opt};

  REQUIRE(sched.has_value());
  CHECK(sched.at(StageUid {1}, BlockUid {1}).value_or(-1.0)
        == doctest::Approx(5.0));
  CHECK(sched.at(StageUid {2}, BlockUid {2}).value_or(-1.0)
        == doctest::Approx(15.0));
  CHECK(sched.at(StageUid {2}, BlockUid {3}).value_or(-1.0)
        == doctest::Approx(25.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// OptSchedule with no value (empty optional)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits OptSchedule empty returns nullopt")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const OptTBRealSched sched;

  CHECK_FALSE(sched.has_value());
  CHECK_FALSE(sched.at(StageUid {1}, BlockUid {1}).has_value());
}

// ---------------------------------------------------------------------------
// FileSched with column name lookup by element name (not uid:N pattern)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched - column found by name")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_byname";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // Write Parquet with column named "myval" instead of "uid:N"
  write_tb_double_parquet(
      input_dir, "TestName", "field1", "myval", 7.0, 8.0, 9.0);

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  // Id.second = "myval" should match the column name directly
  const Id id {Uid {99}, "myval"};
  const TBRealFieldSched fsched {std::string("field1")};
  const TBRealSched sched {ic, "TestName", id, fsched};

  CHECK(sched.at(StageUid {1}, BlockUid {1}) == doctest::Approx(7.0));
  CHECK(sched.at(StageUid {2}, BlockUid {2}) == doctest::Approx(8.0));
  CHECK(sched.at(StageUid {2}, BlockUid {3}) == doctest::Approx(9.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// Scalar and vector paths (light coverage — mainly for completeness)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits scalar path via Schedule")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TBRealSched sched {5.5};
  CHECK(sched.at(StageUid {1}, BlockUid {1}) == doctest::Approx(5.5));
  CHECK(sched.at(StageUid {2}, BlockUid {3}) == doctest::Approx(5.5));
}

TEST_CASE("InputTraits vector path via Schedule with InputContext")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto sim = make_test_simulation();
  const PlanningOptionsLP options;
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id;
  std::vector<std::vector<double>> vec = {{1.1}, {2.2, 3.3}};
  const TBRealFieldSched fsched {vec};
  const TBRealSched sched {ic, "VecClass", id, fsched};

  CHECK(sched.at(StageUid {1}, BlockUid {1}) == doctest::Approx(1.1));
  CHECK(sched.at(StageUid {2}, BlockUid {2}) == doctest::Approx(2.2));
  CHECK(sched.at(StageUid {2}, BlockUid {3}) == doctest::Approx(3.3));
}

// ---------------------------------------------------------------------------
// FileSched with float32 Parquet column (exercises widen_to_double_array)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched - float32 widened to double")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_f32";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // Write Parquet with float32 data column
  {
    arrow::Int32Builder stage_b;
    REQUIRE(stage_b.AppendValues({1, 2}).ok());
    std::shared_ptr<arrow::Array> stages;
    REQUIRE(stage_b.Finish(&stages).ok());

    arrow::FloatBuilder val_b;
    REQUIRE(val_b.AppendValues({1.5F, 2.5F}).ok());
    std::shared_ptr<arrow::Array> vals;
    REQUIRE(val_b.Finish(&vals).ok());

    auto schema = arrow::schema({
        arrow::field("stage", arrow::int32()),
        arrow::field("uid:1", arrow::float32()),
    });

    write_schedule_parquet(
        input_dir, "TestF32", "fval", schema, {stages, vals});
  }

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "fval"};
  const TRealFieldSched fsched {std::string("fval")};
  const TRealSched sched {ic, "TestF32", id, fsched};

  CHECK(sched.at(StageUid {1}) == doctest::Approx(1.5));
  CHECK(sched.at(StageUid {2}) == doctest::Approx(2.5));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// FileSched with int16 Parquet column (exercises widen_to_int32_array)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched - int16 widened to int32")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_i16";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // Write Parquet with int16 data column and int16 stage column
  {
    arrow::Int32Builder stage_b;
    REQUIRE(stage_b.AppendValues({1, 2}).ok());
    std::shared_ptr<arrow::Array> stages;
    REQUIRE(stage_b.Finish(&stages).ok());

    arrow::Int16Builder val_b;
    REQUIRE(val_b.AppendValues({int16_t {10}, int16_t {20}}).ok());
    std::shared_ptr<arrow::Array> vals;
    REQUIRE(val_b.Finish(&vals).ok());

    auto schema = arrow::schema({
        arrow::field("stage", arrow::int32()),
        arrow::field("uid:1", arrow::int16()),
    });

    write_schedule_parquet(
        input_dir, "TestI16", "ival", schema, {stages, vals});
  }

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "ival"};
  const IntBoolFieldSched fsched {std::string("ival")};
  const ActiveSched sched {ic, "TestI16", id, fsched};

  CHECK(sched.at(StageUid {1}) == 10);
  CHECK(sched.at(StageUid {2}) == 20);

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// FileSched with CSV fallback (parquet not found, falls back to CSV)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits FileSched - CSV fallback")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_input_traits_csv";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";
  const auto class_dir = input_dir / "TestCSV";
  std::filesystem::create_directories(class_dir);

  // Write a CSV file instead of Parquet
  {
    std::ofstream ofs((class_dir / "cost.csv").string());
    ofs << "stage,block,uid:1\n";
    ofs << "1,1,11.0\n";
    ofs << "2,2,22.0\n";
    ofs << "2,3,33.0\n";
  }

  const auto sim = make_test_simulation();
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  const PlanningOptionsLP options {
      opts,
  };
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "cost"};
  const TBRealFieldSched fsched {std::string("cost")};
  const TBRealSched sched {ic, "TestCSV", id, fsched};

  CHECK(sched.at(StageUid {1}, BlockUid {1}) == doctest::Approx(11.0));
  CHECK(sched.at(StageUid {2}, BlockUid {2}) == doctest::Approx(22.0));
  CHECK(sched.at(StageUid {2}, BlockUid {3}) == doctest::Approx(33.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// InputTraits — scalar schedule (constant value via OptReal)
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits scalar OptTBRealSched returns constant")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto sim = make_test_simulation();
  const PlanningOptions opts;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "scalar_cost"};
  const OptReal scalar_value {42.0};
  const OptTBRealSched sched {ic, "Scalar", id, scalar_value};

  CHECK(sched.at(StageUid {1}, BlockUid {1}).value_or(0.0)
        == doctest::Approx(42.0));
  CHECK(sched.at(StageUid {2}, BlockUid {2}).value_or(0.0)
        == doctest::Approx(42.0));
}

// ---------------------------------------------------------------------------
// InputTraits — nullopt schedule returns nullopt
// ---------------------------------------------------------------------------

TEST_CASE("InputTraits nullopt OptTBRealSched returns nullopt")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto sim = make_test_simulation();
  const PlanningOptions opts;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation {sim, options};

  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const Id id {Uid {1}, "null_sched"};
  const OptReal null_value;
  const OptTBRealSched sched {ic, "Null", id, null_value};

  CHECK_FALSE(sched.at(StageUid {1}, BlockUid {1}).has_value());
}
