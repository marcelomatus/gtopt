/**
 * @file      test_ieee14b_ori_planning.cpp
 * @brief     Integration test for IEEE 14-bus original base case
 * @date      2026-02-22
 * @copyright BSD-3-Clause
 *
 * Integration test for the IEEE 14-bus original base case.  The system has:
 *   - 14 buses, 5 simple thermal generators (no solar profile), 11 demand
 * buses, and 20 transmission lines.
 *   - A single 1-hour block (1 stage, 1 scenario).
 *   - Original IEEE 14-bus loads at buses 2-6, 9-14.
 *   - All generators are thermal with costs 20, 35, 30, 40, 45 $/MWh.
 *   - No generator_profile_array (no solar/renewable profiles).
 *
 * Numerical solution (DC OPF with Kirchhoff constraints):
 *   - Total demand ≈ 259.0 MW, fully served (no load shedding).
 *   - Obj (scaled) ≈ 8.334 (unscaled ≈ 8333.8 $/h).
 *   - G2 is idle (0 MW), merit-order uses g1, g3, g6, g8.
 *   - LMPs vary by bus due to Kirchhoff constraints.
 */

#include <filesystem>
#include <format>
#include <memory>
#include <numeric>
#include <string_view>
#include <vector>

#include <arrow/array.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace ieee14b_detail  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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

}  // namespace ieee14b_detail

