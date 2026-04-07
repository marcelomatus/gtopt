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

#include <numbers>

#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/variable_scale.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

// ── SparseCol::scale field ──────────────────────────────────────────────────

TEST_CASE("SparseCol default scale is 1.0")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SparseCol col {};
  CHECK(col.scale == doctest::Approx(1.0));
}

TEST_CASE("SparseCol scale via designated initializer")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SparseCol col {
      .name = "theta_b1",
      .lowb = -std::numbers::pi,
      .uppb = +std::numbers::pi,
      .scale = 0.001,
  };
  CHECK(col.name == "theta_b1");
  CHECK(col.scale == doctest::Approx(0.001));
}

TEST_CASE("SparseCol scale with energy_scale")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const VariableScale vs {};
  CHECK(vs.class_name.empty());
  CHECK(vs.variable.empty());
  CHECK(vs.uid == unknown_uid);
  CHECK(vs.scale == doctest::Approx(1.0));
}

TEST_CASE("VariableScale designated initializer")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const VariableScaleMap map;
  CHECK(map.empty());
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(1.0));
  CHECK(map.lookup("Reservoir", "energy", Uid {42}) == doctest::Approx(1.0));
}

TEST_CASE("VariableScaleMap per-class lookup")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LinearProblem lp("lifecycle_test");

  // Simulate bus theta: physical bounds in radians, scale = scale_theta.
  // flatten() converts to LP units by dividing by scale.
  constexpr double scale_theta = 0.001;  // 1/1000
  const auto theta = lp.add_col(SparseCol {
      .name = "theta_b1",
      .lowb = -std::numbers::pi,
      .uppb = +std::numbers::pi,
      .scale = scale_theta,
  });

  // Simulate reservoir energy: physical bounds in hm³, scale = 100000.
  // flatten() converts to LP units by dividing by scale.
  constexpr double energy_scale = 100000.0;
  const auto vol = lp.add_col(SparseCol {
      .name = "vol_r1",
      .lowb = 500.0,
      .uppb = 2000.0,
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
  const double theta_lp = 1570.0;  // ~pi/2 / 0.001
  const double vol_lp = 0.01;  // 1000 dam³ / 100000

  // Physical values: physical = LP × scale
  CHECK(theta_sol(theta_lp) == doctest::Approx(1.57));  // 1570 × 0.001
  CHECK(vol_sol(vol_lp) == doctest::Approx(1000.0));  // 0.01 × 100000

  // Reduced costs: rc_physical = rc_LP / scale
  CHECK(theta_cost(1.0) == doctest::Approx(1000.0));  // 1 / 0.001
  CHECK(vol_cost(1.0) == doctest::Approx(1.0 / 100000.0));

  // Invariance: obj_contribution = rc_LP × x_LP = rc_phys × x_phys
  const double rc_lp_theta = 0.5;
  const double obj_lp = rc_lp_theta * theta_lp;
  const double obj_phys = theta_cost(rc_lp_theta) * theta_sol(theta_lp);
  CHECK(obj_phys == doctest::Approx(obj_lp));
}

// ── LinearProblem auto-scale via VariableScaleMap ──────────────────────────

TEST_CASE("LinearProblem add_col auto-resolves scale from metadata")  // NOLINT
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Bus",
          .variable = "theta",
          .scale = 0.0002,
      },
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .scale = 1000.0,
      },
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .uid = Uid {7},
          .scale = 500.0,
      },
  };
  const VariableScaleMap map(scales);

  LinearProblem lp("auto_scale_test");
  lp.set_variable_scale_map(map);

  SUBCASE("class_name metadata triggers lookup")
  {
    const auto c = lp.add_col(SparseCol {
        .name = "theta_b1",
        .class_name = "Bus",
        .variable_name = "theta",
        .variable_uid = Uid {1},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(0.0002));
  }

  SUBCASE("per-element UID match wins over per-class")
  {
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_energy_7",
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {7},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(500.0));
  }

  SUBCASE("per-class match for non-matching UID")
  {
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_energy_99",
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {99},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(1000.0));
  }

  SUBCASE("map entry overrides pre-set scale when class_name is set")
  {
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_energy_override",
        .scale = 42.0,
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {1},
    });
    // Map entry (1000.0 per-class) wins over pre-set scale (42.0)
    // because class_name metadata opts into map resolution.
    CHECK(lp.get_col_scale(c) == doctest::Approx(1000.0));
  }

  SUBCASE("explicit scale without class_name is immune to map")
  {
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_energy_explicit",
        .scale = 42.0,
    });
    // No class_name → no lookup, explicit scale preserved
    CHECK(lp.get_col_scale(c) == doctest::Approx(42.0));
  }

  SUBCASE("empty class_name skips lookup")
  {
    const auto c = lp.add_col(SparseCol {
        .name = "gen_output",
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(1.0));
  }

  SUBCASE("no map set returns default scale")
  {
    LinearProblem lp2("no_map");
    const auto c = lp2.add_col(SparseCol {
        .name = "theta_no_map",
        .class_name = "Bus",
        .variable_name = "theta",
        .variable_uid = Uid {1},
    });
    CHECK(lp2.get_col_scale(c) == doctest::Approx(1.0));
  }
}

