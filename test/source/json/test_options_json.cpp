#include <string>

#include <doctest/doctest.h>
#include <gtopt/json/json_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("json_options - Deserialization of Options from JSON")
{
  using namespace gtopt;
  // JSON string representing Options
  const std::string json_string = R"({
    "input_directory": "input_dir",
    "input_format": "parquet",
    "demand_fail_cost": 1000.0,
    "reserve_fail_cost": 500.0,
    "use_line_losses": true,
    "use_kirchhoff": false,
    "use_single_bus": true,
    "kirchhoff_threshold": 0.01,
    "scale_objective": 100.0,
    "scale_theta": 10.0,
    "output_directory": "output_dir",
    "output_format": "csv",
    "output_compression": "gzip",
    "use_uid_fname": false,
    "annual_discount_rate": 0.05,
    "lp_matrix_options": {
      "names_level": 1
    }
  })";

  // Deserialize from JSON
  const auto options = daw::json::from_json<PlanningOptions>(json_string);

  // Check all fields are correctly deserialized
  REQUIRE(options.input_directory.has_value());
  if (options.input_directory) {
    CHECK(*options.input_directory == "input_dir");
  }

  REQUIRE(options.input_format.has_value());
  if (options.input_format) {
    CHECK(*options.input_format == DataFormat::parquet);
  }

  REQUIRE(options.demand_fail_cost.has_value());
  if (options.demand_fail_cost) {
    CHECK(*options.demand_fail_cost == doctest::Approx(1000.0));
  }

  REQUIRE(options.reserve_fail_cost.has_value());
  if (options.reserve_fail_cost) {
    CHECK(options.reserve_fail_cost.value() == doctest::Approx(500.0));
  }

  REQUIRE(options.use_line_losses.has_value());
  if (options.use_line_losses) {
    CHECK(*options.use_line_losses == true);
  }

  REQUIRE(options.use_kirchhoff.has_value());
  if (options.use_kirchhoff) {
    CHECK(*options.use_kirchhoff == false);
  }

  REQUIRE(options.use_single_bus.has_value());
  if (options.use_single_bus) {
    CHECK(*options.use_single_bus == true);
  }

  REQUIRE(options.kirchhoff_threshold.has_value());
  if (options.kirchhoff_threshold) {
    CHECK(*options.kirchhoff_threshold == doctest::Approx(0.01));
  }

  REQUIRE(options.scale_objective.has_value());
  if (options.scale_objective) {
    CHECK(*options.scale_objective == doctest::Approx(100.0));
  }

  REQUIRE(options.scale_theta.has_value());
  if (options.scale_theta) {
    CHECK(*options.scale_theta == doctest::Approx(10.0));
  }

  REQUIRE(options.output_directory.has_value());
  if (options.output_directory) {
    CHECK(*options.output_directory == "output_dir");
  }

  REQUIRE(options.output_format.has_value());
  if (options.output_format) {
    CHECK(*options.output_format == DataFormat::csv);
  }

  REQUIRE(options.output_compression.has_value());
  if (options.output_compression) {
    CHECK(*options.output_compression == CompressionCodec::gzip);
  }

  REQUIRE(options.lp_matrix_options.names_level.has_value());
  if (options.lp_matrix_options.names_level) {
    CHECK(*options.lp_matrix_options.names_level == LpNamesLevel::only_cols);
  }

  REQUIRE(options.use_uid_fname.has_value());
  if (options.use_uid_fname) {
    CHECK(*options.use_uid_fname == false);
  }

  REQUIRE(options.annual_discount_rate.has_value());
  if (options.annual_discount_rate) {
    CHECK(*options.annual_discount_rate == doctest::Approx(0.05));
  }
}

