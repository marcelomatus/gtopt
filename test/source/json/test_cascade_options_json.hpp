/**
 * @file      test_cascade_options_json.hpp
 * @brief     JSON serialization tests for CascadeTransition,
 * CascadeLevelMethod, CascadeLevel, and CascadeOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_cascade_options.hpp>

// ── CascadeTransition JSON ──────────────────────────────────────────────────

TEST_CASE("CascadeTransition JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "inherit_optimality_cuts": -1,
    "inherit_feasibility_cuts": 5,
    "inherit_targets": 10,
    "target_rtol": 0.05,
    "target_min_atol": 1.0,
    "target_penalty": 500.0,
    "optimality_dual_threshold": 1e-6
  })";

  const auto tr = daw::json::from_json<CascadeTransition>(json_data);

  REQUIRE(tr.inherit_optimality_cuts.has_value());
  CHECK(*tr.inherit_optimality_cuts == -1);
  REQUIRE(tr.inherit_feasibility_cuts.has_value());
  CHECK(*tr.inherit_feasibility_cuts == 5);
  REQUIRE(tr.inherit_targets.has_value());
  CHECK(*tr.inherit_targets == 10);
  REQUIRE(tr.target_rtol.has_value());
  CHECK(*tr.target_rtol == doctest::Approx(0.05));
  REQUIRE(tr.target_min_atol.has_value());
  CHECK(*tr.target_min_atol == doctest::Approx(1.0));
  REQUIRE(tr.target_penalty.has_value());
  CHECK(*tr.target_penalty == doctest::Approx(500.0));
  REQUIRE(tr.optimality_dual_threshold.has_value());
  CHECK(*tr.optimality_dual_threshold == doctest::Approx(1e-6));
}

TEST_CASE("CascadeTransition JSON - Round-trip")
{
  const CascadeTransition original {
      .inherit_optimality_cuts = -1,
      .target_rtol = 0.1,
      .target_penalty = 1000.0,
  };

  const auto json = daw::json::to_json(original);
  const auto rt = daw::json::from_json<CascadeTransition>(json);

  CHECK(rt.inherit_optimality_cuts == original.inherit_optimality_cuts);
  CHECK(rt.target_rtol == original.target_rtol);
  CHECK(rt.target_penalty == original.target_penalty);
  CHECK_FALSE(rt.inherit_feasibility_cuts.has_value());
  CHECK_FALSE(rt.inherit_targets.has_value());
}

// ── CascadeLevelMethod JSON ─────────────────────────────────────────────────

TEST_CASE("CascadeLevelMethod JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "max_iterations": 50,
    "min_iterations": 3,
    "apertures": [1, 2],
    "convergence_tol": 1e-3
  })";

  const auto m = daw::json::from_json<CascadeLevelMethod>(json_data);

  REQUIRE(m.max_iterations.has_value());
  CHECK(*m.max_iterations == 50);
  REQUIRE(m.min_iterations.has_value());
  CHECK(*m.min_iterations == 3);
  REQUIRE(m.apertures.has_value());
  REQUIRE(m.apertures->size() == 2);
  CHECK((*m.apertures)[0] == 1);
  REQUIRE(m.convergence_tol.has_value());
  CHECK(*m.convergence_tol == doctest::Approx(1e-3));
}

TEST_CASE("CascadeLevelMethod JSON - Empty apertures")
{
  const std::string_view json_data = R"({
    "max_iterations": 10,
    "apertures": []
  })";

  const auto m = daw::json::from_json<CascadeLevelMethod>(json_data);

  REQUIRE(m.apertures.has_value());
  CHECK(m.apertures->empty());
}

// ── CascadeLevel JSON ───────────────────────────────────────────────────────

TEST_CASE("CascadeLevel JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "coarse",
    "model_options": {
      "use_single_bus": true,
      "scale_objective": 500.0
    },
    "sddp_options": {
      "max_iterations": 30,
      "convergence_tol": 1e-3
    },
    "transition": {
      "inherit_optimality_cuts": -1,
      "target_rtol": 0.1
    }
  })";

  const auto lv = daw::json::from_json<CascadeLevel>(json_data);

  REQUIRE(lv.uid.has_value());
  CHECK(*lv.uid == 1);
  REQUIRE(lv.name.has_value());
  CHECK(*lv.name == "coarse");
  REQUIRE(lv.model_options.has_value());
  CHECK(*lv.model_options->use_single_bus == true);
  CHECK(*lv.model_options->scale_objective == doctest::Approx(500.0));
  REQUIRE(lv.sddp_options.has_value());
  CHECK(*lv.sddp_options->max_iterations == 30);
  REQUIRE(lv.transition.has_value());
  CHECK(*lv.transition->inherit_optimality_cuts == -1);
}

TEST_CASE(
    "CascadeLevel JSON - Minimal (absent sub-objects default-constructed)")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "name": "fine"
  })";

  const auto lv = daw::json::from_json<CascadeLevel>(json_data);

  REQUIRE(lv.uid.has_value());
  CHECK(*lv.uid == 2);
  REQUIRE(lv.name.has_value());
  CHECK(*lv.name == "fine");
  // json_class_null creates a default-constructed optional when absent
  // The inner objects are present but empty (all fields nullopt)
  if (lv.model_options.has_value()) {
    CHECK_FALSE(lv.model_options->has_any());
  }
  if (lv.sddp_options.has_value()) {
    CHECK_FALSE(lv.sddp_options->max_iterations.has_value());
  }
  if (lv.transition.has_value()) {
    CHECK_FALSE(lv.transition->inherit_optimality_cuts.has_value());
  }
}

// ── CascadeOptions JSON ─────────────────────────────────────────────────────

TEST_CASE("CascadeOptions JSON - Full deserialization with levels")
{
  const std::string_view json_data = R"({
    "model_options": {
      "use_single_bus": true
    },
    "sddp_options": {
      "max_iterations": 200,
      "convergence_tol": 1e-4
    },
    "level_array": [
      {
        "uid": 1,
        "name": "coarse",
        "model_options": { "use_single_bus": true },
        "sddp_options": { "max_iterations": 30 }
      },
      {
        "uid": 2,
        "name": "fine",
        "model_options": { "use_single_bus": false },
        "sddp_options": { "max_iterations": 100 },
        "transition": {
          "inherit_optimality_cuts": -1,
          "target_rtol": 0.05
        }
      }
    ]
  })";

  const auto opts = daw::json::from_json<CascadeOptions>(json_data);

  REQUIRE(opts.model_options.use_single_bus.has_value());
  CHECK(*opts.model_options.use_single_bus == true);
  REQUIRE(opts.sddp_options.max_iterations.has_value());
  CHECK(*opts.sddp_options.max_iterations == 200);
  REQUIRE(opts.level_array.size() == 2);

  CHECK(*opts.level_array[0].name == "coarse");
  CHECK(*opts.level_array[0].model_options->use_single_bus == true);
  CHECK(*opts.level_array[0].sddp_options->max_iterations == 30);

  CHECK(*opts.level_array[1].name == "fine");
  CHECK(*opts.level_array[1].model_options->use_single_bus == false);
  REQUIRE(opts.level_array[1].transition.has_value());
  CHECK(*opts.level_array[1].transition->inherit_optimality_cuts == -1);
  CHECK(*opts.level_array[1].transition->target_rtol == doctest::Approx(0.05));
}

TEST_CASE("CascadeOptions JSON - Empty level_array")
{
  const std::string_view json_data = R"({
    "sddp_options": { "max_iterations": 100 }
  })";

  const auto opts = daw::json::from_json<CascadeOptions>(json_data);

  REQUIRE(opts.sddp_options.max_iterations.has_value());
  CHECK(*opts.sddp_options.max_iterations == 100);
  CHECK(opts.level_array.empty());
}

TEST_CASE("CascadeOptions JSON - Round-trip serialization")
{
  const CascadeOptions original {
      .model_options =
          ModelOptions {
              .use_single_bus = true,
              .demand_fail_cost = 5000.0,
          },
      .sddp_options =
          SddpOptions {
              .max_iterations = 150,
          },
      .level_array =
          {
              CascadeLevel {
                  .uid = 1,
                  .name = "level1",
              },
          },
  };

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<CascadeOptions>(json);

  CHECK(*rt.model_options.use_single_bus == true);
  CHECK(*rt.model_options.demand_fail_cost == doctest::Approx(5000.0));
  CHECK(*rt.sddp_options.max_iterations == 150);
  REQUIRE(rt.level_array.size() == 1);
  CHECK(*rt.level_array[0].name == "level1");
}
