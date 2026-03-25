/**
 * @file      test_cascade_options.hpp
 * @brief     Unit tests for CascadeTransition, CascadeLevelMethod,
 *            CascadeLevel, and CascadeOptions structs
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/cascade_options.hpp>

// ── CascadeTransition ───────────────────────────────────────────────────────

TEST_CASE("CascadeTransition - Default construction")
{
  const CascadeTransition tr {};

  CHECK_FALSE(tr.inherit_optimality_cuts.has_value());
  CHECK_FALSE(tr.inherit_feasibility_cuts.has_value());
  CHECK_FALSE(tr.inherit_targets.has_value());
  CHECK_FALSE(tr.target_rtol.has_value());
  CHECK_FALSE(tr.target_min_atol.has_value());
  CHECK_FALSE(tr.target_penalty.has_value());
  CHECK_FALSE(tr.optimality_dual_threshold.has_value());
}

TEST_CASE("CascadeTransition - Construction with all fields")
{
  const CascadeTransition tr {
      .inherit_optimality_cuts = -1,
      .inherit_feasibility_cuts = 5,
      .inherit_targets = 10,
      .target_rtol = 0.05,
      .target_min_atol = 1.0,
      .target_penalty = 500.0,
      .optimality_dual_threshold = 1e-6,
  };

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

TEST_CASE("CascadeTransition - Merge (overlay wins)")
{
  CascadeTransition base {
      .inherit_optimality_cuts = -1,
      .target_rtol = 0.05,
  };

  const CascadeTransition overlay {
      .inherit_optimality_cuts = 10,
      .inherit_feasibility_cuts = 5,
      .target_penalty = 1000.0,
  };

  base.merge(overlay);

  // Overlay wins: set fields overwrite base
  REQUIRE(base.inherit_optimality_cuts.has_value());
  CHECK(*base.inherit_optimality_cuts == 10);
  REQUIRE(base.target_rtol.has_value());
  CHECK(*base.target_rtol == doctest::Approx(0.05));

  // New from overlay
  REQUIRE(base.inherit_feasibility_cuts.has_value());
  CHECK(*base.inherit_feasibility_cuts == 5);
  REQUIRE(base.target_penalty.has_value());
  CHECK(*base.target_penalty == doctest::Approx(1000.0));

  // Still unset
  CHECK_FALSE(base.inherit_targets.has_value());
  CHECK_FALSE(base.target_min_atol.has_value());
  CHECK_FALSE(base.optimality_dual_threshold.has_value());
}

// ── CascadeLevelMethod ──────────────────────────────────────────────────────

TEST_CASE("CascadeLevelMethod - Default construction")
{
  const CascadeLevelMethod m {};

  CHECK_FALSE(m.max_iterations.has_value());
  CHECK_FALSE(m.min_iterations.has_value());
  CHECK_FALSE(m.apertures.has_value());
  CHECK_FALSE(m.convergence_tol.has_value());
}

TEST_CASE("CascadeLevelMethod - Construction with all fields")
{
  const CascadeLevelMethod m {
      .max_iterations = 50,
      .min_iterations = 3,
      .apertures =
          Array<Uid> {
              1,
              2,
          },
      .convergence_tol = 1e-3,
  };

  REQUIRE(m.max_iterations.has_value());
  CHECK(*m.max_iterations == 50);
  REQUIRE(m.min_iterations.has_value());
  CHECK(*m.min_iterations == 3);
  REQUIRE(m.apertures.has_value());
  CHECK(m.apertures->size() == 2);
  REQUIRE(m.convergence_tol.has_value());
  CHECK(*m.convergence_tol == doctest::Approx(1e-3));
}

TEST_CASE("CascadeLevelMethod - Merge (overlay wins)")
{
  CascadeLevelMethod base {
      .max_iterations = 50,
  };

  const CascadeLevelMethod overlay {
      .max_iterations = 100,
      .min_iterations = 2,
      .convergence_tol = 1e-5,
  };

  base.merge(overlay);

  // Overlay wins
  REQUIRE(base.max_iterations.has_value());
  CHECK(*base.max_iterations == 100);
  REQUIRE(base.min_iterations.has_value());
  CHECK(*base.min_iterations == 2);
  REQUIRE(base.convergence_tol.has_value());
  CHECK(*base.convergence_tol == doctest::Approx(1e-5));
}

TEST_CASE("CascadeLevelMethod - Merge apertures replaces when set")
{
  CascadeLevelMethod base {
      .apertures =
          Array<Uid> {
              1,
          },
  };

  const CascadeLevelMethod overlay {
      .apertures =
          Array<Uid> {
              10,
              20,
          },
  };

  base.merge(overlay);

  REQUIRE(base.apertures.has_value());
  REQUIRE(base.apertures->size() == 2);
  CHECK((*base.apertures)[0] == 10);
}

// ── CascadeLevel ────────────────────────────────────────────────────────────

TEST_CASE("CascadeLevel - Default construction")
{
  const CascadeLevel lv {};

  CHECK_FALSE(lv.uid.has_value());
  CHECK_FALSE(lv.name.has_value());
  CHECK_FALSE(lv.model_options.has_value());
  CHECK_FALSE(lv.sddp_options.has_value());
  CHECK_FALSE(lv.transition.has_value());
}

TEST_CASE("CascadeLevel - Construction with all fields")
{
  const CascadeLevel lv {
      .uid = 1,
      .name = "coarse",
      .model_options =
          ModelOptions {
              .use_single_bus = true,
              .scale_objective = 500.0,
          },
      .sddp_options =
          CascadeLevelMethod {
              .max_iterations = 30,
              .convergence_tol = 1e-3,
          },
      .transition =
          CascadeTransition {
              .inherit_optimality_cuts = -1,
              .target_rtol = 0.1,
          },
  };

  REQUIRE(lv.uid.has_value());
  CHECK(*lv.uid == 1);
  REQUIRE(lv.name.has_value());
  CHECK(*lv.name == "coarse");
  REQUIRE(lv.model_options.has_value());
  REQUIRE(lv.model_options->use_single_bus.has_value());
  CHECK(*lv.model_options->use_single_bus == true);
  REQUIRE(lv.sddp_options.has_value());
  REQUIRE(lv.sddp_options->max_iterations.has_value());
  CHECK(*lv.sddp_options->max_iterations == 30);
  REQUIRE(lv.transition.has_value());
  REQUIRE(lv.transition->inherit_optimality_cuts.has_value());
  CHECK(*lv.transition->inherit_optimality_cuts == -1);
}

// ── CascadeOptions ──────────────────────────────────────────────────────────

TEST_CASE("CascadeOptions - Default construction")
{
  const CascadeOptions opts {};

  CHECK_FALSE(opts.model_options.has_any());
  CHECK_FALSE(opts.sddp_options.max_iterations.has_value());
  CHECK(opts.level_array.empty());
}

TEST_CASE("CascadeOptions - Construction with levels")
{
  const CascadeOptions opts {
      .model_options =
          ModelOptions {
              .use_single_bus = true,
          },
      .sddp_options =
          SddpOptions {
              .max_iterations = 200,
              .convergence_tol = 1e-4,
          },
      .level_array =
          {
              CascadeLevel {
                  .uid = 1,
                  .name = "coarse",
                  .model_options =
                      ModelOptions {
                          .use_single_bus = true,
                      },
                  .sddp_options =
                      CascadeLevelMethod {
                          .max_iterations = 30,
                      },
              },
              CascadeLevel {
                  .uid = 2,
                  .name = "fine",
                  .model_options =
                      ModelOptions {
                          .use_single_bus = false,
                      },
                  .sddp_options =
                      CascadeLevelMethod {
                          .max_iterations = 100,
                      },
                  .transition =
                      CascadeTransition {
                          .inherit_optimality_cuts = -1,
                          .target_rtol = 0.05,
                      },
              },
          },
  };

  REQUIRE(opts.model_options.use_single_bus.has_value());
  CHECK(*opts.model_options.use_single_bus == true);
  REQUIRE(opts.sddp_options.max_iterations.has_value());
  CHECK(*opts.sddp_options.max_iterations == 200);
  REQUIRE(opts.level_array.size() == 2);

  CHECK(*opts.level_array[0].name == "coarse");
  CHECK(*opts.level_array[0].model_options->use_single_bus == true);

  CHECK(*opts.level_array[1].name == "fine");
  CHECK(*opts.level_array[1].model_options->use_single_bus == false);
  REQUIRE(opts.level_array[1].transition.has_value());
  CHECK(*opts.level_array[1].transition->inherit_optimality_cuts == -1);
}

TEST_CASE(
    "CascadeOptions - Merge model_options and sddp_options (overlay wins)")
{
  CascadeOptions base {
      .model_options =
          ModelOptions {
              .use_single_bus = true,
          },
      .sddp_options =
          SddpOptions {
              .max_iterations = 100,
          },
  };

  CascadeOptions overlay {
      .model_options =
          ModelOptions {
              .use_single_bus = false,
              .demand_fail_cost = 5000.0,
          },
      .sddp_options =
          SddpOptions {
              .max_iterations = 200,
              .convergence_tol = 1e-3,
          },
  };

  base.merge(std::move(overlay));

  // model_options: overlay wins for set fields
  REQUIRE(base.model_options.use_single_bus.has_value());
  CHECK(*base.model_options.use_single_bus == false);
  REQUIRE(base.model_options.demand_fail_cost.has_value());
  CHECK(*base.model_options.demand_fail_cost == doctest::Approx(5000.0));

  // sddp_options: overlay wins for set fields
  REQUIRE(base.sddp_options.max_iterations.has_value());
  CHECK(*base.sddp_options.max_iterations == 200);
  REQUIRE(base.sddp_options.convergence_tol.has_value());
  CHECK(*base.sddp_options.convergence_tol == doctest::Approx(1e-3));
}

TEST_CASE("CascadeOptions - Merge level_array replaces when non-empty")
{
  CascadeOptions base {
      .level_array =
          {
              CascadeLevel {
                  .uid = 1,
                  .name = "old",
              },
          },
  };

  CascadeOptions overlay {
      .level_array =
          {
              CascadeLevel {
                  .uid = 10,
                  .name = "new1",
              },
              CascadeLevel {
                  .uid = 20,
                  .name = "new2",
              },
          },
  };

  base.merge(std::move(overlay));

  REQUIRE(base.level_array.size() == 2);
  CHECK(*base.level_array[0].name == "new1");
  CHECK(*base.level_array[1].name == "new2");
}

TEST_CASE("CascadeOptions - Merge empty level_array keeps base")
{
  CascadeOptions base {
      .level_array =
          {
              CascadeLevel {
                  .uid = 1,
                  .name = "keep",
              },
          },
  };

  CascadeOptions overlay {};

  base.merge(std::move(overlay));

  REQUIRE(base.level_array.size() == 1);
  CHECK(*base.level_array[0].name == "keep");
}
