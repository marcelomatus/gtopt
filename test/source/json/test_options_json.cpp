// SPDX-License-Identifier: BSD-3-Clause
//
// JSON deserialization tests for `PlanningOptions`.
//
// Post-§11 (2026-05-17): the 11 legacy top-level ModelOptions-mirror
// keys (`demand_fail_cost`, `reserve_fail_cost`, `hydro_fail_cost`,
// `hydro_use_value`, `use_line_losses`, `loss_segments`,
// `use_kirchhoff`, `use_single_bus`, `kirchhoff_threshold`,
// `scale_objective`, `scale_theta`) live ONLY inside the nested
// `options.model_options.*` sub-object.  The legacy flat form is
// rejected by StrictParsePolicy.

#include <string>

#include <doctest/doctest.h>
#include <gtopt/json/json_options.hpp>
#include <gtopt/json/json_parse_policy.hpp>

using namespace gtopt;

TEST_CASE("json_options - Deserialization of Options from JSON")
{
  using namespace gtopt;
  const std::string json_string = R"({
    "input_directory": "input_dir",
    "input_format": "parquet",
    "model_options": {
      "demand_fail_cost": 1000.0,
      "reserve_shortage_cost": 500.0,
      "use_line_losses": true,
      "use_kirchhoff": false,
      "use_single_bus": true,
      "kirchhoff_threshold": 0.01,
      "scale_objective": 100.0,
      "scale_theta": 10.0
    },
    "output_directory": "output_dir",
    "output_format": "csv",
    "output_compression": "gzip",
    "use_uid_fname": false,
    "annual_discount_rate": 0.05
  })";

  const auto options =
      daw::json::from_json<PlanningOptions>(json_string, StrictParsePolicy);

  REQUIRE(options.input_directory.has_value());
  CHECK(*options.input_directory == "input_dir");
  REQUIRE(options.input_format.has_value());
  CHECK(*options.input_format == DataFormat::parquet);

  REQUIRE(options.model_options.demand_fail_cost.has_value());
  CHECK(*options.model_options.demand_fail_cost == doctest::Approx(1000.0));
  REQUIRE(options.model_options.reserve_shortage_cost.has_value());
  CHECK(options.model_options.reserve_shortage_cost.value()
        == doctest::Approx(500.0));
  REQUIRE(options.model_options.use_line_losses.has_value());
  CHECK(*options.model_options.use_line_losses == true);
  REQUIRE(options.model_options.use_kirchhoff.has_value());
  CHECK(*options.model_options.use_kirchhoff == false);
  REQUIRE(options.model_options.use_single_bus.has_value());
  CHECK(*options.model_options.use_single_bus == true);
  REQUIRE(options.model_options.kirchhoff_threshold.has_value());
  CHECK(*options.model_options.kirchhoff_threshold == doctest::Approx(0.01));
  REQUIRE(options.model_options.scale_objective.has_value());
  CHECK(*options.model_options.scale_objective == doctest::Approx(100.0));
  REQUIRE(options.model_options.scale_theta.has_value());
  CHECK(*options.model_options.scale_theta == doctest::Approx(10.0));

  REQUIRE(options.output_directory.has_value());
  CHECK(*options.output_directory == "output_dir");
  REQUIRE(options.output_format.has_value());
  CHECK(*options.output_format == DataFormat::csv);
  REQUIRE(options.output_compression.has_value());
  CHECK(*options.output_compression == CompressionCodec::gzip);
  REQUIRE(options.use_uid_fname.has_value());
  CHECK(*options.use_uid_fname == false);
  REQUIRE(options.annual_discount_rate.has_value());
  CHECK(*options.annual_discount_rate == doctest::Approx(0.05));
}

