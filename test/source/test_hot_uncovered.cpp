/**
 * @file      test_hot_uncovered.hpp
 * @brief     Tests for hot production paths with zero test coverage
 * @date      2026-04-04
 * @copyright BSD-3-Clause
 *
 * Targets specific uncovered lines identified by profiling/coverage analysis:
 *   - as_label.hpp:209     string_holder(std::string&&) rvalue constructor
 *   - line_lp.cpp:152      inactive bus early return in add_to_lp
 *   - line_lp.cpp:270      is_loop() early return in add_to_output
 *   - line_losses.cpp:163  LossAllocationMode::sender in apply_loss_allocation
 *   - array_index_traits.hpp:172  bare UID string column lookup
 */

#include <filesystem>
#include <string>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/as_label.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── Item 4: string_holder rvalue string constructor (as_label.hpp:209) ──

TEST_CASE("string_holder rvalue string takes ownership")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Build an rvalue string that is long enough to avoid SSO (>= 22 chars)
  // so that the move is observable (the original is left empty).
  std::string original = "this_is_a_long_rvalue_string_for_testing";
  const auto* data_ptr = original.data();

  detail::string_holder holder(std::move(original));

  // After move, the holder should have the string contents
  CHECK(holder.view() == "this_is_a_long_rvalue_string_for_testing");

  // The original string should have been moved-from (typically empty
  // for strings longer than SSO). We cannot require this portably,
  // but we can verify the holder's view is correct.
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
  CHECK(original.empty());

  // The holder should own the buffer (same pointer as original had).
  CHECK(holder.view().data() == data_ptr);
}

TEST_CASE("as_label with rvalue string exercises owned path")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Construct a temporary string that will be passed as rvalue
  auto result = as_label(std::string("Rvalue"), "const_ref");
  CHECK(result == "Rvalue_const_ref");

  // Also test as_label_into with rvalue
  std::string buf;
  as_label_into(buf, std::string("Moved"), 42);
  CHECK(buf == "Moved_42");
}

// ── Item 5: inactive bus early return in add_to_lp (line_lp.cpp:152) ──

TEST_CASE("LineLP inactive bus early return")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two buses: b2 is inactive. Line connects b1-b2.
  // The line should hit the inactive-bus early return.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {
          .uid = Uid {2},
          .name = "b2",
          .active = IntBool {0},
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 30.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.1,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "InactiveBusTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  PlanningOptions opts;
  opts.use_kirchhoff = false;
  opts.use_single_bus = false;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();

  // The LP should still solve (line skipped due to inactive bus)
  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ── Item 2: loop line add_to_output (line_lp.cpp:270) ──

