/**
 * @file      test_ieee9b_equilibration.hpp
 * @brief     IEEE 9-bus integration test for LP equilibration modes
 * @date      2026-04-04
 * @copyright BSD-3-Clause
 *
 * Solves the IEEE 9-bus case with each LpEquilibrationMethod (none, row_max,
 * ruiz) and verifies:
 *   (a) all modes solve successfully
 *   (b) all modes produce the same physical generation dispatch
 *   (c) all modes produce the same power flows (line flows)
 *   (d) all modes produce the same objective value
 *
 * The solver writes output to a temporary directory.  After each solve the
 * Generator/generation_sol and Line/flowp_sol CSV files are read back and
 * the physical values compared across equilibration modes.
 */

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── IEEE 9-bus base JSON (no losses, single 1-h block) ──────────────────────
// clang-format off
constexpr std::string_view ieee9b_eq_json = R"({
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
    "name": "ieee_9b_eq",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"},
      {"uid": 4, "name": "b4"}, {"uid": 5, "name": "b5"}, {"uid": 6, "name": "b6"},
      {"uid": 7, "name": "b7"}, {"uid": 8, "name": "b8"}, {"uid": 9, "name": "b9"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin":  0, "pmax": 270, "gcost": 30, "capacity": 270}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]]},
      {"uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]]},
      {"uid": 3, "name": "d3", "bus": "b9", "lmax":  [[90.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576,  "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625,  "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586,  "tmax_ab": 270, "tmax_ba": 270},
      {"uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,    "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008,  "tmax_ab": 250, "tmax_ba": 250}
    ]
  }
})";
// clang-format on

// ── Result of one solve run ──────────────────────────────────────────────────

struct Ieee9bEquilibrationResult
{
  int solve_status {-1};
  double obj_value {0.0};
  // Rows read from Generator/generation_sol.csv (per generator column values)
  std::vector<double> generation;  ///< generation per generator UID
  // Rows read from Line/flowp_sol.csv (per line column values)
  std::vector<double> flowp;  ///< flow per line UID
  // Rows read from Demand/load_sol.csv (per demand column values)
  std::vector<double> load;  ///< served load per demand UID
  // Rows read from Bus/balance_dual.csv (per bus column values)
  std::vector<double> balance_dual;  ///< LMP per bus UID
};

/// Solve IEEE 9-bus with the given equilibration method.
/// Writes output to @p out_dir and reads back output CSV files.
auto solve_ieee9b_eq(gtopt::LpEquilibrationMethod method,
                     const std::filesystem::path& out_dir)
    -> Ieee9bEquilibrationResult
{
  using namespace gtopt;

  std::filesystem::create_directories(out_dir);

  // Parse base planning and override equilibration_method + output_directory.
  Planning base;
  base.merge(parse_planning_json(ieee9b_eq_json));
  base.options.output_directory = out_dir.string();
  base.options.lp_matrix_options.equilibration_method = method;

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  if (!result.has_value()) {
    return {};
  }

  Ieee9bEquilibrationResult res;
  res.solve_status = result.value();

  // Capture objective from the LP interface before writing output.
  auto&& systems = planning_lp.systems();
  if (!systems.empty() && !systems.front().empty()) {
    const auto& li = systems.front().front().linear_interface();
    res.obj_value = li.get_obj_value();
  }

  // Write output files.
  planning_lp.write_out();

  // Helper: read all uid:N values from a CSV table into a vector.
  auto read_uid_values = [](const std::filesystem::path& path,
                            int count) -> std::vector<double>
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
      const auto arr =
          std::static_pointer_cast<arrow::DoubleArray>(col->chunk(0));
      // Single row (1 block × 1 stage × 1 scenario).
      out.push_back(arr->length() > 0 ? arr->Value(0) : 0.0);
    }
    return out;
  };

  // Read back all output CSV files.
  res.generation = read_uid_values(out_dir / "Generator" / "generation_sol", 3);
  res.flowp = read_uid_values(out_dir / "Line" / "flowp_sol", 9);
  res.load = read_uid_values(out_dir / "Demand" / "load_sol", 3);
  res.balance_dual = read_uid_values(out_dir / "Bus" / "balance_dual", 9);

  return res;
}

}  // namespace

// ── Test cases ───────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "IEEE 9-bus equilibration - all modes solve successfully")
{
  using namespace gtopt;

  const auto tmp_base =
      std::filesystem::temp_directory_path() / "gtopt_eq_test_solve";

  for (const auto* mode_name : {"none", "row_max", "ruiz"}) {
    CAPTURE(mode_name);
    const auto method = enum_from_name<LpEquilibrationMethod>(mode_name);
    REQUIRE(method.has_value());

    const auto out_dir = tmp_base / mode_name;
    const auto res =
        solve_ieee9b_eq(method.value_or(LpEquilibrationMethod::none), out_dir);

    CHECK(res.solve_status == 1);  // 1 scene successfully processed
    CHECK(res.obj_value > 0.0);

    std::filesystem::remove_all(out_dir);
  }
  std::filesystem::remove_all(tmp_base);
}

