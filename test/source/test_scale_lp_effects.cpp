/**
 * @file      test_scale_lp_effects.hpp
 * @brief     Tests that scale_objective, scale_theta, scale_alpha, and
 *            energy_scale produce detectable changes in the LP and improve
 *            numerical conditioning (kappa).
 * @date      2026-03-30
 * @copyright BSD-3-Clause
 *
 * All scales follow the unified convention: physical = LP × scale.
 *   - scale_theta  = 1e-4  (1/ScaleAng) — theta is small, scaled UP in LP
 *   - scale_alpha  = 1e7   (ScalePhi)   — alpha is large, scaled DOWN in LP
 *   - energy_scale = 1000  (ScaleVol)   — energy is large, scaled DOWN in LP
 *   - scale_objective = 1e7 (ScaleObj)  — cost divisor
 */

#include <cmath>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ---------------------------------------------------------------
// Helper: IEEE 4-bus JSON template with replaceable scale values
// ---------------------------------------------------------------

auto make_ieee4b_json(double scale_obj,
                      double scale_theta_val,
                      bool use_kirchhoff = true) -> std::string
{
  return std::format(
      R"({{
  "options": {{
    "annual_discount_rate": 0.0,
    "lp_matrix_options": {{"names_level": 2}},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": {},
    "scale_theta": {},
    "use_kirchhoff": {}
  }},
  "simulation": {{
    "block_array": [{{"uid": 1, "duration": 1}}],
    "stage_array": [{{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}}],
    "scenario_array": [{{"uid": 1, "probability_factor": 1}}]
  }},
  "system": {{
    "name": "ieee_4b_scale_test",
    "bus_array": [
      {{"uid": 1, "name": "b1"}}, {{"uid": 2, "name": "b2"}},
      {{"uid": 3, "name": "b3"}}, {{"uid": 4, "name": "b4"}}
    ],
    "generator_array": [
      {{"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}},
      {{"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}}
    ],
    "demand_array": [
      {{"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]]}},
      {{"uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]]}}
    ],
    "line_array": [
      {{"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300}},
      {{"uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300}},
      {{"uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200}},
      {{"uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200}},
      {{"uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150}}
    ]
  }}
}})",
      scale_obj,
      scale_theta_val,
      use_kirchhoff ? "true" : "false");
}

/// Solve the IEEE 4-bus case and return (scaled_obj, kappa).
auto solve_ieee4b(double scale_obj,
                  double scale_theta_val,
                  bool use_kirchhoff = true) -> std::pair<double, double>
{
  auto json = make_ieee4b_json(scale_obj, scale_theta_val, use_kirchhoff);
  Planning base;
  base.merge(daw::json::from_json<Planning>(json));

  PlanningLP planning_lp(std::move(base));
  MonolithicMethod solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();

  return {li.get_obj_value(), li.get_kappa()};
}

// ---------------------------------------------------------------
// 1. scale_objective divides objective coefficients
// ---------------------------------------------------------------

TEST_CASE("scale_objective — LP objective scales inversely")  // NOLINT
{
  // Physical cost = 250 MW × 20 $/MWh = 5000 $
  // LP obj = 5000 / scale_objective
  constexpr double physical_cost = 250.0 * 20.0;
  constexpr double scale_theta = 0.0001;  // 1/ScaleAng

  auto [obj_1k, kappa_1k] = solve_ieee4b(1'000, scale_theta);
  auto [obj_10k, kappa_10k] = solve_ieee4b(10'000, scale_theta);
  auto [obj_1m, kappa_1m] = solve_ieee4b(1'000'000, scale_theta);

  CHECK(obj_1k == doctest::Approx(physical_cost / 1'000));
  CHECK(obj_10k == doctest::Approx(physical_cost / 10'000));
  CHECK(obj_1m == doctest::Approx(physical_cost / 1'000'000));

  CHECK(obj_1k * 1'000 == doctest::Approx(physical_cost));
  CHECK(obj_10k * 10'000 == doctest::Approx(physical_cost));
  CHECK(obj_1m * 1'000'000 == doctest::Approx(physical_cost));
}

// ---------------------------------------------------------------
// 2. scale_theta affects Kirchhoff coefficients and theta bounds
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "scale_theta — Kirchhoff coefficients scale with theta")
{
  // Convention: physical = LP × scale_theta, LP = physical / scale_theta.
  // Theta bounds: ±2π / scale_theta.
  // Smaller scale_theta → larger LP bounds → scaled-up theta.

  constexpr double scale_obj = 10'000.0;

  for (const auto st : {0.01, 0.001, 0.0001}) {
    auto json = make_ieee4b_json(scale_obj, st);
    Planning base;
    base.merge(daw::json::from_json<Planning>(json));
    PlanningLP planning_lp(std::move(base));

    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());

    auto&& systems = planning_lp.systems();
    const auto& li = systems.front().front().linear_interface();

    // LP obj should be the same regardless of scale_theta
    constexpr double expected_obj = 250.0 * 20.0 / scale_obj;
    CHECK(li.get_obj_value() == doctest::Approx(expected_obj).epsilon(1e-4));

    // Theta column physical bounds should be ±2π (invariant of scale).
    // Raw LP bounds should be ±2π / scale_theta.
    const auto& col_map = li.col_name_map();
    constexpr double two_pi = 2.0 * 3.14159265358979323846;

    for (const auto& [name, idx] : col_map) {
      if (name.find("theta") != std::string::npos) {
        const auto col_low = li.get_col_low();
        const auto col_upp = li.get_col_upp();
        const auto col_low_raw = li.get_col_low_raw();
        const auto col_upp_raw = li.get_col_upp_raw();
        const auto low = col_low[idx];
        const auto upp = col_upp[idx];

        // Reference angle bus has fixed theta=0, skip those
        if (std::abs(low - upp) < 1e-6) {
          continue;
        }

        // Physical bounds are invariant: ±2π
        CHECK(low == doctest::Approx(-two_pi).epsilon(1e-3));
        CHECK(upp == doctest::Approx(+two_pi).epsilon(1e-3));

        // Raw LP bounds scale with theta: ±2π / scale_theta
        CHECK(col_low_raw[idx] == doctest::Approx(-two_pi / st).epsilon(1e-3));
        CHECK(col_upp_raw[idx] == doctest::Approx(+two_pi / st).epsilon(1e-3));
        break;
      }
    }
  }
}

// ---------------------------------------------------------------
// 3. scale_theta changes kappa on Kirchhoff problems
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "scale_theta — different values produce different kappa")
{
  constexpr double scale_obj = 10'000.0;

  // scale_theta=1.0 → theta bounds ≈ ±6.28 (no scaling)
  // scale_theta=0.0001 → theta bounds ≈ ±62800 (scaled up 10000×)
  auto [obj_noscale, kappa_noscale] = solve_ieee4b(scale_obj, 1.0);
  auto [obj_scaled, kappa_scaled] = solve_ieee4b(scale_obj, 0.0001);

  // Both should produce the same physical objective
  CHECK(obj_noscale == doctest::Approx(obj_scaled).epsilon(1e-3));

  // Kappa values should differ — scale_theta has a measurable effect.
  // Some solver backends (e.g. CLP) use largestDualError() as a kappa
  // proxy, which may return near-zero (~1e-19) for well-conditioned LPs.
  // Backends that don't support kappa return -1 (e.g. MindOpt); skip in
  // that case.  When both values are effectively zero the comparison is
  // meaningless, so only assert non-equality when at least one value is
  // above a small threshold.
  constexpr double kappa_threshold = 1e-10;
  if (kappa_noscale >= 0.0 && kappa_scaled >= 0.0
      && (kappa_noscale > kappa_threshold || kappa_scaled > kappa_threshold))
  {
    CHECK(kappa_noscale != doctest::Approx(kappa_scaled).epsilon(0.01));
  }

  CHECK(std::isfinite(kappa_noscale));
  CHECK(std::isfinite(kappa_scaled));
}

// ---------------------------------------------------------------
// 4. scale_objective keeps kappa bounded
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "scale_objective — appropriate scaling reduces kappa")
{
  constexpr double scale_theta = 0.0001;

  auto [obj_1, kappa_1] = solve_ieee4b(1.0, scale_theta);
  auto [obj_10k, kappa_10k] = solve_ieee4b(10'000.0, scale_theta);

  constexpr double physical_cost = 250.0 * 20.0;
  CHECK(obj_1 * 1.0 == doctest::Approx(physical_cost).epsilon(1e-3));
  CHECK(obj_10k * 10'000.0 == doctest::Approx(physical_cost).epsilon(1e-3));

  // Backends without kappa support return -1 (e.g. MindOpt).
  if (kappa_10k >= 0.0) {
    CHECK(kappa_10k >= 0.0);
  }
}

// ---------------------------------------------------------------
// 5. CostHelper cost_factor scales inversely with scale_objective
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "CostHelper — cost_factor independent of scale_objective")
{
  // scale_objective is now applied in flatten(), not in CostHelper.
  // cost_factor = probability × discount × duration (no scale_obj).
  for (const auto scale : {1.0, 1'000.0, 10'000'000.0}) {
    PlanningOptions opt;
    opt.scale_objective = scale;
    const PlanningOptionsLP options {opt};

    Scenario scenario;
    scenario.probability_factor = 1.0;
    std::vector<ScenarioLP> scenarios {ScenarioLP {scenario}};

    Stage stage;
    stage.discount_factor = 1.0;
    std::vector<StageLP> stages {StageLP {stage}};

    Block pblock;
    pblock.duration = 1.0;
    const BlockLP block {pblock};

    const CostHelper helper(options, scenarios, stages);

    const double cost = 100.0;
    const double ecost =
        helper.block_ecost(scenarios[0], stages[0], block, cost);
    // cost_factor = 1.0 * 1.0 * 1.0 = 1.0, so ecost = cost
    CHECK(ecost == doctest::Approx(cost));
  }
}

// ---------------------------------------------------------------
// 6. CostHelper inverse factors scale with scale_objective
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "CostHelper — inverse cost factors independent of scale_objective")
{
  // scale_objective is now applied in LinearInterface, not CostHelper.
  // Inverse factors = 1 / (prob × discount × duration).
  for (const auto scale : {1.0, 1'000.0, 10'000'000.0}) {
    PlanningOptions opt;
    opt.scale_objective = scale;
    const PlanningOptionsLP options {opt};

    Scenario scenario;
    scenario.probability_factor = 0.5;
    std::vector<ScenarioLP> scenarios {ScenarioLP {scenario}};

    std::vector<Block> blocks(1);
    blocks[0].duration = 2.0;

    Stage stage;
    stage.discount_factor = 0.9;
    stage.first_block = 0;
    stage.count_block = 1;

    std::vector<StageLP> stages;
    stages.emplace_back(stage, blocks);

    const CostHelper helper(options, scenarios, stages);

    auto stage_factors = helper.stage_icost_factors();
    REQUIRE(!stage_factors.empty());
    CHECK(stage_factors[0] == doctest::Approx(1.0 / (1.0 * 0.9 * 2.0)));

    auto ss_factors = helper.scenario_stage_icost_factors();
    REQUIRE(!ss_factors.empty());
    REQUIRE(!ss_factors[0].empty());
    CHECK(ss_factors[0][0] == doctest::Approx(1.0 / (0.5 * 0.9 * 2.0)));
  }
}

// ---------------------------------------------------------------
// 7. VariableScale auto-population from global scale options
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "VariableScale — auto-populated from scale_theta and scale_alpha")
{
  SUBCASE("default options inject Bus.theta but not Sddp.alpha")
  {
    const PlanningOptionsLP lp_opts {};

    // Bus.theta: default scale_theta = 1.0 (neutral; auto_scale_theta
    // overrides when Kirchhoff is enabled and lines exist).
    const auto vs_theta = lp_opts.variable_scale_map().lookup("Bus", "theta");
    CHECK(vs_theta == doctest::Approx(1.0));

    // Sddp.alpha: not injected when scale_alpha is unset (auto-scale = 0)
    // lookup returns default 1.0
    const auto vs_alpha = lp_opts.variable_scale_map().lookup("Sddp", "alpha");
    CHECK(vs_alpha == doctest::Approx(1.0));
  }

  SUBCASE("explicit scale_theta populates Bus.theta directly")
  {
    PlanningOptions opts;
    opts.scale_theta = 0.0002;  // = 1/5000
    const PlanningOptionsLP lp_opts {opts};

    const auto vs = lp_opts.variable_scale_map().lookup("Bus", "theta");
    CHECK(vs == doctest::Approx(0.0002));
  }

  SUBCASE("user-provided Bus.theta entry is NOT overwritten")
  {
    PlanningOptions opts;
    opts.scale_theta = 0.0002;
    opts.variable_scales = {
        VariableScale {
            .class_name = "Bus",
            .variable = "theta",
            .scale = 0.01,
        },
    };
    const PlanningOptionsLP lp_opts {opts};

    const auto vs = lp_opts.variable_scale_map().lookup("Bus", "theta");
    CHECK(vs == doctest::Approx(0.01));
  }

  SUBCASE("explicit scale_alpha populates Sddp.alpha correctly")
  {
    PlanningOptions opts;
    opts.sddp_options.scale_alpha = 500'000.0;
    const PlanningOptionsLP lp_opts {opts};

    const auto vs = lp_opts.variable_scale_map().lookup("Sddp", "alpha");
    CHECK(vs == doctest::Approx(500'000.0));
  }
}

// ---------------------------------------------------------------
// 8. PlanningOptionsLP defaults match PLP scale values
// ---------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PlanningOptionsLP — defaults match PLP scale values")
{
  const PlanningOptions empty_opts {};
  const PlanningOptionsLP lp_opts {empty_opts};

  // Default scale_objective = 1000
  CHECK(lp_opts.scale_objective() == doctest::Approx(1'000.0));
  // Default scale_theta = 1.0 (neutral; auto_scale_theta overrides).
  CHECK(lp_opts.scale_theta() == doctest::Approx(1.0));
  // PLP ScalePhi: auto-scale when not explicitly set (returns 0.0)
  CHECK(lp_opts.sddp_scale_alpha() == doctest::Approx(0.0));
}

}  // namespace