TEST_CASE(
    "json_options - Deserialization with missing fields (should use nulls)")
{
  using namespace gtopt;

  // JSON string with only some fields
  const std::string json_string = R"({
    "input_directory": "input_dir",
    "use_kirchhoff": true,
    "output_directory": "output_dir"
  })";

  // Deserialize from JSON
  const auto options = daw::json::from_json<PlanningOptions>(json_string);

  // Check populated fields
  REQUIRE(options.input_directory.has_value());
  if (options.input_directory) {
    CHECK(*options.input_directory == "input_dir");
  }

  REQUIRE(options.use_kirchhoff.has_value());
  if (options.use_kirchhoff) {
    CHECK(*options.use_kirchhoff == true);
  }

  REQUIRE(options.output_directory.has_value());
  if (options.output_directory) {
    CHECK(*options.output_directory == "output_dir");
  }

  // Check unpopulated fields
  CHECK_FALSE(options.input_format.has_value());
  CHECK_FALSE(options.demand_fail_cost.has_value());
  CHECK_FALSE(options.reserve_fail_cost.has_value());
  CHECK_FALSE(options.use_line_losses.has_value());
  CHECK_FALSE(options.use_single_bus.has_value());
  CHECK_FALSE(options.kirchhoff_threshold.has_value());
  CHECK_FALSE(options.scale_objective.has_value());
  CHECK_FALSE(options.scale_theta.has_value());
  CHECK_FALSE(options.output_format.has_value());
  CHECK_FALSE(options.output_compression.has_value());
  CHECK_FALSE(options.lp_matrix_options.names_level.has_value());
  CHECK_FALSE(options.use_uid_fname.has_value());
  CHECK_FALSE(options.annual_discount_rate.has_value());
}

TEST_CASE("json_options - Round-trip serialization and deserialization")
{
  using namespace gtopt;

  // Create original Options
  PlanningOptions original {
      .input_directory = "input_dir",
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = true,
      .scale_objective = 100.0,
      .output_directory = "output_dir",
      .lp_matrix_options {
          .names_level = LpNamesLevel::minimal,
      },
  };

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Deserialize back to Options
  const auto deserialized = daw::json::from_json<PlanningOptions>(json_data);

  // Check all fields match
  CHECK(deserialized.input_directory == original.input_directory);
  CHECK(deserialized.demand_fail_cost == original.demand_fail_cost);
  CHECK(deserialized.use_kirchhoff == original.use_kirchhoff);
  CHECK(deserialized.scale_objective == original.scale_objective);
  CHECK(deserialized.output_directory == original.output_directory);
  CHECK(deserialized.lp_matrix_options.names_level
        == original.lp_matrix_options.names_level);

  // Check that unpopulated fields remain empty
  CHECK_FALSE(deserialized.input_format.has_value());
  CHECK_FALSE(deserialized.reserve_fail_cost.has_value());
  CHECK_FALSE(deserialized.use_line_losses.has_value());
  CHECK_FALSE(deserialized.use_single_bus.has_value());
  CHECK_FALSE(deserialized.kirchhoff_threshold.has_value());
  CHECK_FALSE(deserialized.scale_theta.has_value());
  CHECK_FALSE(deserialized.output_format.has_value());
  CHECK_FALSE(deserialized.output_compression.has_value());
  CHECK_FALSE(deserialized.use_uid_fname.has_value());
  CHECK_FALSE(deserialized.annual_discount_rate.has_value());
}

TEST_CASE("json_options - Solver options fields JSON round-trip")  // NOLINT
{
  using namespace gtopt;
  // Verify that solver_options.algorithm, .threads, and .presolve are
  // serialized and deserialized correctly via the nested sub-object.
  const PlanningOptions original {
      .solver_options =
          SolverOptions {
              .algorithm = LPAlgo::dual,
              .threads = 4,
              .presolve = false,
          },
  };

  const auto json_data = daw::json::to_json(original);
  const auto deserialized = daw::json::from_json<PlanningOptions>(json_data);

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
      "algorithm": 1,
      "threads": 2,
      "presolve": true,
      "log_level": 0,
      "reuse_basis": false
    }
  })";

  const auto options = daw::json::from_json<PlanningOptions>(json_string);

  CHECK(options.solver_options.algorithm == LPAlgo::primal);
  CHECK(options.solver_options.threads == 2);
  CHECK(options.solver_options.presolve == true);

  // Other fields should be null
  CHECK_FALSE(options.use_single_bus.has_value());
  CHECK_FALSE(options.demand_fail_cost.has_value());
}
