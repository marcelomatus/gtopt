// SPDX-License-Identifier: BSD-3-Clause
//
// End-to-end oracle for LONG-layout input resolved through the full
// read_arrow_table -> get_arrow_index -> Schedule::at machinery.
//
// The pre-existing coverage only pins the long->wide pivot table in
// isolation (test_long_input_pivot.cpp) and drives Schedule::at exclusively
// from WIDE ("uid:N") parquet (test_input_traits.cpp).  Nothing exercised a
// LONG file (bare `uid` + `value` columns) all the way to a resolved value.
//
// This file closes that gap.  It is the equivalence oracle for the
// long-direct input refactor: it MUST pass on the current pivot-based code
// first, then continue to pass once get_arrow_index resolves long input
// without materialising a wide table.  Helpers are prefixed `lis_` so unity
// builds don't collide with other test translation units.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;

namespace
{

[[nodiscard]] auto lis_i32(const std::vector<int32_t>& v) -> ArrowArray
{
  arrow::Int32Builder b;
  REQUIRE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  REQUIRE(b.Finish(&a).ok());
  return a;
}

[[nodiscard]] auto lis_f64(const std::vector<double>& v) -> ArrowArray
{
  arrow::DoubleBuilder b;
  REQUIRE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  REQUIRE(b.Finish(&a).ok());
  return a;
}

/// Write a parquet file under <input_dir>/<class>/<field>.parquet from the
/// given fields/arrays.  Mirrors the helper used by test_input_traits.
void lis_write_parquet(const std::filesystem::path& input_dir,
                       std::string_view class_name,
                       std::string_view field_name,
                       const arrow::FieldVector& fields,
                       const arrow::ArrayVector& arrays)
{
  const auto dir = input_dir / class_name;
  std::filesystem::create_directories(dir);
  auto table = arrow::Table::Make(arrow::schema(fields), arrays);
  const auto fname = (dir / field_name).string() + ".parquet";
  auto ostream = arrow::io::FileOutputStream::Open(fname);
  REQUIRE(ostream.ok());
  REQUIRE(parquet::arrow::WriteTable(
              *table, arrow::default_memory_pool(), ostream.ValueOrDie(), 1024)
              .ok());
  REQUIRE(ostream.ValueOrDie()->Close().ok());
}

/// Write a LONG-layout double schedule.  `idx_cols` are the axis columns
/// (e.g. {"scenario", "stage", "block"}); `uids` is the bare `uid` column;
/// `values` the bare `value` column.
void lis_write_long_double(
    const std::filesystem::path& input_dir,
    std::string_view class_name,
    std::string_view field_name,
    const std::vector<std::pair<std::string, std::vector<int32_t>>>& idx_cols,
    const std::vector<int32_t>& uids,
    const std::vector<double>& values)
{
  arrow::FieldVector fields;
  arrow::ArrayVector arrays;
  for (const auto& [name, vals] : idx_cols) {
    fields.push_back(arrow::field(name, arrow::int32()));
    arrays.push_back(lis_i32(vals));
  }
  fields.push_back(arrow::field("uid", arrow::int32()));
  arrays.push_back(lis_i32(uids));
  fields.push_back(arrow::field("value", arrow::float64()));
  arrays.push_back(lis_f64(values));
  lis_write_parquet(input_dir, class_name, field_name, fields, arrays);
}

/// Write a LONG-layout int32 schedule (bare `value` column is int32).
void lis_write_long_int(
    const std::filesystem::path& input_dir,
    std::string_view class_name,
    std::string_view field_name,
    const std::vector<std::pair<std::string, std::vector<int32_t>>>& idx_cols,
    const std::vector<int32_t>& uids,
    const std::vector<int32_t>& values)
{
  arrow::FieldVector fields;
  arrow::ArrayVector arrays;
  for (const auto& [name, vals] : idx_cols) {
    fields.push_back(arrow::field(name, arrow::int32()));
    arrays.push_back(lis_i32(vals));
  }
  fields.push_back(arrow::field("uid", arrow::int32()));
  arrays.push_back(lis_i32(uids));
  fields.push_back(arrow::field("value", arrow::int32()));
  arrays.push_back(lis_i32(values));
  lis_write_parquet(input_dir, class_name, field_name, fields, arrays);
}

/// 1 scenario x 2 stages x (1 + 2) blocks.  Valid (s,t,b) cells:
/// (1,1,1), (1,2,2), (1,2,3) — identical to test_input_traits.
[[nodiscard]] auto lis_make_simulation() -> Simulation
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

/// Build the options/context AFTER the parquet exists, so the input file is
/// on disk before any Arrow read happens.
[[nodiscard]] auto lis_options(const std::filesystem::path& input_dir)
    -> PlanningOptionsLP
{
  const PlanningOptions opts {
      .input_directory = input_dir.string(),
  };
  return PlanningOptionsLP {opts};
}

}  // namespace