// clang-format off
static constexpr std::string_view ieee14b_ori_json = R"({
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
    "name": "ieee_14b_ori",
    "bus_array": [
      {"uid": 1,  "name": "b1"},  {"uid": 2,  "name": "b2"},
      {"uid": 3,  "name": "b3"},  {"uid": 4,  "name": "b4"},
      {"uid": 5,  "name": "b5"},  {"uid": 6,  "name": "b6"},
      {"uid": 7,  "name": "b7"},  {"uid": 8,  "name": "b8"},
      {"uid": 9,  "name": "b9"},  {"uid": 10, "name": "b10"},
      {"uid": 11, "name": "b11"}, {"uid": 12, "name": "b12"},
      {"uid": 13, "name": "b13"}, {"uid": 14, "name": "b14"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 260, "gcost": 20, "capacity": 260},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 130, "gcost": 35, "capacity": 130},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin": 0, "pmax": 130, "gcost": 30, "capacity": 130},
      {"uid": 4, "name": "g6", "bus": "b6", "pmin": 0, "pmax": 100, "gcost": 40, "capacity": 100},
      {"uid": 5, "name": "g8", "bus": "b8", "pmin": 0, "pmax": 80,  "gcost": 45, "capacity": 80}
    ],
    "demand_array": [
      {"uid": 1,  "name": "d2",  "bus": "b2",  "lmax": [[21.7]]},
      {"uid": 2,  "name": "d3",  "bus": "b3",  "lmax": [[94.2]]},
      {"uid": 3,  "name": "d4",  "bus": "b4",  "lmax": [[47.8]]},
      {"uid": 4,  "name": "d5",  "bus": "b5",  "lmax": [[7.6]]},
      {"uid": 5,  "name": "d6",  "bus": "b6",  "lmax": [[11.2]]},
      {"uid": 6,  "name": "d9",  "bus": "b9",  "lmax": [[29.5]]},
      {"uid": 7,  "name": "d10", "bus": "b10", "lmax": [[9.0]]},
      {"uid": 8,  "name": "d11", "bus": "b11", "lmax": [[3.5]]},
      {"uid": 9,  "name": "d12", "bus": "b12", "lmax": [[6.1]]},
      {"uid": 10, "name": "d13", "bus": "b13", "lmax": [[13.5]]},
      {"uid": 11, "name": "d14", "bus": "b14", "lmax": [[14.9]]}
    ],
    "line_array": [
      {"uid": 1,  "name": "l1_2",   "bus_a": "b1",  "bus_b": "b2",  "reactance": 0.05917, "tmax_ab": 150, "tmax_ba": 150},
      {"uid": 2,  "name": "l1_5",   "bus_a": "b1",  "bus_b": "b5",  "reactance": 0.22304, "tmax_ab": 75,  "tmax_ba": 75},
      {"uid": 3,  "name": "l2_3",   "bus_a": "b2",  "bus_b": "b3",  "reactance": 0.19797, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 4,  "name": "l2_4",   "bus_a": "b2",  "bus_b": "b4",  "reactance": 0.17632, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 5,  "name": "l2_5",   "bus_a": "b2",  "bus_b": "b5",  "reactance": 0.17388, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 6,  "name": "l3_4",   "bus_a": "b3",  "bus_b": "b4",  "reactance": 0.17103, "tmax_ab": 60,  "tmax_ba": 60},
      {"uid": 7,  "name": "l4_5",   "bus_a": "b4",  "bus_b": "b5",  "reactance": 0.04211, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 8,  "name": "l4_7",   "bus_a": "b4",  "bus_b": "b7",  "reactance": 0.20912, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 9,  "name": "l4_9",   "bus_a": "b4",  "bus_b": "b9",  "reactance": 0.55618, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 10, "name": "l5_6",   "bus_a": "b5",  "bus_b": "b6",  "reactance": 0.25202, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 11, "name": "l6_11",  "bus_a": "b6",  "bus_b": "b11", "reactance": 0.19890, "tmax_ab": 35,  "tmax_ba": 35},
      {"uid": 12, "name": "l6_12",  "bus_a": "b6",  "bus_b": "b12", "reactance": 0.25581, "tmax_ab": 25,  "tmax_ba": 25},
      {"uid": 13, "name": "l6_13",  "bus_a": "b6",  "bus_b": "b13", "reactance": 0.13027, "tmax_ab": 50,  "tmax_ba": 50},
      {"uid": 14, "name": "l7_8",   "bus_a": "b7",  "bus_b": "b8",  "reactance": 0.17615, "tmax_ab": 50,  "tmax_ba": 50},
      {"uid": 15, "name": "l7_9",   "bus_a": "b7",  "bus_b": "b9",  "reactance": 0.11001, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 16, "name": "l9_10",  "bus_a": "b9",  "bus_b": "b10", "reactance": 0.08450, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 17, "name": "l9_14",  "bus_a": "b9",  "bus_b": "b14", "reactance": 0.27038, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 18, "name": "l10_11", "bus_a": "b10", "bus_b": "b11", "reactance": 0.19207, "tmax_ab": 30,  "tmax_ba": 30},
      {"uid": 19, "name": "l12_13", "bus_a": "b12", "bus_b": "b13", "reactance": 0.19988, "tmax_ab": 20,  "tmax_ba": 20},
      {"uid": 20, "name": "l13_14", "bus_a": "b13", "bus_b": "b14", "reactance": 0.34802, "tmax_ab": 30,  "tmax_ba": 30}
    ]
  }
})";
// clang-format on

TEST_CASE("IEEE 14-bus original - JSON parse and structure check")
{
  using namespace gtopt;
  auto planning = parse_planning_json(ieee14b_ori_json);

  CHECK(planning.system.name == "ieee_14b_ori");
  CHECK(planning.system.bus_array.size() == 14);
  CHECK(planning.system.generator_array.size() == 5);
  CHECK(planning.system.demand_array.size() == 11);
  CHECK(planning.system.line_array.size() == 20);
  CHECK(planning.system.generator_profile_array.empty());

  // Single block, 1 stage, 1 scenario
  CHECK(planning.simulation.block_array.size() == 1);
  CHECK(planning.simulation.stage_array.size() == 1);
  CHECK(planning.simulation.scenario_array.size() == 1);
}

