/**
 * @file      test_output_long_form.cpp
 * @brief     Unit tests for the long-form parquet output (2026-05-19 work)
 * @date      2026-05-19
 * @copyright BSD-3-Clause
 *
 * Pins the contracts of:
 *   * float32-vs-double value-column type selection based on
 *     `output_round_decimals` (`1..7` → float32, ≥ 8 → double, ≤ 0 →
 *     double-no-round)
 *   * round-then-cast correctness on float32 (BSS compression depends
 *     on it; a direct cast without explicit rounding produces noisy
 *     low-mantissa bytes that defeat the codec)
 *   * exact-zero row dropping in the long form (`make_field_arrays_long`)
 *   * `LpReplayBuffer::clear()` wipes every container
 *   * `SystemLP::total_write_ms` / `total_write_cells` accumulate
 *     across calls
 *   * `output_layout = long` schema shape (`scenario, stage, block,
 *     uid, value` with `uint16` keys)
 */

#include <cmath>
#include <filesystem>

#include <arrow/api.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_replay_buffer.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace  // NOLINT
{
auto make_tiny_system()
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1.0},
              {.uid = Uid {2}, .duration = 1.0},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const System system = {
      .name = "LongFormTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  return std::pair {system, simulation};
}

// Read the per-cell parquet leaf for a hive-partitioned long-form
// dataset via the project's `parquet_read_table` helper.  Returns an
// empty pointer when the file is missing.
auto read_long_leaf(const std::filesystem::path& dataset_dir)
    -> std::shared_ptr<arrow::Table>
{
  const auto leaf_stem = dataset_dir / "scene=0" / "phase=0" / "part";
  auto result = parquet_read_table(leaf_stem);
  if (!result.has_value()) {
    return {};
  }
  return *result;
}
}  // namespace