// ---------------------------------------------------------------------------
// STB (Scenario, Stage, Block) long input, two uids, dense.
// ---------------------------------------------------------------------------
TEST_CASE("long input STB double — two uids dense resolves per-uid")  // NOLINT
{
  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_lis_stb_dense";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // 6 long rows: uid 1 over the 3 cells, then uid 2 over the same 3 cells.
  lis_write_long_double(input_dir,
                        "TestGen",
                        "profile",
                        {{"scenario", {1, 1, 1, 1, 1, 1}},
                         {"stage", {1, 2, 2, 1, 2, 2}},
                         {"block", {1, 2, 3, 1, 2, 3}}},
                        {1, 1, 1, 2, 2, 2},
                        {100.0, 200.0, 300.0, 1.5, 2.5, 3.5});

  const auto sim = lis_make_simulation();
  const auto options = lis_options(input_dir);
  SimulationLP simulation {sim, options};
  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const STBRealFieldSched fsched {std::string("profile")};

  const STBRealSched s1 {ic, "TestGen", Id {Uid {1}, "profile"}, fsched};
  CHECK(s1.at(make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1))
        == doctest::Approx(100.0));
  CHECK(s1.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(2))
        == doctest::Approx(200.0));
  CHECK(s1.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3))
        == doctest::Approx(300.0));

  const STBRealSched s2 {ic, "TestGen", Id {Uid {2}, "profile"}, fsched};
  CHECK(s2.at(make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1))
        == doctest::Approx(1.5));
  CHECK(s2.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(2))
        == doctest::Approx(2.5));
  CHECK(s2.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3))
        == doctest::Approx(3.5));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// STB long input, sparse: a (uid, cell) absent from the long file must
