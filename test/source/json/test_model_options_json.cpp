/**
 * @file      test_model_options_json.hpp
 * @brief     JSON serialization tests for ModelOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_model_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ModelOptions JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "use_single_bus": true,
    "use_kirchhoff": false,
    "use_line_losses": true,
    "kirchhoff_threshold": 110.0,
    "loss_segments": 3,
    "scale_objective": 500.0,
    "scale_theta": 2000.0,
    "demand_fail_cost": 5000.0,
    "reserve_fail_cost": 3000.0
  })";

  const auto opts = daw::json::from_json<ModelOptions>(json_data);

  REQUIRE(opts.use_single_bus.has_value());
  CHECK(*opts.use_single_bus == true);
  REQUIRE(opts.use_kirchhoff.has_value());
  CHECK(*opts.use_kirchhoff == false);
  REQUIRE(opts.use_line_losses.has_value());
  CHECK(*opts.use_line_losses == true);
  REQUIRE(opts.kirchhoff_threshold.has_value());
  CHECK(*opts.kirchhoff_threshold == doctest::Approx(110.0));
  REQUIRE(opts.loss_segments.has_value());
  CHECK(*opts.loss_segments == 3);
  REQUIRE(opts.scale_objective.has_value());
  CHECK(*opts.scale_objective == doctest::Approx(500.0));
  REQUIRE(opts.scale_theta.has_value());
  CHECK(*opts.scale_theta == doctest::Approx(2000.0));
  REQUIRE(opts.demand_fail_cost.has_value());
  CHECK(*opts.demand_fail_cost == doctest::Approx(5000.0));
  REQUIRE(opts.reserve_fail_cost.has_value());
  CHECK(*opts.reserve_fail_cost == doctest::Approx(3000.0));
}

TEST_CASE("ModelOptions JSON - Missing fields stay nullopt")
{
  const std::string_view json_data = R"({
    "use_kirchhoff": true,
    "scale_objective": 1000.0
  })";

  const auto opts = daw::json::from_json<ModelOptions>(json_data);

  REQUIRE(opts.use_kirchhoff.has_value());
  CHECK(*opts.use_kirchhoff == true);
  REQUIRE(opts.scale_objective.has_value());
  CHECK(*opts.scale_objective == doctest::Approx(1000.0));

  CHECK_FALSE(opts.use_single_bus.has_value());
  CHECK_FALSE(opts.use_line_losses.has_value());
  CHECK_FALSE(opts.kirchhoff_threshold.has_value());
  CHECK_FALSE(opts.loss_segments.has_value());
  CHECK_FALSE(opts.scale_theta.has_value());
  CHECK_FALSE(opts.demand_fail_cost.has_value());
  CHECK_FALSE(opts.reserve_fail_cost.has_value());
}

TEST_CASE("ModelOptions JSON - Round-trip serialization")
{
  const ModelOptions original {
      .use_single_bus = false,
      .use_kirchhoff = true,
      .kirchhoff_threshold = 66.0,
      .scale_objective = 2000.0,
      .demand_fail_cost = 1500.0,
  };

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto roundtrip = daw::json::from_json<ModelOptions>(json);

  CHECK(roundtrip.use_single_bus == original.use_single_bus);
  CHECK(roundtrip.use_kirchhoff == original.use_kirchhoff);
  CHECK(roundtrip.kirchhoff_threshold == original.kirchhoff_threshold);
  CHECK(roundtrip.scale_objective == original.scale_objective);
  CHECK(roundtrip.demand_fail_cost == original.demand_fail_cost);
  CHECK_FALSE(roundtrip.use_line_losses.has_value());
  CHECK_FALSE(roundtrip.loss_segments.has_value());
}

TEST_CASE("ModelOptions JSON - Empty object")
{
  const std::string_view json_data = R"({})";
  const auto opts = daw::json::from_json<ModelOptions>(json_data);

  CHECK_FALSE(opts.use_single_bus.has_value());
  CHECK_FALSE(opts.use_kirchhoff.has_value());
  CHECK_FALSE(opts.demand_fail_cost.has_value());
  CHECK_FALSE(opts.has_any());
}
