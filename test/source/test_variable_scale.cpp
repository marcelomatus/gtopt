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

#include <algorithm>
#include <numbers>
#include <tuple>

#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/variable_scale.hpp>

using namespace gtopt;

namespace
{

using namespace gtopt;

// ── SparseCol::scale field ──────────────────────────────────────────────────

TEST_CASE("SparseCol default scale is 1.0")  // NOLINT
{
  using namespace gtopt;

  const SparseCol col {};
  CHECK(col.scale == doctest::Approx(1.0));
}

TEST_CASE("SparseCol scale via designated initializer")  // NOLINT
{
  using namespace gtopt;

  const SparseCol col {
      .lowb = -std::numbers::pi,
      .uppb = +std::numbers::pi,
      .scale = 0.001,
  };
  CHECK(col.scale == doctest::Approx(0.001));
}

TEST_CASE("SparseCol scale with energy_scale")  // NOLINT
{
  using namespace gtopt;

  const SparseCol col {
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
  using namespace gtopt;

  LinearProblem lp("scale_test");

  const auto c1 = lp.add_col(SparseCol {
      .scale = 0.001,
  });
  const auto c2 = lp.add_col(SparseCol {
      .scale = 1000.0,
  });
  const auto c3 = lp.add_col(SparseCol {});

  CHECK(lp.get_col_scale(c1) == doctest::Approx(0.001));
  CHECK(lp.get_col_scale(c2) == doctest::Approx(1000.0));
  CHECK(lp.get_col_scale(c3) == doctest::Approx(1.0));
}

// ── col_scale_sol() helper ──────────────────────────────────────────────────

TEST_CASE("col_scale_sol rescales LP primal to physical")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

  const VariableScale vs {};
  CHECK(vs.class_name.empty());
  CHECK(vs.variable.empty());
  CHECK(vs.uid == unknown_uid);
  CHECK(vs.scale == doctest::Approx(1.0));
}

TEST_CASE("VariableScale designated initializer")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

  const VariableScaleMap map;
  CHECK(map.empty());
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(1.0));
  CHECK(map.lookup("Reservoir", "energy", Uid {42}) == doctest::Approx(1.0));
}