TEST_CASE("IEEE 14-bus original - LP solve")
{
  Planning base;
  base.merge(parse_planning_json(ieee14b_ori_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}

TEST_CASE("IEEE 14-bus original - solution correctness")
{
  // DC OPF solution verified by running gtopt standalone binary.
  // All demand fully served, no load shedding.
  // g2 is idle (cheaper generators + network can serve all demand).
  constexpr int num_gen = 5;
  constexpr int num_dem = 11;
  constexpr int num_bus = 14;
  constexpr double total_demand = 259.0;  // sum of all lmax values

  const auto out_dir =
      std::filesystem::temp_directory_path() / "gtopt_ieee14b_correctness";
  std::filesystem::create_directories(out_dir);

  Planning base;
  base.merge(parse_planning_json(ieee14b_ori_json));
  base.options.output_directory = out_dir.string();

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp_interface = systems.front().front().linear_interface();

  planning_lp.write_out();

  SUBCASE("objective value")
  {
    // Obj (scaled) ≈ 8.334
    CHECK(lp_interface.get_obj_value() == doctest::Approx(8.334).epsilon(1e-2));
  }

  SUBCASE("no load shedding")
  {
    const auto fail = ieee14b_detail::read_uid_values(
        out_dir / "Demand" / "fail_sol", num_dem);
    REQUIRE(fail.size() == num_dem);

    for (size_t d = 0; d < num_dem; ++d) {
      CAPTURE(d);
      CHECK(fail[d] == doctest::Approx(0.0).epsilon(1e-4));
    }
  }

  SUBCASE("generation balance equals total demand")
  {
    const auto gen = ieee14b_detail::read_uid_values(
        out_dir / "Generator" / "generation_sol", num_gen);
    const auto load = ieee14b_detail::read_uid_values(
        out_dir / "Demand" / "load_sol", num_dem);
    REQUIRE(gen.size() == num_gen);
    REQUIRE(load.size() == num_dem);

    const double total_gen = std::accumulate(gen.begin(), gen.end(), 0.0);
    const double total_load = std::accumulate(load.begin(), load.end(), 0.0);

    CHECK(total_gen == doctest::Approx(total_load).epsilon(1e-4));
    CHECK(total_load == doctest::Approx(total_demand).epsilon(1e-4));
  }

  SUBCASE("generator dispatch within bounds")
  {
    const auto gen = ieee14b_detail::read_uid_values(
        out_dir / "Generator" / "generation_sol", num_gen);
    REQUIRE(gen.size() == num_gen);

    // pmin=0, pmax: g1=260, g2=130, g3=130, g6=100, g8=80
    constexpr double pmax[] = {
        260.0,
        130.0,
        130.0,
        100.0,
        80.0,
    };
    for (size_t g = 0; g < num_gen; ++g) {
      CAPTURE(g);
      CHECK(gen[g] >= -1e-4);
      CHECK(gen[g] <= pmax[g] + 1e-4);
    }

    // g2 is idle — cheaper alternatives serve all demand via network
    CHECK(gen[1] == doctest::Approx(0.0).epsilon(1e-4));
  }

  SUBCASE("bus LMPs are positive and vary by location")
  {
    const auto lmp = ieee14b_detail::read_uid_values(
        out_dir / "Bus" / "balance_dual", num_bus);
    REQUIRE(lmp.size() == num_bus);

    // b1 LMP = g1 marginal cost ($20) — cheapest generator bus
    CHECK(lmp[0] == doctest::Approx(20.0).epsilon(1e-4));

    // All LMPs must be positive
    for (size_t b = 0; b < num_bus; ++b) {
      CAPTURE(b);
      CHECK(lmp[b] > 0.0);
    }

    // LMP at g8 bus (b8, uid=8) = $45 (g8 is at capacity, marginal there)
    CHECK(lmp[7] == doctest::Approx(45.0).epsilon(1e-4));
  }

  std::filesystem::remove_all(out_dir);
}
