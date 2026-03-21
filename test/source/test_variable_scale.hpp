/**
 * @file      test_variable_scale.hpp
 * @brief     Tests for LP variable scaling architecture
 * @date      Mon Mar 17 04:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for:
 * - SparseCol::scale field (physical = LP × scale)
 * - LinearProblem::get_col_scale() accessor
 * - col_scale_sol() / col_scale_cost() output rescaling helpers
 * - VariableScale struct and VariableScaleMap lookup
 */

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── SparseCol::scale field ──────────────────────────────────────────────────

TEST_CASE("SparseCol default scale is 1.0")  // NOLINT
{
  const SparseCol col {};
  CHECK(col.scale == doctest::Approx(1.0));
}

TEST_CASE("SparseCol scale via designated initializer")  // NOLINT
{
  const SparseCol col {
      .name = "theta_b1",
      .lowb = -3.14,
      .uppb = +3.14,
      .scale = 0.001,
  };
  CHECK(col.name == "theta_b1");
  CHECK(col.scale == doctest::Approx(0.001));
}

TEST_CASE("SparseCol scale with energy_scale")  // NOLINT
{
  const SparseCol col {
      .name = "vol_r1",
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 0.5,
      .scale = 100000.0,
  };
  CHECK(col.scale == doctest::Approx(100000.0));
}

// ── LinearProblem::get_col_scale() ──────────────────────────────────────────

TEST_CASE("LinearProblem get_col_scale returns stored scale")  // NOLINT
{
  LinearProblem lp("scale_test");

  const auto c1 = lp.add_col(SparseCol {
      .name = "theta",
      .scale = 0.001,
  });
  const auto c2 = lp.add_col(SparseCol {
      .name = "volume",
      .scale = 1000.0,
  });
  const auto c3 = lp.add_col(SparseCol {
      .name = "generation",
  });

  CHECK(lp.get_col_scale(c1) == doctest::Approx(0.001));
  CHECK(lp.get_col_scale(c2) == doctest::Approx(1000.0));
  CHECK(lp.get_col_scale(c3) == doctest::Approx(1.0));
}

// ── col_scale_sol() helper ──────────────────────────────────────────────────

TEST_CASE("col_scale_sol rescales LP primal to physical")  // NOLINT
{
  SUBCASE("reservoir volume: physical = LP × 100000")
  {
    const auto rescale = col_scale_sol(100000.0);
    CHECK(rescale(0.5) == doctest::Approx(50000.0));
    CHECK(rescale(0.0) == doctest::Approx(0.0));
    CHECK(rescale(-1.0) == doctest::Approx(-100000.0));
  }

  SUBCASE("theta: physical = LP × 0.001")
  {
    const auto rescale = col_scale_sol(0.001);
    CHECK(rescale(3141.59) == doctest::Approx(3.14159));
  }

  SUBCASE("no scaling: scale = 1.0")
  {
    const auto rescale = col_scale_sol(1.0);
    CHECK(rescale(42.0) == doctest::Approx(42.0));
  }
}

// ── col_scale_cost() helper ─────────────────────────────────────────────────

TEST_CASE("col_scale_cost rescales LP reduced cost to physical")  // NOLINT
{
  SUBCASE("reservoir volume: rc_phys = rc_LP / 100000")
  {
    const auto rescale = col_scale_cost(100000.0);
    CHECK(rescale(100000.0) == doctest::Approx(1.0));
    CHECK(rescale(50000.0) == doctest::Approx(0.5));
  }

  SUBCASE("theta: rc_phys = rc_LP / 0.001 = rc_LP × 1000")
  {
    const auto rescale = col_scale_cost(0.001);
    CHECK(rescale(0.005) == doctest::Approx(5.0));
  }

  SUBCASE("invariance: sol_rescale × cost_rescale = identity")
  {
    constexpr double scale = 1000.0;
    const auto sol_r = col_scale_sol(scale);
    const auto cost_r = col_scale_cost(scale);

    // cost_phys × sol_phys = (cost_LP / scale) × (sol_LP × scale) =
    //                        cost_LP × sol_LP
    const double lp_sol = 5.0;
    const double lp_cost = 2.0;
    const double product_lp = lp_sol * lp_cost;
    const double product_phys = sol_r(lp_sol) * cost_r(lp_cost);
    CHECK(product_phys == doctest::Approx(product_lp));
  }
}

// ── VariableScale struct ────────────────────────────────────────────────────