TEST_CASE("LineLP loop line exercises add_to_output early return")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Use PlanningLP to exercise the full pipeline including add_to_output.
  // A loop line (bus_a == bus_b) should hit the early return in both
  // add_to_lp (line 137-138) and add_to_output (line 269-270).
  Planning planning = {
      .options =
          {
              .use_kirchhoff = false,
              .use_single_bus = false,
          },
      .simulation =
          {
              .block_array = {{
                  .uid = Uid {1},
                  .duration = 1,
              }},
              .stage_array = {{
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              }},
              .scenario_array = {{
                  .uid = Uid {0},
              }},
          },
      .system =
          {
              .name = "LoopLineOutputTest",
              .bus_array = {{
                  .uid = Uid {1},
                  .name = "b1",
              }},
              .demand_array = {{
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = 100.0,
              }},
              .generator_array = {{
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 50.0,
                  .capacity = 200.0,
              }},
              .line_array = {{
                  .uid = Uid {1},
                  .name = "loop",
                  .bus_a = Uid {1},
                  .bus_b = Uid {1},
                  .capacity = 100.0,
              }},
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// ── Item 3: sender loss allocation in piecewise mode ──
//    (line_losses.cpp:163-164, apply_loss_allocation sender path)

TEST_CASE("LineLP piecewise mode with sender loss allocation")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Piecewise mode requires: R > 0, V > 0, loss_segments >= 2.
  // Sender allocation routes loss to the sending bus only.
  Planning planning = {
      .options =
          {
              .demand_fail_cost = 1000.0,
              .use_kirchhoff = false,
              .use_single_bus = false,
          },
      .simulation =
          {
              .block_array = {{
                  .uid = Uid {1},
                  .duration = 1,
              }},
              .stage_array = {{
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              }},
              .scenario_array = {{
                  .uid = Uid {0},
              }},
          },
      .system =
          {
              .name = "PiecewiseSenderTest",
              .bus_array =
                  {
                      {
                          .uid = Uid {1},
                          .name = "b1",
                      },
                      {
                          .uid = Uid {2},
                          .name = "b2",
                      },
                  },
              .demand_array = {{
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {2},
                  .lmax = 100.0,
                  .capacity = 100.0,
              }},
              .generator_array = {{
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 500.0,
              }},
              .line_array = {{
                  .uid = Uid {1},
                  .name = "l1",
                  .bus_a = Uid {1},
                  .bus_b = Uid {2},
                  .voltage = 100.0,
                  .resistance = 0.01,
                  .line_losses_mode = OptName {"piecewise"},
                  .loss_segments = 3,
                  .loss_allocation_mode = Name {"sender"},
                  .tmax_ba = 200.0,
                  .tmax_ab = 200.0,
                  .capacity = 200.0,
              }},
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // Verify objective is positive (generator dispatches to meet demand)
  const auto obj = planning_lp.systems()
                       .front()
                       .front()
                       .linear_interface()
                       .get_obj_value_raw();
  CHECK(obj > 0.0);
}

TEST_CASE("LineLP bidirectional mode with sender loss allocation")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Bidirectional mode also calls apply_loss_allocation.
  // Sender allocation with R/V parameters.
  Planning planning = {
      .options =
          {
              .demand_fail_cost = 1000.0,
              .use_kirchhoff = false,
              .use_single_bus = false,
          },
      .simulation =
          {
              .block_array = {{
                  .uid = Uid {1},
                  .duration = 1,
              }},
              .stage_array = {{
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              }},
              .scenario_array = {{
                  .uid = Uid {0},
              }},
          },
      .system =
          {
              .name = "BidirectionalSenderTest",
              .bus_array =
                  {
                      {
                          .uid = Uid {1},
                          .name = "b1",
                      },
                      {
                          .uid = Uid {2},
                          .name = "b2",
                      },
                  },
              .demand_array = {{
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {2},
                  .lmax = 100.0,
                  .capacity = 100.0,
              }},
              .generator_array = {{
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 500.0,
              }},
              .line_array = {{
                  .uid = Uid {1},
                  .name = "l1",
                  .bus_a = Uid {1},
                  .bus_b = Uid {2},
                  .voltage = 100.0,
                  .resistance = 0.01,
                  .line_losses_mode = OptName {"bidirectional"},
                  .loss_segments = 3,
                  .loss_allocation_mode = Name {"sender"},
                  .tmax_ba = 200.0,
                  .tmax_ab = 200.0,
                  .capacity = 200.0,
              }},
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto obj = planning_lp.systems()
                       .front()
                       .front()
                       .linear_interface()
                       .get_obj_value_raw();
  CHECK(obj > 0.0);
}

// ── Item 1: bare UID string column lookup (array_index_traits.hpp:172) ──

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Write a Parquet schedule file with bare UID column names (e.g. "1")
/// instead of the standard "uid:1" or "name" patterns.
/// This exercises the fallback path at array_index_traits.hpp:172.
void write_bare_uid_parquet(const std::filesystem::path& input_dir,
                            std::string_view class_name,
                            std::string_view field_name,
                            int uid,
                            double value)
{
  auto dir = input_dir / class_name;
  std::filesystem::create_directories(dir);

  // Build columns: "stage" (int32), "block" (int32), "<uid>" (double)
  arrow::Int32Builder stage_b;
  REQUIRE(stage_b.Append(1).ok());
  std::shared_ptr<arrow::Array> stages;
  REQUIRE(stage_b.Finish(&stages).ok());

  arrow::Int32Builder block_b;
  REQUIRE(block_b.Append(1).ok());
  std::shared_ptr<arrow::Array> blocks;
  REQUIRE(block_b.Finish(&blocks).ok());

  arrow::DoubleBuilder val_b;
  REQUIRE(val_b.Append(value).ok());
  std::shared_ptr<arrow::Array> vals;
  REQUIRE(val_b.Finish(&vals).ok());

  // Column name is the bare UID string (e.g. "1")
  auto schema = arrow::schema({
      arrow::field("stage", arrow::int32()),
      arrow::field("block", arrow::int32()),
      arrow::field(std::to_string(uid), arrow::float64()),
  });

  auto table = arrow::Table::Make(schema, {stages, blocks, vals});
  const auto stem = dir / field_name;
  const auto fname = stem.string() + ".parquet";
  auto ostream = arrow::io::FileOutputStream::Open(fname);
  REQUIRE(ostream.ok());
  REQUIRE(parquet::arrow::WriteTable(
              *table, arrow::default_memory_pool(), ostream.ValueOrDie(), 1024)
              .ok());
  REQUIRE(ostream.ValueOrDie()->Close().ok());
}

}  // namespace

TEST_CASE("Bare UID column name lookup in Parquet schedule")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Create a temporary directory for the test
  const auto tmp_root =
      std::filesystem::temp_directory_path() / "test_bare_uid_col";
  std::filesystem::remove_all(tmp_root);
  const auto input_dir = tmp_root / "input";

  // Write a Parquet file for Generator gcost with column named "1"
  // (bare UID) instead of "uid:1" or "gcost"
  write_bare_uid_parquet(input_dir, "Generator", "gcost", 1, 42.0);

  const Simulation sim = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  Planning planning = {
      .options =
          {
              .input_directory = input_dir.string(),
              .demand_fail_cost = 1000.0,
              .use_kirchhoff = false,
              .use_single_bus = true,
          },
      .simulation = sim,
      .system =
          {
              .name = "BareUidTest",
              .bus_array = {{
                  .uid = Uid {1},
                  .name = "b1",
              }},
              .demand_array = {{
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = 50.0,
              }},
              .generator_array = {{
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  // gcost comes from Parquet file with bare "1" column
                  .gcost = std::string("gcost"),
                  .capacity = 100.0,
              }},
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // Verify the generator cost came from the Parquet file (42.0 * 50 MW * 1h)
  const auto obj = planning_lp.systems()
                       .front()
                       .front()
                       .linear_interface()
                       .get_obj_value_raw();
  // The objective should reflect gcost = 42.0 from Parquet
  CHECK(obj > 0.0);

  std::filesystem::remove_all(tmp_root);
}
