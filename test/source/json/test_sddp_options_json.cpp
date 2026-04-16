/**
 * @file      test_sddp_options_json.hpp
 * @brief     JSON serialization tests for SddpOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_sddp_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SddpOptions JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "cut_sharing_mode": "expected",
    "cut_directory": "my_cuts",
    "api_enabled": true,
    "update_lp_skip": 2,
    "max_iterations": 200,
    "min_iterations": 5,
    "convergence_tol": 1e-3,
    "elastic_penalty": 1e5,
    "alpha_min": 0.0,
    "alpha_max": 1e10,
    "cut_recovery_mode": "append",
    "recovery_mode": "cuts",
    "save_per_iteration": false,
    "cuts_input_file": "init.csv",
    "sentinel_file": "stop.flag",
    "elastic_mode": "multi_cut",
    "multi_cut_threshold": 20,
    "apertures": [1, 2, 3],
    "aperture_directory": "ap_data",
    "aperture_timeout": 30.0,
    "save_aperture_lp": true,
    "warm_start": false,
    "boundary_cuts_file": "boundary.csv",
    "boundary_cuts_mode": "combined",
    "boundary_max_iterations": 50,
    "named_cuts_file": "named.csv",
    "max_cuts_per_phase": 100,
    "cut_prune_interval": 5,
    "prune_dual_threshold": 1e-6,
    "single_cut_storage": true,
    "max_stored_cuts": 500,
    "simulation_mode": true,
    "stationary_tol": 0.01,
    "stationary_window": 15,
    "forward_solver_options": {
      "algorithm": 3,
      "threads": 4,
      "presolve": true,
      "log_level": 0,
      "reuse_basis": false
    },
    "backward_solver_options": {
      "algorithm": 2,
      "threads": 1,
      "presolve": false,
      "log_level": 0,
      "reuse_basis": true
    }
  })";

  const auto opts = daw::json::from_json<SddpOptions>(json_data);

  REQUIRE(opts.cut_sharing_mode.has_value());
  CHECK(*opts.cut_sharing_mode == CutSharingMode::expected);
  REQUIRE(opts.cut_directory.has_value());
  CHECK(*opts.cut_directory == "my_cuts");
  REQUIRE(opts.api_enabled.has_value());
  CHECK(*opts.api_enabled == true);
  REQUIRE(opts.update_lp_skip.has_value());
  CHECK(*opts.update_lp_skip == 2);
  REQUIRE(opts.max_iterations.has_value());
  CHECK(*opts.max_iterations == 200);
  REQUIRE(opts.min_iterations.has_value());
  CHECK(*opts.min_iterations == 5);
  REQUIRE(opts.convergence_tol.has_value());
  CHECK(*opts.convergence_tol == doctest::Approx(1e-3));
  REQUIRE(opts.elastic_penalty.has_value());
  CHECK(*opts.elastic_penalty == doctest::Approx(1e5));
  REQUIRE(opts.alpha_min.has_value());
  CHECK(*opts.alpha_min == doctest::Approx(0.0));
  REQUIRE(opts.alpha_max.has_value());
  CHECK(*opts.alpha_max == doctest::Approx(1e10));
  REQUIRE(opts.cut_recovery_mode.has_value());
  CHECK(*opts.cut_recovery_mode == HotStartMode::append);
  REQUIRE(opts.recovery_mode.has_value());
  CHECK(*opts.recovery_mode == RecoveryMode::cuts);
  REQUIRE(opts.save_per_iteration.has_value());
  CHECK(*opts.save_per_iteration == false);
  REQUIRE(opts.cuts_input_file.has_value());
  CHECK(*opts.cuts_input_file == "init.csv");
  REQUIRE(opts.sentinel_file.has_value());
  CHECK(*opts.sentinel_file == "stop.flag");
  REQUIRE(opts.elastic_mode.has_value());
  CHECK(*opts.elastic_mode == ElasticFilterMode::multi_cut);
  REQUIRE(opts.multi_cut_threshold.has_value());
  CHECK(*opts.multi_cut_threshold == 20);
  REQUIRE(opts.apertures.has_value());
  REQUIRE(opts.apertures->size() == 3);
  CHECK((*opts.apertures)[0] == 1);
  CHECK((*opts.apertures)[1] == 2);
  CHECK((*opts.apertures)[2] == 3);
  REQUIRE(opts.aperture_directory.has_value());
  CHECK(*opts.aperture_directory == "ap_data");
  REQUIRE(opts.aperture_timeout.has_value());
  CHECK(*opts.aperture_timeout == doctest::Approx(30.0));
  REQUIRE(opts.save_aperture_lp.has_value());
  CHECK(*opts.save_aperture_lp == true);
  REQUIRE(opts.warm_start.has_value());
  CHECK(*opts.warm_start == false);
  REQUIRE(opts.boundary_cuts_file.has_value());
  CHECK(*opts.boundary_cuts_file == "boundary.csv");
  REQUIRE(opts.boundary_cuts_mode.has_value());
  CHECK(*opts.boundary_cuts_mode == BoundaryCutsMode::combined);
  REQUIRE(opts.boundary_max_iterations.has_value());
  CHECK(*opts.boundary_max_iterations == 50);
  REQUIRE(opts.named_cuts_file.has_value());
  CHECK(*opts.named_cuts_file == "named.csv");
  REQUIRE(opts.max_cuts_per_phase.has_value());
  CHECK(*opts.max_cuts_per_phase == 100);
  REQUIRE(opts.cut_prune_interval.has_value());
  CHECK(*opts.cut_prune_interval == 5);
  REQUIRE(opts.prune_dual_threshold.has_value());
  CHECK(*opts.prune_dual_threshold == doctest::Approx(1e-6));
  REQUIRE(opts.single_cut_storage.has_value());
  CHECK(*opts.single_cut_storage == true);
  REQUIRE(opts.max_stored_cuts.has_value());
  CHECK(*opts.max_stored_cuts == 500);
  REQUIRE(opts.simulation_mode.has_value());
  CHECK(*opts.simulation_mode == true);
  REQUIRE(opts.stationary_tol.has_value());
  CHECK(*opts.stationary_tol == doctest::Approx(0.01));
  REQUIRE(opts.stationary_window.has_value());
  CHECK(*opts.stationary_window == 15);
  REQUIRE(opts.forward_solver_options.has_value());
  CHECK(opts.forward_solver_options->algorithm == LPAlgo::barrier);
  CHECK(opts.forward_solver_options->threads == 4);
  CHECK(opts.forward_solver_options->presolve == true);
  CHECK(opts.forward_solver_options->reuse_basis == false);
  REQUIRE(opts.backward_solver_options.has_value());
  CHECK(opts.backward_solver_options->algorithm == LPAlgo::dual);
  CHECK(opts.backward_solver_options->threads == 1);
  CHECK(opts.backward_solver_options->presolve == false);
  CHECK(opts.backward_solver_options->reuse_basis == true);
}

TEST_CASE("SddpOptions JSON - Missing fields stay nullopt")
{
  const std::string_view json_data = R"({
    "max_iterations": 50,
    "elastic_mode": "single_cut"
  })";

  const auto opts = daw::json::from_json<SddpOptions>(json_data);

  REQUIRE(opts.max_iterations.has_value());
  CHECK(*opts.max_iterations == 50);
  REQUIRE(opts.elastic_mode.has_value());
  CHECK(*opts.elastic_mode == ElasticFilterMode::single_cut);

  CHECK_FALSE(opts.cut_sharing_mode.has_value());
  CHECK_FALSE(opts.min_iterations.has_value());
  CHECK_FALSE(opts.convergence_tol.has_value());
  CHECK_FALSE(opts.apertures.has_value());
  CHECK_FALSE(opts.simulation_mode.has_value());
  CHECK_FALSE(opts.stationary_tol.has_value());
}

TEST_CASE("SddpOptions JSON - Empty apertures array")
{
  const std::string_view json_data = R"({
    "apertures": []
  })";

  const auto opts = daw::json::from_json<SddpOptions>(json_data);

  REQUIRE(opts.apertures.has_value());
  CHECK(opts.apertures->empty());
}

TEST_CASE("SddpOptions JSON - Round-trip serialization")
{
  const SddpOptions original {
      .cut_sharing_mode = CutSharingMode::accumulate,
      .max_iterations = 150,
      .convergence_tol = 1e-5,
      .elastic_mode = ElasticFilterMode::backpropagate,
      .apertures =
          Array<Uid> {
              10,
              20,
          },
      .warm_start = true,
      .stationary_tol = 0.02,
      .stationary_window = 25,
  };

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<SddpOptions>(json);

  CHECK(rt.cut_sharing_mode == original.cut_sharing_mode);
  CHECK(rt.max_iterations == original.max_iterations);
  CHECK(rt.convergence_tol == original.convergence_tol);
  CHECK(rt.elastic_mode == original.elastic_mode);
  REQUIRE(rt.apertures.has_value());
  REQUIRE(rt.apertures->size() == 2);
  CHECK((*rt.apertures)[0] == 10);
  CHECK((*rt.apertures)[1] == 20);
  CHECK(rt.warm_start == original.warm_start);
  CHECK(rt.stationary_tol == original.stationary_tol);
  CHECK(rt.stationary_window == original.stationary_window);

  CHECK_FALSE(rt.min_iterations.has_value());
  CHECK_FALSE(rt.elastic_penalty.has_value());
}

TEST_CASE("SddpOptions JSON - Empty object")
{
  const std::string_view json_data = R"({})";
  const auto opts = daw::json::from_json<SddpOptions>(json_data);

  CHECK_FALSE(opts.max_iterations.has_value());
  CHECK_FALSE(opts.cut_sharing_mode.has_value());
  CHECK_FALSE(opts.apertures.has_value());
}

TEST_CASE("SddpOptions JSON - Forward/backward solver options round-trip")
{
  const SddpOptions original {
      .max_iterations = 100,
      .forward_solver_options =
          SolverOptions {
              .algorithm = LPAlgo::barrier,
              .threads = 4,
              .presolve = true,
          },
      .backward_solver_options =
          SolverOptions {
              .algorithm = LPAlgo::dual,
              .threads = 1,
              .reuse_basis = true,
          },
  };

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<SddpOptions>(json);

  REQUIRE(rt.max_iterations.has_value());
  CHECK(*rt.max_iterations == 100);
  REQUIRE(rt.forward_solver_options.has_value());
  CHECK(rt.forward_solver_options->algorithm == LPAlgo::barrier);
  CHECK(rt.forward_solver_options->threads == 4);
  CHECK(rt.forward_solver_options->presolve == true);
  REQUIRE(rt.backward_solver_options.has_value());
  CHECK(rt.backward_solver_options->algorithm == LPAlgo::dual);
  CHECK(rt.backward_solver_options->threads == 1);
  CHECK(rt.backward_solver_options->reuse_basis == true);
}

TEST_CASE("SddpOptions JSON - cut_coeff_max parsing and round-trip")
{
  SUBCASE("default when missing is nullopt")
  {
    const std::string_view json_data = R"({})";
    const auto opts = daw::json::from_json<SddpOptions>(json_data);
    CHECK_FALSE(opts.cut_coeff_max.has_value());
  }

  SUBCASE("explicit value")
  {
    const std::string_view json_data = R"({"cut_coeff_max": 1e6})";
    const auto opts = daw::json::from_json<SddpOptions>(json_data);
    REQUIRE(opts.cut_coeff_max.has_value());
    CHECK(opts.cut_coeff_max.value_or(0.0) == doctest::Approx(1e6));
  }

  SUBCASE("round-trip")
  {
    SddpOptions original;
    original.cut_coeff_max = 1e7;

    const auto json = daw::json::to_json(original);
    const auto rt = daw::json::from_json<SddpOptions>(json);
    REQUIRE(rt.cut_coeff_max.has_value());
    CHECK(rt.cut_coeff_max.value_or(0.0) == doctest::Approx(1e7));
  }
}

TEST_CASE("SddpOptions JSON - cut_coeff_eps parsing and round-trip")
{
  SUBCASE("default when missing is nullopt")
  {
    const std::string_view json_data = R"({})";
    const auto opts = daw::json::from_json<SddpOptions>(json_data);
    CHECK_FALSE(opts.cut_coeff_eps.has_value());
  }

  SUBCASE("explicit value")
  {
    const std::string_view json_data = R"({"cut_coeff_eps": 1e-10})";
    const auto opts = daw::json::from_json<SddpOptions>(json_data);
    REQUIRE(opts.cut_coeff_eps.has_value());
    CHECK(opts.cut_coeff_eps.value_or(0.0) == doctest::Approx(1e-10));
  }

  SUBCASE("round-trip")
  {
    SddpOptions original;
    original.cut_coeff_eps = 1e-8;

    const auto json = daw::json::to_json(original);
    const auto rt = daw::json::from_json<SddpOptions>(json);
    REQUIRE(rt.cut_coeff_eps.has_value());
    CHECK(rt.cut_coeff_eps.value_or(0.0) == doctest::Approx(1e-8));
  }
}