TEST_CASE(  // NOLINT
    "IEEE 9-bus equilibration - all modes produce identical physical outputs")
{
  using namespace gtopt;

  const auto tmp_base =
      std::filesystem::temp_directory_path() / "gtopt_eq_test_all";

  // Solve once per equilibration mode.
  const auto res_none =
      solve_ieee9b_eq(LpEquilibrationMethod::none, tmp_base / "none");
  const auto res_row =
      solve_ieee9b_eq(LpEquilibrationMethod::row_max, tmp_base / "row_max");
  const auto res_ruiz =
      solve_ieee9b_eq(LpEquilibrationMethod::ruiz, tmp_base / "ruiz");

  REQUIRE(res_none.solve_status == 1);
  REQUIRE(res_row.solve_status == 1);
  REQUIRE(res_ruiz.solve_status == 1);

  SUBCASE("objective value")
  {
    MESSAGE("obj none:    ", res_none.obj_value);
    MESSAGE("obj row_max: ", res_row.obj_value);
    MESSAGE("obj ruiz:    ", res_ruiz.obj_value);
    CHECK(res_row.obj_value
          == doctest::Approx(res_none.obj_value).epsilon(1e-8));
    CHECK(res_ruiz.obj_value
          == doctest::Approx(res_none.obj_value).epsilon(1e-8));
  }

  SUBCASE("generation dispatch from CSV")
  {
    REQUIRE(res_none.generation.size() == 3);
    REQUIRE(res_row.generation.size() == 3);
    REQUIRE(res_ruiz.generation.size() == 3);

    for (size_t g = 0; g < 3; ++g) {
      CAPTURE(g);
      MESSAGE("gen[", g, "] none:    ", res_none.generation[g]);
      MESSAGE("gen[", g, "] row_max: ", res_row.generation[g]);
      MESSAGE("gen[", g, "] ruiz:    ", res_ruiz.generation[g]);
      CHECK(res_row.generation[g]
            == doctest::Approx(res_none.generation[g]).epsilon(1e-8));
      CHECK(res_ruiz.generation[g]
            == doctest::Approx(res_none.generation[g]).epsilon(1e-8));
    }

    // Sanity: total generation > 0 and each generator non-negative.
    for (const double gen : res_none.generation) {
      CHECK(gen >= 0.0);
    }
  }

  SUBCASE("line flows from CSV")
  {
    REQUIRE(res_none.flowp.size() == 9);
    REQUIRE(res_row.flowp.size() == 9);
    REQUIRE(res_ruiz.flowp.size() == 9);

    for (size_t l = 0; l < 9; ++l) {
      CAPTURE(l);
      CHECK(res_row.flowp[l]
            == doctest::Approx(res_none.flowp[l]).epsilon(1e-8));
      CHECK(res_ruiz.flowp[l]
            == doctest::Approx(res_none.flowp[l]).epsilon(1e-8));
    }
  }

  SUBCASE("served demand from CSV")
  {
    REQUIRE(res_none.load.size() == 3);
    REQUIRE(res_row.load.size() == 3);
    REQUIRE(res_ruiz.load.size() == 3);

    for (size_t d = 0; d < 3; ++d) {
      CAPTURE(d);
      CHECK(res_row.load[d] == doctest::Approx(res_none.load[d]).epsilon(1e-8));
      CHECK(res_ruiz.load[d]
            == doctest::Approx(res_none.load[d]).epsilon(1e-8));
    }
  }

  SUBCASE("bus LMP (balance dual) from CSV")
  {
    REQUIRE(res_none.balance_dual.size() == 9);
    REQUIRE(res_row.balance_dual.size() == 9);
    REQUIRE(res_ruiz.balance_dual.size() == 9);

    for (size_t b = 0; b < 9; ++b) {
      CAPTURE(b);
      CHECK(res_row.balance_dual[b]
            == doctest::Approx(res_none.balance_dual[b]).epsilon(1e-8));
      CHECK(res_ruiz.balance_dual[b]
            == doctest::Approx(res_none.balance_dual[b]).epsilon(1e-8));
    }
  }

  SUBCASE("output CSV files exist")
  {
    for (const auto* mode : {"none", "row_max", "ruiz"}) {
      CAPTURE(mode);
      const auto d = tmp_base / mode;
      CHECK(std::filesystem::exists(d / "Generator" / "generation_sol.csv"));
      CHECK(std::filesystem::exists(d / "Line" / "flowp_sol.csv"));
      CHECK(std::filesystem::exists(d / "Demand" / "load_sol.csv"));
      CHECK(std::filesystem::exists(d / "solution.csv"));
    }
  }

  std::filesystem::remove_all(tmp_base);
}