TEST_CASE("VariableScale default construction")  // NOLINT
{
  const VariableScale vs {};
  CHECK(vs.class_name.empty());
  CHECK(vs.variable.empty());
  CHECK(vs.uid == unknown_uid);
  CHECK(vs.scale == doctest::Approx(1.0));
}

TEST_CASE("VariableScale designated initializer")  // NOLINT
{
  const VariableScale vs {
      .class_name = "Reservoir",
      .variable = "energy",
      .scale = 100000.0,
  };
  CHECK(vs.class_name == "Reservoir");
  CHECK(vs.variable == "energy");
  CHECK(vs.uid == unknown_uid);
  CHECK(vs.scale == doctest::Approx(100000.0));
}

// ── VariableScaleMap lookup ─────────────────────────────────────────────────

TEST_CASE("VariableScaleMap default lookup returns 1.0")  // NOLINT
{
  const VariableScaleMap map;
  CHECK(map.empty());
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(1.0));
  CHECK(map.lookup("Reservoir", "energy", Uid {42}) == doctest::Approx(1.0));
}

TEST_CASE("VariableScaleMap per-class lookup")  // NOLINT
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Bus",
          .variable = "theta",
          .scale = 0.001,
      },
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .scale = 100000.0,
      },
  };
  const VariableScaleMap map(scales);

  CHECK_FALSE(map.empty());
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(0.001));
  CHECK(map.lookup("Reservoir", "energy") == doctest::Approx(100000.0));
  CHECK(map.lookup("Generator", "output") == doctest::Approx(1.0));
}

TEST_CASE(
    "VariableScaleMap priority: per-element > per-class > default")  // NOLINT
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .scale = 100000.0,
      },
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .uid = Uid {42},
          .scale = 50000.0,
      },
  };
  const VariableScaleMap map(scales);

  // Per-element match wins for UID 42
  CHECK(map.lookup("Reservoir", "energy", Uid {42})
        == doctest::Approx(50000.0));
  // Per-class match for other UIDs
  CHECK(map.lookup("Reservoir", "energy", Uid {99})
        == doctest::Approx(100000.0));
  // Per-class match when no UID given
  CHECK(map.lookup("Reservoir", "energy") == doctest::Approx(100000.0));
  // Default for unknown class/variable
  CHECK(map.lookup("Battery", "energy") == doctest::Approx(1.0));
}

// ── Integration: scale through LP lifecycle ─────────────────────────────────

TEST_CASE("Integration: LP column scale drives output rescaling")  // NOLINT
{
  LinearProblem lp("lifecycle_test");

  // Simulate bus theta: LP = physical × 1000, scale = 1/1000
  constexpr double scale_theta = 1000.0;
  const auto theta = lp.add_col(SparseCol {
      .name = "theta_b1",
      .lowb = -3.14 * scale_theta,
      .uppb = +3.14 * scale_theta,
      .scale = 1.0 / scale_theta,
  });

  // Simulate reservoir energy: LP = physical / 100000, scale = 100000
  constexpr double energy_scale = 100000.0;
  const auto vol = lp.add_col(SparseCol {
      .name = "vol_r1",
      .lowb = 500.0 / energy_scale,
      .uppb = 2000.0 / energy_scale,
      .scale = energy_scale,
  });

  // Retrieve scales
  const double ts = lp.get_col_scale(theta);
  const double vs = lp.get_col_scale(vol);

  // Create output rescalers from stored scales
  const auto theta_sol = col_scale_sol(ts);
  const auto theta_cost = col_scale_cost(ts);
  const auto vol_sol = col_scale_sol(vs);
  const auto vol_cost = col_scale_cost(vs);

  // Simulate LP solution values (internal LP units)
  const double theta_lp = 1570.0;  // ~pi/2 × 1000
  const double vol_lp = 0.01;  // 1000 dam³ / 100000

  // Physical values
  CHECK(theta_sol(theta_lp) == doctest::Approx(1.57));
  CHECK(vol_sol(vol_lp) == doctest::Approx(1000.0));

  // Reduced costs: if rc_LP = 1, what is rc_physical?
  CHECK(theta_cost(1.0) == doctest::Approx(1000.0));
  CHECK(vol_cost(1.0) == doctest::Approx(1.0 / 100000.0));

  // Invariance: obj_contribution = rc_LP × x_LP = rc_phys × x_phys
  const double rc_lp_theta = 0.5;
  const double obj_lp = rc_lp_theta * theta_lp;
  const double obj_phys = theta_cost(rc_lp_theta) * theta_sol(theta_lp);
  CHECK(obj_phys == doctest::Approx(obj_lp));
}

}  // namespace