// ── VariableScaleMap hash-based structure tests ─────────────────────────────

TEST_CASE(
    "VariableScaleMap transparent hash lookup avoids allocations")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<VariableScale> scales {
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .scale = 1000.0,
      },
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .uid = Uid {5},
          .scale = 500.0,
      },
      {
          .class_name = "Bus",
          .variable = "theta",
          .scale = 0.001,
      },
      {
          .class_name = "Battery",
          .variable = "flow",
          .uid = Uid {3},
          .scale = 2.0,
      },
  };
  const VariableScaleMap map(scales);

  SUBCASE("per-class lookup with string_view")
  {
    // These string_views do NOT own the data — no allocation
    const std::string_view cls = "Reservoir";
    const std::string_view var = "energy";
    CHECK(map.lookup(cls, var) == doctest::Approx(1000.0));
  }

  SUBCASE("per-element lookup with string_view")
  {
    CHECK(map.lookup("Reservoir", "energy", Uid {5}) == doctest::Approx(500.0));
  }

  SUBCASE("per-element miss falls back to per-class")
  {
    CHECK(map.lookup("Reservoir", "energy", Uid {99})
          == doctest::Approx(1000.0));
  }

  SUBCASE("complete miss returns 1.0")
  {
    CHECK(map.lookup("Generator", "output") == doctest::Approx(1.0));
    CHECK(map.lookup("Reservoir", "flow") == doctest::Approx(1.0));
  }

  SUBCASE("element-only entry (no per-class fallback)")
  {
    // Battery/flow has only a per-element entry for uid=3
    CHECK(map.lookup("Battery", "flow", Uid {3}) == doctest::Approx(2.0));
    CHECK(map.lookup("Battery", "flow", Uid {7}) == doctest::Approx(1.0));
    CHECK(map.lookup("Battery", "flow") == doctest::Approx(1.0));
  }

  SUBCASE("empty map returns 1.0 for everything")
  {
    const VariableScaleMap empty_map;
    CHECK(empty_map.empty());
    CHECK(empty_map.lookup("Bus", "theta") == doctest::Approx(1.0));
    CHECK(empty_map.lookup("Bus", "theta", Uid {1}) == doctest::Approx(1.0));
  }
}

TEST_CASE("VariableScaleMap duplicate entries use last value")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two entries for the same key — unordered_map::emplace keeps the first
  const std::vector<VariableScale> scales {
      {
          .class_name = "Bus",
          .variable = "theta",
          .scale = 0.001,
      },
      {
          .class_name = "Bus",
          .variable = "theta",
          .scale = 0.002,
      },
  };
  const VariableScaleMap map(scales);

  // emplace keeps the first insertion
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(0.001));
}

// ── add_col: map overrides pre-set scale ────────────────────────────────────

TEST_CASE("add_col map entry overrides auto_scale pre-set value")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<VariableScale> scales {
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .uid = Uid {1},
          .scale = 10.0,
      },
  };
  const VariableScaleMap map(scales);

  LinearProblem lp("override_test");
  lp.set_variable_scale_map(map);

  SUBCASE("map entry overrides auto_scale pre-set")
  {
    // Simulates reservoir auto_scale setting scale=1000, but map has 10.0
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_eini",
        .scale = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {1},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(10.0));
  }

  SUBCASE("no map entry keeps pre-set scale")
  {
    // Different uid — no map entry, pre-set scale preserved
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_eini_99",
        .scale = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {99},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(1000.0));
  }

  SUBCASE("per-element field without class_name is immune")
  {
    // Simulates explicit reservoir().energy_scale — no class_name set
    const auto c = lp.add_col(SparseCol {
        .name = "rsv_explicit",
        .scale = 42.0,
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(42.0));
  }
}

}  // namespace