TEST_CASE(
    "json_options - Deserialization with missing fields (should use nulls)")
{
  using namespace gtopt;

  const std::string json_string = R"({
    "input_directory": "input_dir",
    "model_options": {
      "use_kirchhoff": true
    },
    "output_directory": "output_dir"
  })";

  const auto options =
      daw::json::from_json<PlanningOptions>(json_string, StrictParsePolicy);

  REQUIRE(options.input_directory.has_value());
  CHECK(*options.input_directory == "input_dir");
  REQUIRE(options.model_options.use_kirchhoff.has_value());
  CHECK(*options.model_options.use_kirchhoff == true);
  REQUIRE(options.output_directory.has_value());
  CHECK(*options.output_directory == "output_dir");

  CHECK_FALSE(options.input_format.has_value());
  CHECK_FALSE(options.model_options.demand_fail_cost.has_value());
  CHECK_FALSE(options.model_options.reserve_shortage_cost.has_value());
  CHECK_FALSE(options.model_options.use_line_losses.has_value());
  CHECK_FALSE(options.model_options.use_single_bus.has_value());
  CHECK_FALSE(options.model_options.kirchhoff_threshold.has_value());
  CHECK_FALSE(options.model_options.scale_objective.has_value());
  CHECK_FALSE(options.model_options.scale_theta.has_value());
  CHECK_FALSE(options.output_format.has_value());
  CHECK_FALSE(options.output_compression.has_value());
  CHECK_FALSE(options.use_uid_fname.has_value());
  CHECK_FALSE(options.annual_discount_rate.has_value());
}

TEST_CASE("json_options - Round-trip serialization and deserialization")
{
  using namespace gtopt;

  PlanningOptions original {
      .input_directory = "input_dir",
      .output_directory = "output_dir",
      .model_options =
          {
              .use_kirchhoff = true,
              .scale_objective = 100.0,
              .demand_fail_cost = 1000.0,
          },
      .lp_matrix_options {},
  };

  const auto json_data = daw::json::to_json(original);
  const auto deserialized =
      daw::json::from_json<PlanningOptions>(json_data, StrictParsePolicy);

  CHECK(deserialized.input_directory == original.input_directory);
  CHECK(deserialized.output_directory == original.output_directory);
  CHECK(deserialized.model_options.demand_fail_cost
        == original.model_options.demand_fail_cost);
  CHECK(deserialized.model_options.use_kirchhoff
        == original.model_options.use_kirchhoff);
  CHECK(deserialized.model_options.scale_objective
        == original.model_options.scale_objective);

  CHECK_FALSE(deserialized.input_format.has_value());
  CHECK_FALSE(deserialized.model_options.reserve_shortage_cost.has_value());
  CHECK_FALSE(deserialized.model_options.use_line_losses.has_value());
  CHECK_FALSE(deserialized.model_options.use_single_bus.has_value());
  CHECK_FALSE(deserialized.model_options.kirchhoff_threshold.has_value());
  CHECK_FALSE(deserialized.model_options.scale_theta.has_value());
  CHECK_FALSE(deserialized.output_format.has_value());
  CHECK_FALSE(deserialized.output_compression.has_value());
  CHECK_FALSE(deserialized.use_uid_fname.has_value());
  CHECK_FALSE(deserialized.annual_discount_rate.has_value());
}

TEST_CASE("json_options - Solver options fields JSON round-trip")  // NOLINT
{
  using namespace gtopt;
  const PlanningOptions original {
      .solver_options =
          SolverOptions {
              .algorithm = LPAlgo::dual,
              .threads = 4,
              .presolve = false,
          },
  };

  const auto json_data = daw::json::to_json(original);
  const auto deserialized =
      daw::json::from_json<PlanningOptions>(json_data, StrictParsePolicy);

  CHECK(deserialized.solver_options.algorithm == LPAlgo::dual);
  CHECK(deserialized.solver_options.threads == 4);
  CHECK(deserialized.solver_options.presolve == false);
}

TEST_CASE(
    "json_options - solver_options deserialization from JSON "
    "string")  // NOLINT
{
  using namespace gtopt;
  const std::string json_string = R"({
    "solver_options": {
      "algorithm": "primal",
      "threads": 2,
      "presolve": true,
      "log_level": 0
    }
  })";

  const auto options =
      daw::json::from_json<PlanningOptions>(json_string, StrictParsePolicy);

  CHECK(options.solver_options.algorithm == LPAlgo::primal);
  CHECK(options.solver_options.threads == 2);
  CHECK(options.solver_options.presolve == true);

  CHECK_FALSE(options.model_options.use_single_bus.has_value());
  CHECK_FALSE(options.model_options.demand_fail_cost.has_value());
}