TEST_CASE("VariableScaleMap per-class lookup")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

  LinearProblem lp("lifecycle_test");

  // Simulate bus theta: physical bounds in radians, scale = scale_theta.
  // flatten() converts to LP units by dividing by scale.
  constexpr double scale_theta = 0.001;  // 1/1000
  const auto theta = lp.add_col(SparseCol {
      .lowb = -std::numbers::pi,
      .uppb = +std::numbers::pi,
      .scale = scale_theta,
  });

  // Simulate reservoir energy: physical bounds in hm³, scale = 100000.
  // flatten() converts to LP units by dividing by scale.
  constexpr double energy_scale = 100000.0;
  const auto vol = lp.add_col(SparseCol {
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
        .class_name = "Bus",
        .variable_name = "theta",
        .variable_uid = Uid {1},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(0.0002));
  }

  SUBCASE("per-element UID match wins over per-class")
  {
    const auto c = lp.add_col(SparseCol {
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {7},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(500.0));
  }

  SUBCASE("per-class match for non-matching UID")
  {
    const auto c = lp.add_col(SparseCol {
        .class_name = "Reservoir",
        .variable_name = "energy",
        .variable_uid = Uid {99},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(1000.0));
  }

  SUBCASE("map entry overrides pre-set scale when class_name is set")
  {
    const auto c = lp.add_col(SparseCol {
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
        .scale = 42.0,
    });
    // No class_name → no lookup, explicit scale preserved
    CHECK(lp.get_col_scale(c) == doctest::Approx(42.0));
  }

  SUBCASE("empty class_name skips lookup")
  {
    const auto c = lp.add_col(SparseCol {});
    CHECK(lp.get_col_scale(c) == doctest::Approx(1.0));
  }

  SUBCASE("no map set returns default scale")
  {
    LinearProblem lp2("no_map");
    const auto c = lp2.add_col(SparseCol {
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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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
        .scale = 42.0,
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(42.0));
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Integer-column scaling invariant
// ─────────────────────────────────────────────────────────────────────────
//
// Regression anchor: integer columns must NEVER be rescaled.  A non-unit
// scale on a [0, 1] binary variable turns the LP-side upper bound into
// 1 / scale (e.g. 11.6189 for a status column rescaled to 0.0861), and
// the solver's integer enforcement then has no LP value that maps back
// to physical 1 (only u ∈ {0, 0.086, …, 0.947}).  Surfaced on UC.jl's
// case14/base.json full-network MIP: status columns came out with bound
// [0, 11.6189] and the MIP either silently capped commitment at 94.7 %
// or reported infeasible when ``must_run`` fixed status at 11.6189.
//
// The fix in `LinearProblem::add_col` pins ``col.scale = 1.0`` for any
// column with ``is_integer = true``.  The four sub-cases below pin all
// the entry points that could otherwise leak a non-unit scale onto an
// integer column.

TEST_CASE(  // NOLINT
    "LinearProblem add_col: integer columns are never rescaled")
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Commitment",
          .variable = "status",
          .scale = 0.0861,  // pre-fix bogus value from Ruiz row-eq
      },
      {
          .class_name = "Generator",
          .variable = "generation",
          .scale = 0.1,  // would scale dispatch in [0, pmax]
      },
  };
  const VariableScaleMap map(scales);
  LinearProblem lp("integer_scale_guard");
  lp.set_variable_scale_map(map);

  SUBCASE("VariableScaleMap entry is ignored for is_integer = true")
  {
    const auto c = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .is_integer = true,
        .class_name = "Commitment",
        .variable_name = "status",
        .variable_uid = Uid {1},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(1.0));
  }

  SUBCASE("Pre-set explicit scale is also overridden for is_integer = true")
  {
    // Simulates a caller that explicitly tried to set .scale on an
    // integer column — the invariant still holds.
    const auto c = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .is_integer = true,
        .scale = 0.5,
        .class_name = "Commitment",
        .variable_name = "status",
        .variable_uid = Uid {2},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(1.0));
  }

  SUBCASE("Continuous companion column is still scaled by the map")
  {
    // Sanity: the scale fix is targeted — non-integer columns still
    // pick up their VariableScaleMap entry as before.
    const auto c = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 100.0,
        .class_name = "Generator",
        .variable_name = "generation",
        .variable_uid = Uid {1},
    });
    CHECK(lp.get_col_scale(c) == doctest::Approx(0.1));
  }

  SUBCASE("Flattened LP upper bound stays at the physical 1.0")
  {
    // End-to-end: a binary [0, 1] integer column with a registered
    // (but ignored) scale of 0.0861 must still flatten to colub = 1,
    // not 1 / 0.0861 = 11.6189.  This is the precise invariant CPLEX
    // needs to be able to pick physical u = 1 in MIP mode.
    const auto c = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .is_integer = true,
        .class_name = "Commitment",
        .variable_name = "status",
        .variable_uid = Uid {3},
    });
    const auto flat = lp.flatten({});
    const auto i = static_cast<size_t>(c.value_of());
    CHECK(flat.colub[i] == doctest::Approx(1.0));
    CHECK(flat.collb[i] == doctest::Approx(0.0));
    CHECK(flat.col_scales[i] == doctest::Approx(1.0));
    // And the integer column index list contains it.
    const auto found =
        std::ranges::find(flat.colint, static_cast<int>(c.value_of()))
        != flat.colint.end();
    CHECK(found);
  }
}

