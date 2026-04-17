/**
 * @file      test_ieee4b_ori_planning.cpp
 * @brief     Unit and solution-correctness tests for IEEE 4-bus original case
 * @date      2026-02-22
 * @copyright BSD-3-Clause
 *
 * Tests for the IEEE 4-bus original base case.  The system has:
 *   - 4 buses, 2 simple thermal generators (no solar profile), 2 demand buses,
 *     and 5 transmission lines.
 *   - A single 1-hour block (1 stage, 1 scenario).
 *   - Loads: 150 MW at b3, 100 MW at b4 (total 250 MW).
 *   - G1 at b1 (pmax=300, gcost=20 $/MWh), G2 at b2 (pmax=200, gcost=35 $/MWh).
 *   - No generator_profile_array (no solar/renewable profiles).
 *   - All demand is fully served (status=0, no load shedding).
 *   - Optimal dispatch: G1 produces 250 MW (cheaper), G2 is idle.
 *   - Objective value = 250 × 20 / scale_objective(1000) = 5.0.
 *   - Solution validated via e2e integration test comparing output CSVs.
 *
 * Analytical solution:
 *   - G1 produces 250 MW (cheapest, $20/MWh), G2 is idle (0 MW).
 *   - No line congestion (all lines have tmax ≥ 150 MW, total demand 250 MW).
 *   - All bus LMPs = $20/MWh (uniform, marginal gen cost).
 *   - Zero load shedding (fail = 0 for all demands).
 */

#include <filesystem>
#include <format>
#include <memory>
#include <string_view>
#include <vector>

#include <arrow/array.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace ieee4b_detail  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Extract the first value from an Arrow chunk as double (handles
/// int64/double).
auto chunk_first_value(const std::shared_ptr<arrow::Array>& chunk) -> double
{
  if (!chunk || chunk->length() == 0) {
    return 0.0;
  }
  if (chunk->type_id() == arrow::Type::DOUBLE) {
    return std::static_pointer_cast<arrow::DoubleArray>(chunk)->Value(0);
  }
  if (chunk->type_id() == arrow::Type::INT64) {
    return static_cast<double>(
        std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(0));
  }
  return 0.0;
}

/// Read single-row uid:1..uid:count values from a CSV output table.
auto read_uid_values(const std::filesystem::path& path, int count)
    -> std::vector<double>
{
  std::vector<double> out;
  auto tbl = csv_read_table(path);
  if (!tbl.has_value()) {
    return out;
  }
  for (int uid = 1; uid <= count; ++uid) {
    const auto col_name = std::format("uid:{}", uid);
    auto col = (*tbl)->GetColumnByName(col_name);
    if (!col || col->num_chunks() == 0) {
      out.push_back(0.0);
      continue;
    }
    out.push_back(chunk_first_value(col->chunk(0)));
  }
  return out;
}

}  // namespace ieee4b_detail

// clang-format off
static constexpr std::string_view ieee4b_ori_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_4b_ori",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"},
      {"uid": 3, "name": "b3"}, {"uid": 4, "name": "b4"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]]},
      {"uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150}
    ]
  }
})";
// clang-format on

TEST_CASE("IEEE 4-bus original - JSON parse and structure check")
{
  using namespace gtopt;
  auto planning = parse_planning_json(ieee4b_ori_json);

  CHECK(planning.system.name == "ieee_4b_ori");
  CHECK(planning.system.bus_array.size() == 4);
  CHECK(planning.system.generator_array.size() == 2);
  CHECK(planning.system.demand_array.size() == 2);
  CHECK(planning.system.line_array.size() == 5);
  CHECK(planning.system.generator_profile_array.empty());

  // Single block, 1 stage, 1 scenario
  CHECK(planning.simulation.block_array.size() == 1);
  CHECK(planning.simulation.stage_array.size() == 1);
  CHECK(planning.simulation.scenario_array.size() == 1);
}

TEST_CASE("IEEE 4-bus original - LP solve")
{
  // Use Planning::merge so arrays accumulate across multiple JSON files –
  // the same pattern gtopt_main uses when reading JSON files.
  Planning base;
  base.merge(parse_planning_json(ieee4b_ori_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}

TEST_CASE("IEEE 4-bus original - solution correctness")
{
  // Known analytical solution:
  //   G1 (gcost=20) is cheaper → dispatches all 250 MW of demand.
  //   G2 (gcost=35) is idle.
  //   No load shedding.
  //   Objective = 250 × 20 / scale_objective(1000) = 5.0.
  //   All bus LMPs = 20 $/MWh (no congestion, uniform marginal cost).
  const auto out_dir =
      std::filesystem::temp_directory_path() / "gtopt_ieee4b_correctness";
  std::filesystem::create_directories(out_dir);

  Planning base;
  base.merge(parse_planning_json(ieee4b_ori_json));
  base.options.output_directory = out_dir.string();

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);  // 1 scene processed

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp_interface = systems.front().front().linear_interface();

  // Write output CSVs so we can read back per-component values.
  planning_lp.write_out();

  SUBCASE("objective value")
  {
    // Objective value: 250 MW × $20/MWh ÷ 1000 (scale_objective) = 5.0
    CHECK(lp_interface.get_obj_value() == doctest::Approx(5.0).epsilon(1e-6));
  }

  SUBCASE("generator dispatch")
  {
    // G1 dispatches all 250 MW, G2 is idle.
    const auto gen = ieee4b_detail::read_uid_values(
        out_dir / "Generator" / "generation_sol", 2);
    REQUIRE(gen.size() == 2);
    CHECK(gen[0] == doctest::Approx(250.0).epsilon(1e-4));
    CHECK(gen[1] == doctest::Approx(0.0).epsilon(1e-4));
  }

  SUBCASE("demand fully served — no load shedding")
  {
    const auto load =
        ieee4b_detail::read_uid_values(out_dir / "Demand" / "load_sol", 2);
    REQUIRE(load.size() == 2);
    CHECK(load[0] == doctest::Approx(150.0).epsilon(1e-4));  // d3 at b3
    CHECK(load[1] == doctest::Approx(100.0).epsilon(1e-4));  // d4 at b4

    const auto fail =
        ieee4b_detail::read_uid_values(out_dir / "Demand" / "fail_sol", 2);
    REQUIRE(fail.size() == 2);
    CHECK(fail[0] == doctest::Approx(0.0).epsilon(1e-4));
    CHECK(fail[1] == doctest::Approx(0.0).epsilon(1e-4));
  }

  SUBCASE("bus LMPs — uniform at marginal gen cost")
  {
    // No congestion → all bus LMPs equal the marginal generator cost ($20).
    const auto lmp =
        ieee4b_detail::read_uid_values(out_dir / "Bus" / "balance_dual", 4);
    REQUIRE(lmp.size() == 4);
    for (size_t b = 0; b < 4; ++b) {
      CAPTURE(b);
      CHECK(lmp[b] == doctest::Approx(20.0).epsilon(1e-4));
    }
  }

  SUBCASE("generation equals total demand")
  {
    const auto gen = ieee4b_detail::read_uid_values(
        out_dir / "Generator" / "generation_sol", 2);
    const auto load =
        ieee4b_detail::read_uid_values(out_dir / "Demand" / "load_sol", 2);
    REQUIRE(gen.size() == 2);
    REQUIRE(load.size() == 2);
    const double total_gen = gen[0] + gen[1];
    const double total_load = load[0] + load[1];
    CHECK(total_gen == doctest::Approx(total_load).epsilon(1e-4));
    CHECK(total_gen == doctest::Approx(250.0).epsilon(1e-4));
  }

  std::filesystem::remove_all(out_dir);
}