// resolve to 0.0 (the same zero-fill the wide pivot produced).
// ---------------------------------------------------------------------------
TEST_CASE("long input STB double — missing cell zero-fills")  // NOLINT
{
  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_lis_stb_sparse";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // uid 1 present on all 3 cells; uid 2 present on only 2 cells (missing the
  // last block of stage 2).
  lis_write_long_double(input_dir,
                        "TestGen",
                        "profile",
                        {{"scenario", {1, 1, 1, 1, 1}},
                         {"stage", {1, 2, 2, 1, 2}},
                         {"block", {1, 2, 3, 1, 2}}},
                        {1, 1, 1, 2, 2},
                        {100.0, 200.0, 300.0, 1.5, 2.5});

  const auto sim = lis_make_simulation();
  const auto options = lis_options(input_dir);
  SimulationLP simulation {sim, options};
  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const STBRealFieldSched fsched {std::string("profile")};
  const STBRealSched s2 {ic, "TestGen", Id {Uid {2}, "profile"}, fsched};
  CHECK(s2.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(2))
        == doctest::Approx(2.5));
  // Absent in the long file → 0.0.
  CHECK(s2.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3))
        == doctest::Approx(0.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// TB (Stage, Block) long input.
// ---------------------------------------------------------------------------
TEST_CASE("long input TB double resolves via Schedule::at")  // NOLINT
{
  const auto tmp_root = std::filesystem::temp_directory_path() / "test_lis_tb";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  lis_write_long_double(input_dir,
                        "TestGen",
                        "gcost",
                        {{"stage", {1, 2, 2}}, {"block", {1, 2, 3}}},
                        {1, 1, 1},
                        {10.0, 20.0, 30.0});

  const auto sim = lis_make_simulation();
  const auto options = lis_options(input_dir);
  SimulationLP simulation {sim, options};
  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const TBRealFieldSched fsched {std::string("gcost")};
  const TBRealSched sched {ic, "TestGen", Id {Uid {1}, "gcost"}, fsched};
  CHECK(sched.at(make_uid<Stage>(1), make_uid<Block>(1))
        == doctest::Approx(10.0));
  CHECK(sched.at(make_uid<Stage>(2), make_uid<Block>(2))
        == doctest::Approx(20.0));
  CHECK(sched.at(make_uid<Stage>(2), make_uid<Block>(3))
        == doctest::Approx(30.0));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// T (Stage) long input.
// ---------------------------------------------------------------------------
TEST_CASE("long input T double resolves via Schedule::at")  // NOLINT
{
  const auto tmp_root = std::filesystem::temp_directory_path() / "test_lis_t";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  lis_write_long_double(
      input_dir, "TestCap", "cap", {{"stage", {1, 2}}}, {1, 1}, {42.5, 99.9});

  const auto sim = lis_make_simulation();
  const auto options = lis_options(input_dir);
  SimulationLP simulation {sim, options};
  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const TRealFieldSched fsched {std::string("cap")};
  const TRealSched sched {ic, "TestCap", Id {Uid {1}, "cap"}, fsched};
  CHECK(sched.at(make_uid<Stage>(1)) == doctest::Approx(42.5));
  CHECK(sched.at(make_uid<Stage>(2)) == doctest::Approx(99.9));

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// T (Stage) long input, int32 value column (ActiveSched).
// ---------------------------------------------------------------------------
TEST_CASE("long input T int32 resolves via ActiveSched")  // NOLINT
{
  const auto tmp_root = std::filesystem::temp_directory_path() / "test_lis_int";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  lis_write_long_int(
      input_dir, "TestAct", "active", {{"stage", {1, 2}}}, {1, 1}, {1, 0});

  const auto sim = lis_make_simulation();
  const auto options = lis_options(input_dir);
  SimulationLP simulation {sim, options};
  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const IntBoolFieldSched fsched {std::string("active")};
  const ActiveSched sched {ic, "TestAct", Id {Uid {1}, "active"}, fsched};
  CHECK(sched.at(make_uid<Stage>(1)) == 1);
  CHECK(sched.at(make_uid<Stage>(2)) == 0);

  std::filesystem::remove_all(tmp_root);
}

// ---------------------------------------------------------------------------
// Broadcast: a long file without a `block` column read as STB must broadcast
// the per-(scenario, stage) value across every block.
// ---------------------------------------------------------------------------
TEST_CASE("long input STB double — missing block axis broadcasts")  // NOLINT
{
  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_lis_bcast";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // No `block` column: one value per (scenario, stage).
  lis_write_long_double(input_dir,
                        "TestGen",
                        "profile",
                        {{"scenario", {1, 1}}, {"stage", {1, 2}}},
                        {1, 1},
                        {5.0, 6.0});

  const auto sim = lis_make_simulation();
  const auto options = lis_options(input_dir);
  SimulationLP simulation {sim, options};
  const System sys;
  SystemLP system {sys, simulation};
  const SystemContext sc {simulation, system};
  const InputContext ic {sc};

  const STBRealFieldSched fsched {std::string("profile")};
  const STBRealSched sched {ic, "TestGen", Id {Uid {1}, "profile"}, fsched};
  // Stage 1 broadcasts across its block; stage 2 across both its blocks.
  CHECK(sched.at(make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1))
        == doctest::Approx(5.0));
  CHECK(sched.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(2))
        == doctest::Approx(6.0));
  CHECK(sched.at(make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3))
        == doctest::Approx(6.0));

  std::filesystem::remove_all(tmp_root);
}