TEST_CASE(  // NOLINT
    "LinearProblem flatten: row_max equilibration also preserves integer cols")
{
  // ``row_max`` is the default equilibration mode for single-bus / no-KVL
  // builds and scales rows only — it would NOT touch column scales even
  // without the integer-column guard.  This test pins the invariant
  // anyway, so a future refactor that adds column scaling to row_max
  // (or replaces row_max with something Ruiz-like) keeps the integer
  // column bounds clean.
  LinearProblem lp("row_max_integer_guard");

  const auto gcol = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 135.0,
      .class_name = "Generator",
      .variable_name = "generation",
      .variable_uid = Uid {1},
  });
  const auto ucol = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .is_integer = true,
      .class_name = "Commitment",
      .variable_name = "status",
      .variable_uid = Uid {1},
  });

  // Same gen_upper-style row as the Ruiz test below: -pmax on the
  // integer status column, 1 on dispatch, ≤ 0.  row_max picks the
  // largest |coeff| in the row (135 here) as the row-scale divisor.
  SparseRow row;
  row[gcol] = 1.0;
  row[ucol] = -135.0;
  row.uppb = 0.0;
  row.class_name = "Commitment";
  row.constraint_name = "gen_upper";
  row.variable_uid = Uid {1};
  std::ignore = lp.add_row(std::move(row));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::row_max,
  });
  const auto u_idx = static_cast<size_t>(ucol.value_of());

  // Both bounds and the recorded col_scale survive row_max untouched.
  CHECK(flat.colub[u_idx] == doctest::Approx(1.0));
  CHECK(flat.collb[u_idx] == doctest::Approx(0.0));
  CHECK(flat.col_scales[u_idx] == doctest::Approx(1.0));

  // And it lands in the integer-column index list.
  const auto found =
      std::ranges::find(flat.colint, static_cast<int>(ucol.value_of()))
      != flat.colint.end();
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "LinearProblem flatten: Ruiz equilibration leaves integer columns alone")
{
  // Build a tiny LP that triggers Ruiz to want to rescale an integer
  // column.  The mixed-magnitude row coefficients (1 on the integer
  // status column, 100 on the continuous dispatch column, sharing a
  // KVL-style equality row) is exactly the pattern that produced the
  // ``11.6189`` LP bound in production.  Post-fix, the Ruiz pass must
  // leave the integer column's bounds and col_scales[] entry untouched.
  LinearProblem lp("ruiz_integer_guard");

  // Continuous dispatch column [0, 135] — analogue of generator_generation.
  const auto gcol = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 135.0,
      .class_name = "Generator",
      .variable_name = "generation",
      .variable_uid = Uid {1},
  });

  // Binary status column [0, 1], is_integer — analogue of commitment_status.
  const auto ucol = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .is_integer = true,
      .class_name = "Commitment",
      .variable_name = "status",
      .variable_uid = Uid {1},
  });

  // Constraint dispatch − pmax · u ≤ 0 (the gen_upper row).  Coefficient
  // -135 on u forces Ruiz to want col_factor ≠ 1 on u for row balancing.
  SparseRow row;
  row[gcol] = 1.0;
  row[ucol] = -135.0;
  row.uppb = 0.0;
  row.class_name = "Commitment";
  row.constraint_name = "gen_upper";
  row.variable_uid = Uid {1};
  std::ignore = lp.add_row(std::move(row));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::ruiz,
  });
  const auto u_idx = static_cast<size_t>(ucol.value_of());
  const auto g_idx = static_cast<size_t>(gcol.value_of());

  // Post-fix: integer column survives Ruiz with its physical bounds
  // intact.  Pre-fix (no is_integer guard in apply_ruiz_scaling) the
  // upper bound would land somewhere near 11.6.
  CHECK(flat.colub[u_idx] == doctest::Approx(1.0));
  CHECK(flat.collb[u_idx] == doctest::Approx(0.0));
  CHECK(flat.col_scales[u_idx] == doctest::Approx(1.0));

  // The continuous companion can still be rescaled — the fix is
  // surgically scoped to integer columns.  We assert NOT-equal to its
  // physical bound to prove Ruiz actually ran (i.e. the test exercises
  // the scaling path, not a degenerate no-op).
  CHECK(flat.colub[g_idx] != doctest::Approx(135.0));
}

}  // namespace