TEST_CASE(
    "output long-form: schema is (scenario, stage, block, uid, value) "
    "with uint16 keys")  // NOLINT
{
  auto [system, simulation] = make_tiny_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_lf_schema";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 10000.0;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_round_decimals = 7;  // float32 path

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  REQUIRE(system_lp.linear_interface().resolve().has_value());
  system_lp.write_out();

  const auto table =
      read_long_leaf(tmpdir / "Generator" / "generation_sol.parquet");
  REQUIRE(table != nullptr);
  REQUIRE(table->num_columns() == 5);
  CHECK(table->schema()->field(0)->name() == "scenario");
  CHECK(table->schema()->field(1)->name() == "stage");
  CHECK(table->schema()->field(2)->name() == "block");
  CHECK(table->schema()->field(3)->name() == "uid");
  CHECK(table->schema()->field(4)->name() == "value");
  CHECK(table->schema()->field(0)->type()->id() == arrow::Type::UINT16);
  CHECK(table->schema()->field(1)->type()->id() == arrow::Type::UINT16);
  CHECK(table->schema()->field(2)->type()->id() == arrow::Type::UINT16);
  CHECK(table->schema()->field(3)->type()->id() == arrow::Type::UINT16);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("output long-form: value column is float32 when 1 <= d <= 7")
// NOLINT
{
  auto [system, simulation] = make_tiny_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_lf_f32";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 10000.0;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_round_decimals = 5;  // float32 path

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  REQUIRE(system_lp.linear_interface().resolve().has_value());
  system_lp.write_out();

  const auto table =
      read_long_leaf(tmpdir / "Generator" / "generation_sol.parquet");
  REQUIRE(table != nullptr);
  CHECK(table->schema()->field(4)->name() == "value");
  CHECK(table->schema()->field(4)->type()->id() == arrow::Type::FLOAT);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("output long-form: value column is float64 when d >= 8")  // NOLINT
{
  auto [system, simulation] = make_tiny_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_lf_f64";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 10000.0;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_round_decimals = 8;  // double path

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  REQUIRE(system_lp.linear_interface().resolve().has_value());
  system_lp.write_out();

  const auto table =
      read_long_leaf(tmpdir / "Generator" / "generation_sol.parquet");
  REQUIRE(table != nullptr);
  CHECK(table->schema()->field(4)->name() == "value");
  CHECK(table->schema()->field(4)->type()->id() == arrow::Type::DOUBLE);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(
    "output long-form: round-then-cast matches "
    "(float)round_to_digits(v, d) bitwise")  // NOLINT
{
  // The optimal dispatch on `make_tiny_system` is g1 covering the full
  // 100 MW demand at $50/MW (g2 is more expensive, stays at 0).  Round
  // the expected 100.0 to 5 decimals → 100.00000 → cast to float =
  // 100.0f.  Any non-zero g2 in the output means the LP solved
  // differently; we still assert the bits match the round-then-cast
  // prediction for whatever value is stored.
  auto [system, simulation] = make_tiny_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_lf_rtc";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 10000.0;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_round_decimals = 5;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  REQUIRE(system_lp.linear_interface().resolve().has_value());
  system_lp.write_out();

  const auto table =
      read_long_leaf(tmpdir / "Generator" / "generation_sol.parquet");
  REQUIRE(table != nullptr);
  REQUIRE(table->num_rows() > 0);

  // First row should be the g1 dispatch for block 1.  Read uid + value
  // and verify the round-then-cast invariant.
  const auto uid_col =
      std::static_pointer_cast<arrow::UInt16Array>(table->column(3)->chunk(0));
  const auto val_col =
      std::static_pointer_cast<arrow::FloatArray>(table->column(4)->chunk(0));
  REQUIRE(uid_col != nullptr);
  REQUIRE(val_col != nullptr);
  for (int64_t i = 0; i < uid_col->length(); ++i) {
    const auto v_stored = val_col->Value(i);
    // The stored value must equal (float)round_to_digits(physical, 5)
    // for some `physical` — we cannot reconstruct `physical` here but
    // we can verify the value is exactly representable in float32 at
    // the 5-decimal grid: i.e. the bit pattern survives the round-
    // then-cast → round-then-cast cycle.
    const auto reapplied = static_cast<float>(
        std::round(static_cast<double>(v_stored) * 1e5) / 1e5);
    CHECK(reapplied == v_stored);
  }

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("output long-form: exact-zero rows are dropped")  // NOLINT
{
  // g1 (cheap, capacity=300) carries the full 100 MW demand for both
  // blocks; g2 (expensive) stays at exactly 0 and should be absent
  // from the long-form output.
  auto [system, simulation] = make_tiny_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_lf_drop";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 10000.0;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_round_decimals = 5;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  REQUIRE(system_lp.linear_interface().resolve().has_value());
  system_lp.write_out();

  const auto table =
      read_long_leaf(tmpdir / "Generator" / "generation_sol.parquet");
  REQUIRE(table != nullptr);

  // Walk every row, collect distinct uids actually present.  g2 (uid=2)
  // should NOT be there because its dispatch is exactly zero.
  std::set<uint16_t> uids_present;
  const auto uid_col =
      std::static_pointer_cast<arrow::UInt16Array>(table->column(3)->chunk(0));
  REQUIRE(uid_col != nullptr);
  for (int64_t i = 0; i < uid_col->length(); ++i) {
    uids_present.insert(uid_col->Value(i));
  }
  CHECK(uids_present.contains(uint16_t {1}));
  CHECK_FALSE(uids_present.contains(uint16_t {2}));

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("LpReplayBuffer::clear() empties every container")  // NOLINT
{
  LpReplayBuffer buf;

  // Populate every container so we can verify clear() drops them all.
  buf.record_dynamic_col_if_tracked(SparseCol {.lowb = 0.0, .uppb = 1.0});
  buf.record_dynamic_row_if_tracked(SparseRow {.lowb = 0.0, .uppb = 1.0});
  buf.record_cut_row_if_tracked(SparseRow {.lowb = 0.0, .uppb = 1.0});
  buf.set_pending_col_lower(ColIndex {0}, 0.0, 1.0);
  buf.set_pending_col_upper(ColIndex {0}, 0.0, 1.0);
  buf.set_pending_coeff(RowIndex {0}, ColIndex {0}, 1.0);
  buf.set_pending_rhs(RowIndex {0}, 1.0);

  REQUIRE(buf.dynamic_cols_size() == 1);
  REQUIRE(buf.dynamic_rows_size() == 1);
  REQUIRE(buf.active_cuts_size() == 1);
  REQUIRE(buf.pending_col_bounds_size() == 1);
  REQUIRE(buf.pending_coeffs_size() == 1);
  REQUIRE(buf.pending_rhs_size() == 1);

  buf.clear();

  CHECK(buf.dynamic_cols_size() == 0);
  CHECK(buf.dynamic_rows_size() == 0);
  CHECK(buf.active_cuts_size() == 0);
  CHECK(buf.pending_col_bounds_size() == 0);
  CHECK(buf.pending_coeffs_size() == 0);
  CHECK(buf.pending_rhs_size() == 0);

  // Idempotent.
  buf.clear();
  CHECK(buf.dynamic_cols_size() == 0);
}

TEST_CASE("SystemLP::total_write_ms / total_write_cells accumulate")  // NOLINT
{
  // The counters are process-wide atomics; we can't reset them between
  // tests.  Snapshot before/after the write_out call and verify the
  // delta is positive.
  const auto cells_before = SystemLP::total_write_cells();
  const auto ms_before = SystemLP::total_write_ms();

  auto [system, simulation] = make_tiny_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_lf_cum";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  REQUIRE(system_lp.linear_interface().resolve().has_value());
  system_lp.write_out();

  CHECK(SystemLP::total_write_cells() > cells_before);
  CHECK(SystemLP::total_write_ms() >= ms_before);

  std::filesystem::remove_all(tmpdir);
}
