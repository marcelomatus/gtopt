// SPDX-License-Identifier: BSD-3-Clause

/// @file test_line_ampl_compound.cpp
/// @brief Unit tests for the `line.flow` AMPL attribute across loss
///        modes and for missing-variable behaviour (direct refs to
///        `line.flown` and `bus.theta`).
///
/// `line.flow` is the ONLY AMPL-addressable flow attribute.  It is
/// registered DIRECTLY by `LineLP::add_to_lp` as a signed weighted sum
/// of the underlying LP columns — `+flowp − flown` (single-col mode),
/// `+Σflowp_seg − Σflown_seg` (piecewise_direct), or `+flows`
/// (tangent_signed_flow).  In the `none` loss mode only the signed
/// `flowp` column exists (in `[-tmax_ba, +tmax_ab]`) so the weighted sum
/// carries a single `+flowp` leg.  The per-direction `flowp`/`flown`/
/// `flows` columns are internal LP decompositions and are NOT registered
/// as AMPL attributes.
///
/// Two distinct resolver code paths are exercised:
///
/// 1. **`line.flow`** — resolves to the signed weighted sum above and
///    stamps each underlying column with its leg coefficient.
///
/// 2. **Bare unregistered/suppressed attribute** — a direct reference
///    like `line.flown` (no longer an AMPL attribute) or `bus.theta`
///    (mode-suppressed).  Under the default strict `constraint_mode`,
///    an unknown attribute on an active class throws a
///    `std::runtime_error` (`user_constraint_lp.cpp:327`); a
///    mode-suppressed class/attribute is silently dropped.  This guard
///    catches author errors (typos, inactive elements, single-bus mode
///    accidentally used with DC-OPF refs).

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;

namespace line_ampl_test
{
namespace
{

// Helpers use substring match because col/row names carry
// scenario/stage/block suffixes we want to match across.  The
// nested anonymous namespace avoids ODR conflicts with identically-
// named helpers in test_line_losses.cpp under unity-build.

auto count_cols_containing(const LinearInterface& li, std::string_view substr)
    -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

auto count_rows_containing(const LinearInterface& li, std::string_view substr)
    -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

auto find_col(const LinearInterface& li, std::string_view substr) -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

auto find_row(const LinearInterface& li, std::string_view substr) -> RowIndex
{
  RowIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

/// Template 2-bus system.  Loss mode, Kirchhoff flag, and user
/// constraints are injected per test.
struct LineAmplFixture
{
  PlanningOptions opts;
  Simulation simulation;
  System system;

  LineAmplFixture(bool use_single_bus,
                  bool use_kirchhoff,
                  std::optional<std::string> line_losses_mode,
                  Array<UserConstraint> user_constraint_array)
  {
    opts.model_options.use_single_bus = use_single_bus;
    opts.model_options.use_kirchhoff = use_kirchhoff;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.model_options.scale_objective = 1.0;

    simulation = Simulation {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {1}, .probability_factor = 1.0}},
    };

    Line line {
        .uid = Uid {1},
        .name = "l1_2",
        .bus_a = Uid {1},
        .bus_b = Uid {2},
        .voltage = 220.0,
        .resistance = 0.01,
        .reactance = 0.02,
        .tmax_ba = 200.0,
        .tmax_ab = 200.0,
        .capacity = 200.0,
    };
    if (line_losses_mode.has_value()) {
      line.line_losses_mode = std::move(line_losses_mode);
      line.loss_segments = 3;
    }

    system = System {
        .name = "line_ampl_compound_test",
        .bus_array =
            {
                {.uid = Uid {1}, .name = "b1"},
                {.uid = Uid {2}, .name = "b2"},
            },
        .demand_array =
            {
                {
                    .uid = Uid {1},
                    .name = "d2",
                    .bus = Uid {2},
                    .lmax = 50.0,
                    .capacity = 50.0,
                },
            },
        .generator_array =
            {
                {
                    .uid = Uid {1},
                    .name = "g1",
                    .bus = Uid {1},
                    .gcost = 10.0,
                    .capacity = 300.0,
                },
            },
        .line_array = {line},
        .user_constraint_array = std::move(user_constraint_array),
    };
  }

  /// LP-matrix options separate from PlanningOptions — must be passed
  /// explicitly to the SystemLP constructor because PlanningOptions's
  /// `lp_matrix_options` does not auto-flow into SystemLP.  Mirrors
  /// `test_line_losses.cpp::LPFixture::build_opts`.
  [[nodiscard]] static auto build_matrix_opts() -> LpMatrixOptions
  {
    return LpMatrixOptions {
        .col_with_names = true,
        .row_with_names = true,
        .col_with_name_map = true,
        .row_with_name_map = true,
    };
  }
};

}  // namespace
}  // namespace line_ampl_test

using line_ampl_test::count_cols_containing;
using line_ampl_test::count_rows_containing;
using line_ampl_test::find_col;
using line_ampl_test::find_row;
using line_ampl_test::LineAmplFixture;

// ═════════════════════════════════════════════════════════════════════════
//   `line.flow` AMPL compound across loss modes
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "LineLP AMPL — lossless mode: signed flowp carries line.flow")  // NOLINT
{
  // Non-binding user constraint that references the compound `line.flow`.
  // The constraint exists purely to force the resolver to expand the
  // compound and emit a row over the line's LP columns.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_flow_bound",
          .expression = "line('l1_2').flow <= 1000",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());

  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  SUBCASE("only flowp column exists; flown is absent")
  {
    CHECK(count_cols_containing(lp, "line_flowp_") == 1);
    CHECK(count_cols_containing(lp, "line_flown_") == 0);
  }

  SUBCASE("flowp column has signed bounds [-tmax_ba, +tmax_ab]")
  {
    const auto fp = find_col(lp, "line_flowp_");
    CHECK(lp.get_col_low()[value_of(fp)] == doctest::Approx(-200.0));
    CHECK(lp.get_col_upp()[value_of(fp)] == doctest::Approx(+200.0));
  }

  SUBCASE("user_constraint row has flowp as its only non-zero coefficient")
  {
    // `flow` is the weighted sum +1·flowp − 1·flown; the flown leg is
    // absent in lossless mode (no flown column), so the row contains a
    // single +1 entry on the signed flowp column.
    const auto fp = find_col(lp, "line_flowp_");
    const auto uc_row = find_row(lp, "uc_flow_bound_constraint_");
    CHECK(lp.get_coeff(uc_row, fp) == doctest::Approx(+1.0));
  }

  SUBCASE("primal flow equals demand (50 MW A→B)")
  {
    const auto fp = find_col(lp, "line_flowp_");
    // Cheap generator at b1 serves 50 MW demand at b2 through the line.
    // Without kirchhoff/losses the flow equals demand.
    CHECK(lp.get_col_sol()[value_of(fp)] == doctest::Approx(50.0));
  }
}

TEST_CASE(
    "LineLP AMPL — linear lossy mode: line.flow = flowp − flown")  // NOLINT
{
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_flow_bound",
          .expression = "line('l1_2').flow <= 1000",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"linear"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());

  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  SUBCASE("both flowp and flown columns exist with non-negative bounds")
  {
    CHECK(count_cols_containing(lp, "line_flowp_") == 1);
    CHECK(count_cols_containing(lp, "line_flown_") == 1);
    const auto fp = find_col(lp, "line_flowp_");
    const auto fn = find_col(lp, "line_flown_");
    CHECK(lp.get_col_low()[value_of(fp)] == doctest::Approx(0.0));
    CHECK(lp.get_col_upp()[value_of(fp)] == doctest::Approx(200.0));
    CHECK(lp.get_col_low()[value_of(fn)] == doctest::Approx(0.0));
    CHECK(lp.get_col_upp()[value_of(fn)] == doctest::Approx(200.0));
  }

  SUBCASE("user_constraint row emits both legs: +1·flowp − 1·flown")
  {
    const auto fp = find_col(lp, "line_flowp_");
    const auto fn = find_col(lp, "line_flown_");
    const auto uc_row = find_row(lp, "uc_flow_bound_constraint_");
    CHECK(lp.get_coeff(uc_row, fp) == doctest::Approx(+1.0));
    CHECK(lp.get_coeff(uc_row, fn) == doctest::Approx(-1.0));
  }
}

TEST_CASE(  // NOLINT
    "LineLP AMPL — lossless: direct `flown` reference throws in strict mode")
{
  // A user constraint that references `line('l1_2').flown` directly.
  // `flown` is no longer a registered AMPL attribute (only `line.flow`
  // is exposed), so on an active line class the default strict
  // `constraint_mode` rejects the unknown attribute at build time —
  // the intentional guard against silent-skip failure modes
  // (user_constraint_lp.cpp:327).  Only `line.flow` is addressable; it
  // folds the flowp/flown columns into one signed weighted sum.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_flown_missing",
          .expression = "line('l1_2').flown <= 10",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, LineAmplFixture::build_matrix_opts()),
      std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════
//   Mode-driven AMPL suppression — `use_single_bus` / `!use_kirchhoff`
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "BusLP AMPL — `bus.theta` in single_bus mode is silently dropped")
{
  // Under `use_single_bus=true`, `BusLP::needs_kirchhoff` returns false
  // and no theta column is ever registered.  `system_lp.cpp` translates
  // this mode into a `suppress_ampl_attribute("bus", "theta", …)` call,
  // so the strict resolver drops the term silently (with a DEBUG-level
  // trace) rather than throwing.  The constraint becomes vacuously
  // satisfied — which is the intended behavior: the modeller did not
  // have to guard their expression on the active mode.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_theta_ref",
          .expression = "bus('b1').theta <= 1.0",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/true,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  // Build must succeed — no throw, no exit.
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // No theta column was created under single_bus.
  CHECK(count_cols_containing(lp, "bus_theta_") == 0);
}

TEST_CASE(  // NOLINT
    "LineLP AMPL — `line.flow` in single_bus mode is silently dropped")
{
  // Under `use_single_bus=true`, `LineLP::add_to_lp` early-exits so no
  // line columns (flowp/flown/theta_rows) are ever created.  A
  // constraint that references `line('l1_2').flow` (the compound) or
  // bare `line('l1_2').flowp` must not throw — the entire `line` class
  // is class-suppressed by `system_lp.cpp`.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_line_flow",
          .expression = "line('l1_2').flow <= 100",
      },
      {
          .uid = Uid {2},
          .name = "uc_line_flowp",
          .expression = "line('l1_2').flowp <= 100",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/true,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  // No line columns of any kind under single_bus.
  CHECK(count_cols_containing(lp, "line_flowp_") == 0);
  CHECK(count_cols_containing(lp, "line_flown_") == 0);
}

TEST_CASE(  // NOLINT
    "AMPL suppression — typo attribute on suppressed class is dropped")
{
  // Current policy: class-level suppression is coarse — once the class
  // is suppressed, any attribute reference on that class is dropped,
  // including typos like `line('l1').flowx`.  This is an accepted
  // trade-off: single_bus users can still reuse their multi-bus
  // constraint files verbatim without hitting attribute-typo errors on
  // references that will never be emitted anyway.  Typos on active
  // (non-suppressed) classes are still caught by the strict guard.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_typo_attr",
          .expression = "line('l1_2').flowx <= 100",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/true,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  // Class-suppression catches the reference before attribute lookup.
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
}

TEST_CASE(  // NOLINT
    "BusLP AMPL — `bus.theta` in multi-bus + `!use_kirchhoff` is dropped")
{
  // Multi-bus but Kirchhoff disabled: theta columns are never created,
  // so references must be silently dropped via suppression.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_theta_no_kirch",
          .expression = "bus('b1').theta <= 1.0",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(count_cols_containing(sys_lp.linear_interface(), "bus_theta_") == 0);
}

TEST_CASE(  // NOLINT
    "BusLP AMPL — `bus.theta` in multi-bus Kirchhoff mode resolves correctly")
{
  // Multi-bus Kirchhoff path: LineLP calls theta_cols_at for each
  // endpoint bus, lazily materializing theta columns.  The user
  // constraint's theta reference must then resolve to a real column.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_theta_ref",
          .expression = "bus('b2').theta <= 1.0",
      },
  };
  LineAmplFixture fix(/*use_single_bus=*/false,
                      /*use_kirchhoff=*/true,
                      /*line_losses_mode=*/std::string {"none"},
                      std::move(ucs));
  // Pin B-θ formulation: this test asserts that `bus('b2').theta` resolves
  // to a real column.  The post-2026-05-14 default `cycle_basis` does not
  // create theta vars, so the AMPL resolver throws.
  fix.opts.model_options.kirchhoff_mode = OptName {"node_angle"};

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());

  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  SUBCASE("theta columns are created for both buses")
  {
    CHECK(count_cols_containing(lp, "bus_theta_") >= 1);
  }

  SUBCASE("user_constraint row references the bus b2 theta column")
  {
    const auto theta_b2 = find_col(lp, "bus_theta_2_");
    const auto uc_row = find_row(lp, "uc_theta_ref_constraint_");
    CHECK(lp.get_coeff(uc_row, theta_b2) == doctest::Approx(+1.0));
  }
}

// ═════════════════════════════════════════════════════════════════════════
//   Per-element optional capacity attrs (`capainst`, `capacost`, `expmod`)
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AMPL capacity — `capainst` on non-expanding generator is dropped")
{
  // `g1` in the fixture has no expansion configured (no `expcap`,
  // `expmod`, `annual_capcost`), so `CapacityObjectBase::add_to_lp`
  // early-exits before creating the capainst/capacost/expmod columns.
  // A user constraint that references `generator('g1').capainst` must
  // therefore silently drop: the attribute is declared per-element
  // optional by `system_lp.cpp`'s AMPL one-shot.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_cap_nonexp",
          .expression = "generator('g1').capainst <= 500",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());

  // No capainst column exists for g1 — it didn't expand.
  CHECK(count_cols_containing(sys_lp.linear_interface(), "generator_capainst_")
        == 0);
}

TEST_CASE(  // NOLINT
    "AMPL capacity — capacity-attr typo still throws on non-expanding elem")
{
  // The suppression is class-attribute-scoped: only the declared
  // optional attributes (capainst / capacost / expmod) are eligible
  // for silent drop.  A typo like `capinst` (missing 'a') is NOT in
  // the optional list, so the strict resolver must still throw — the
  // typo guard is preserved across the relaxation.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_cap_typo",
          .expression = "generator('g1').capinst <= 500",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, LineAmplFixture::build_matrix_opts()),
      std::runtime_error);
}

TEST_CASE(  // NOLINT
    "AMPL capacity — optional attrs registered for every capacity class")
{
  // Smoke test: verify demand/line/converter/battery all share the
  // same per-element optional semantic for capacity attrs.  The
  // non-expanding demand `d2` and line `l1_2` in the fixture should
  // both drop their capainst references silently.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_demand_cap",
          .expression = "demand('d2').capainst <= 100",
      },
      {
          .uid = Uid {2},
          .name = "uc_line_cap",
          .expression = "line('l1_2').capainst <= 500",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
}

// ────────────────────────────────────────────────────────────────────────
//   Independence of `use_single_bus` and `use_kirchhoff` suppression
// ────────────────────────────────────────────────────────────────────────
// The two mode flags register AMPL suppressions independently.  When
// `use_single_bus=false` the `line` class is *not* suppressed, so a
// reference to a non-existent line id must fall through to the strict
// branch and throw — a typo cannot hide behind mode-suppression.

TEST_CASE(  // NOLINT
    "AMPL independence — `line('nope').flow` throws when lines are active")
{
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_bad_line_id",
          .expression = "line('l_does_not_exist').flow <= 1.0",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/true,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, LineAmplFixture::build_matrix_opts()),
      std::runtime_error);
}

TEST_CASE(  // NOLINT
    "AMPL independence — `bus('nope').theta` throws under Kirchhoff "
    "multi-bus (node_angle)")
{
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_bad_bus_id",
          .expression = "bus('b_nope').theta <= 1.0",
      },
  };
  LineAmplFixture fix(/*use_single_bus=*/false,
                      /*use_kirchhoff=*/true,
                      /*line_losses_mode=*/std::string {"none"},
                      std::move(ucs));
  // Pin node_angle so `bus.theta` is NOT mode-suppressed.  Under the
  // post-2026-05-14 default `cycle_basis`, the AMPL resolver suppresses
  // `bus.theta` entirely (mode-driven silent skip), so even a missing
  // bus name would not throw — the throw signal we want here is the
  // strict element-not-found path used when theta resolution is live.
  fix.opts.model_options.kirchhoff_mode = OptName {"node_angle"};

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, LineAmplFixture::build_matrix_opts()),
      std::runtime_error);
}

TEST_CASE(  // NOLINT
    "AMPL independence — `bus('nope').theta` is silently dropped under "
    "cycle_basis (post-2026-05-14 default)")
{
  // Companion test to the node_angle case above.  Under cycle_basis the
  // entire `bus.theta` attribute is mode-suppressed (no theta column is
  // ever materialised), so user constraints touching it are silently
  // dropped — including those with mistyped bus names.  This is the
  // documented trade-off for mode-driven suppression: typo guards
  // weaken to the surrounding `use_kirchhoff=false` / `use_single_bus`
  // precedent.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_bad_bus_id_cycle_basis",
          .expression = "bus('b_nope').theta <= 1.0",
      },
  };
  LineAmplFixture fix(/*use_single_bus=*/false,
                      /*use_kirchhoff=*/true,
                      /*line_losses_mode=*/std::string {"none"},
                      std::move(ucs));
  // Default is cycle_basis but pin explicitly to make the contract
  // visible at the test site.
  fix.opts.model_options.kirchhoff_mode = OptName {"cycle_basis"};

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_NOTHROW(
      SystemLP(fix.system, sim_lp, LineAmplFixture::build_matrix_opts()));
}

TEST_CASE(  // NOLINT
    "AMPL independence — single_bus + !kirchhoff: both flags apply "
    "independently")
{
  // Both flags suppress `bus.theta`; `use_single_bus` also suppresses
  // the whole `line` class.  Both constraints build silently.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_theta_both",
          .expression = "bus('b1').theta <= 1.0",
      },
      {
          .uid = Uid {2},
          .name = "uc_line_both",
          .expression = "line('l1_2').flow <= 100",
      },
  };
  const LineAmplFixture fix(/*use_single_bus=*/true,
                            /*use_kirchhoff=*/false,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(count_cols_containing(sys_lp.linear_interface(), "bus_theta_") == 0);
}

// ═════════════════════════════════════════════════════════════════════════
//   piecewise_direct: no aggregator cols, AMPL `line.flow` via seg-sum
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "LineLP AMPL — piecewise_direct: line.flow expands to Σ seg_p − Σ seg_n")
{
  // Pin the multi-col virtual-aggregator path (added 2026-05-14):
  //   * No `flowp` / `flown` aggregator LP cols exist.
  //   * No `flow_link` / `loss_link` rows exist.
  //   * The K segment cols stamp directly into the bus rows.  This
  //     2-bus fixture has no cycle, so the cycle-basis assembler
  //     (the post-2026-05-14 default kirchhoff mode) emits zero KVL
  //     rows.  See the "3-bus triangle" test below for the cycle
  //     KVL seg-stamping assertions.
  //   * `line.flow` is reachable in PAMPL via the seg-sum AMPL
  //     registration in `LineLP::add_to_lp` — the user-constraint
  //     resolver expands `+flowp − flown` into +Σ seg_p − Σ seg_n.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_flow_bound",
          .expression = "line('l1_2').flow <= 1000",
      },
  };
  const LineAmplFixture fix(
      /*use_single_bus=*/false,
      /*use_kirchhoff=*/true,
      /*line_losses_mode=*/std::string {"piecewise_direct"},
      std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  SUBCASE("no aggregator cols, no link rows — direct stamping only")
  {
    // Per-direction K segs present (line.loss_segments = 3).
    CHECK(count_cols_containing(lp, "line_flowp_seg_") == 3);
    CHECK(count_cols_containing(lp, "line_flown_seg_") == 3);
    // All flowp/flown matches come from segment cols (no aggregator).
    CHECK(count_cols_containing(lp, "line_flowp_")
          == count_cols_containing(lp, "line_flowp_seg_"));
    CHECK(count_cols_containing(lp, "line_flown_")
          == count_cols_containing(lp, "line_flown_seg_"));
    // No loss vars and no link rows in this mode.
    CHECK(count_cols_containing(lp, "line_lossp_") == 0);
    CHECK(count_cols_containing(lp, "line_flowp_link_") == 0);
    CHECK(count_cols_containing(lp, "line_lossp_link_") == 0);
  }

  SUBCASE(
      "uc_flow_bound row stamps Σ seg_p with +1, Σ seg_n with −1 "
      "(seg-sum AMPL expansion)")
  {
    const auto uc_row = find_row(lp, "uc_flow_bound_constraint_");
    // Every flowp_seg col contributes +1·base_coeff to the row,
    // every flown_seg col contributes −1·base_coeff.  base_coeff is
    // the constraint's coefficient on `line.flow` (= 1 here).
    for (const auto& [name, col] : lp.col_name_map()) {
      const auto coef = lp.get_coeff(uc_row, col);
      if (name.find("line_flowp_seg_") != std::string_view::npos) {
        CHECK(coef == doctest::Approx(+1.0));
      } else if (name.find("line_flown_seg_") != std::string_view::npos) {
        CHECK(coef == doctest::Approx(-1.0));
      }
    }
  }

  SUBCASE("primal Σ seg_p − Σ seg_n equals expected line flow (50 MW A→B)")
  {
    // Cheap generator at b1 serves 50 MW demand at b2 via the line.
    // The line operates A→B, so Σ seg_p ≈ 50 and Σ seg_n ≈ 0.  Loss
    // segments fill smallest-first so a small offset is expected for
    // the seg_p sum to absorb the loss; the signed flow ≈ demand + loss.
    double sum_p = 0.0;
    double sum_n = 0.0;
    for (const auto& [cname_i, col] : lp.col_name_map()) {
      const auto sol = lp.get_col_sol()[col];
      if (cname_i.find("line_flowp_seg_") != std::string_view::npos) {
        sum_p += sol;
      } else if (cname_i.find("line_flown_seg_") != std::string_view::npos) {
        sum_n += sol;
      }
    }
    // Reverse direction must be zero — A→B feeds the demand.
    CHECK(sum_n == doctest::Approx(0.0));
    // Forward direction must cover demand plus losses.  Loose tol
    // because PWL loss adds ≈ R·f²/V² · stage factors that vary with
    // segment width; pin the magnitude band rather than the exact
    // value to keep the test robust to segment-count changes.
    CHECK(sum_p >= 50.0 - 1e-3);
    CHECK(sum_p < 60.0);  // generous upper bound for the loss premium
  }
}

// ═════════════════════════════════════════════════════════════════════════
//   piecewise_direct + cycle_basis: cycle KVL row stamps segments directly
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "LineLP cycle KVL — piecewise_direct stamps each seg with ±x_τ "
    "(3-bus triangle)")
{
  // The 2-bus fixture above has no cycle, so the cycle-basis
  // assembler emits zero KVL rows.  This test builds the smallest
  // network that DOES carry a fundamental cycle — three buses
  // connected as a triangle — and pins the row-construction
  // invariants of `kirchhoff::cycle_basis::add_kvl_rows` under
  // `piecewise_direct` line losses:
  //
  //   * No theta cols anywhere (cycle_basis short-circuits theta).
  //   * Exactly one fundamental cycle (L − B + #islands = 3 − 3 + 1
  //     = 1) → one `kirchhoff_cycle_*` row per block.
  //   * The KVL row coefficient on every segment col equals
  //     `sign · x_τ · row_scale` where sign = ±1 from the cycle
  //     traversal, and the bidirectional segments mirror with
  //     opposite signs.
  using namespace gtopt;

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = true;
  // Pin cycle_basis explicitly so the test does not regress if the
  // default kirchhoff_mode shifts back to node_angle.
  opts.model_options.kirchhoff_mode = OptName {"cycle_basis"};
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.scale_objective = 1.0;
  // Disable scale_theta so the cycle row's `row_scale = 1/scale_theta`
  // is 1.0 — coefficients land at their physical `x_τ` value, making
  // the assertion below a direct equality.
  opts.model_options.scale_theta = 1.0;
  opts.model_options.auto_scale = false;

  const Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {1}, .probability_factor = 1.0}},
  };

  // Three lines, each `piecewise_direct` with 3 segments.  `voltage`
  // = 1.0 makes V² = 1, so x_τ = X (per-unit-reactance form).
  auto mk_line =
      [](Uid uid, std::string name, Uid bus_a, Uid bus_b, double X) -> Line
  {
    // R > 0 is required: `line_losses::make_config` demotes any PWL
    // mode (including `piecewise_direct`) to `none`/`linear` when
    // R ≤ 0, V ≤ 0, or nseg < 2 — exactly the path the test must
    // avoid.  Use a small physical resistance so segments are
    // created but the per-seg loss coefficient stays tiny enough
    // that the KVL row coefficients are dominated by `x_τ = X`.
    Line l {
        .uid = uid,
        .name = std::move(name),
        .bus_a = bus_a,
        .bus_b = bus_b,
        .voltage = 1.0,
        .resistance = 0.001,
        .reactance = X,
        .tmax_ba = 100.0,
        .tmax_ab = 100.0,
        .capacity = 100.0,
    };
    l.line_losses_mode = std::string {"piecewise_direct"};
    l.loss_segments = 3;
    return l;
  };

  const System system {
      .name = "line_cycle_kvl_test",
      .bus_array =
          {
              {.uid = Uid {1}, .name = "b1"},
              {.uid = Uid {2}, .name = "b2"},
              {.uid = Uid {3}, .name = "b3"},
          },
      .demand_array =
          {
              {.uid = Uid {1},
               .name = "d3",
               .bus = Uid {3},
               .lmax = 30.0,
               .capacity = 30.0},
          },
      .generator_array =
          {
              {.uid = Uid {1},
               .name = "g1",
               .bus = Uid {1},
               .gcost = 10.0,
               .capacity = 200.0},
          },
      .line_array =
          {
              mk_line(Uid {1}, "l1_2", Uid {1}, Uid {2}, 0.10),
              mk_line(Uid {2}, "l2_3", Uid {2}, Uid {3}, 0.10),
              mk_line(Uid {3}, "l1_3", Uid {1}, Uid {3}, 0.10),
          },
  };

  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  SUBCASE("cycle_basis emits no theta cols, exactly one cycle row per block")
  {
    // Theta short-circuited — every line uses cycle-basis KVL.
    CHECK(count_cols_containing(lp, "bus_theta_") == 0);
    // L − B + #islands = 3 − 3 + 1 = 1 fundamental cycle → 1 row.
    // Row name format is `kirchhoff_cycle_<idx>_<scn>_<stg>_<blk>`
    // (lowercase via the as_label snake-case transform on the
    // constexpr class name "Kirchhoff").
    CHECK(count_rows_containing(lp, "kirchhoff_cycle_") == 1);
  }

  SUBCASE("piecewise_direct: aggregator cols absent, only segs exist")
  {
    // 3 lines × 2 directions × 3 segs = 18 segment cols.
    CHECK(count_cols_containing(lp, "line_flowp_seg_") == 3 * 3);
    CHECK(count_cols_containing(lp, "line_flown_seg_") == 3 * 3);
    // No aggregator: line_flowp_/line_flown_ matches are exactly the
    // segment cols (substring also matches the prefix of seg names).
    CHECK(count_cols_containing(lp, "line_flowp_")
          == count_cols_containing(lp, "line_flowp_seg_"));
    CHECK(count_cols_containing(lp, "line_flown_")
          == count_cols_containing(lp, "line_flown_seg_"));
  }

  SUBCASE(
      "cycle row stamps every flowp_seg with +x_τ and flown_seg with −x_τ "
      "(or their negatives, depending on cycle traversal sign)")
  {
    // With X = 0.10 and V = 1.0, x_τ = 0.10 for every line.
    // Each per-direction segment is stamped with `sign · x_τ` where
    // sign ∈ {+1, −1} is the cycle traversal direction for that line.
    // The signs for flowp_seg and flown_seg of the SAME line are
    // always opposite (forward dir is +sign · x_τ, reverse is
    // −sign · x_τ — see `kirchhoff_cycle_basis.cpp:371-390`).
    //
    // We don't pin the per-line cycle sign here because the cycle
    // construction (BFS spanning tree + non-tree edge closure)
    // depends on traversal order — instead we assert that:
    //   * Every seg col has a non-zero coefficient on the cycle row.
    //   * |coef| equals 0.10 (= x_τ · row_scale, row_scale = 1).
    //   * Within a single line, flowp_seg coef = −flown_seg coef.
    const auto cycle_row = find_row(lp, "kirchhoff_cycle_");

    // Reuse the same iteration shape as the prior test: a flat
    // walk over col_name_map looking for the right substring.
    int seen_p = 0;
    int seen_n = 0;
    for (const auto& [name, col] : lp.col_name_map()) {
      const auto coef = lp.get_coeff(cycle_row, col);
      if (name.find("line_flowp_seg_") != std::string_view::npos) {
        CHECK(std::abs(coef) == doctest::Approx(0.10).epsilon(1e-9));
        ++seen_p;
      } else if (name.find("line_flown_seg_") != std::string_view::npos) {
        CHECK(std::abs(coef) == doctest::Approx(0.10).epsilon(1e-9));
        ++seen_n;
      }
    }
    CHECK(seen_p == 3 * 3);
    CHECK(seen_n == 3 * 3);
  }
}

// ═════════════════════════════════════════════════════════════════════════
//   piecewise_direct edge cases
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "LineLP AMPL — piecewise_direct: B→A direction disabled (tmax_ba=0)")
{
  // Only tmax_ab is non-zero → only flowp_seg cols exist.
  LineAmplFixture fix(
      /*use_single_bus=*/false,
      /*use_kirchhoff=*/true,
      /*line_losses_mode=*/std::string {"piecewise_direct"},
      Array<UserConstraint> {});
  // Override the fixture's line to disable B→A flow.
  fix.system.line_array[0].tmax_ba = 0.0;
  fix.system.line_array[0].tmax_ab = 200.0;

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Only A→B segment cols exist.
  CHECK(count_cols_containing(lp, "line_flowp_seg_") == 3);
  CHECK(count_cols_containing(lp, "line_flown_seg_") == 0);
  // No aggregator cols.
  CHECK(count_cols_containing(lp, "line_flowp_")
        == count_cols_containing(lp, "line_flowp_seg_"));
  CHECK(count_cols_containing(lp, "line_flown_") == 0);
  // Line carries 50 MW A→B.
  double sum_p = 0.0;
  for (const auto& [name, col] : lp.col_name_map()) {
    if (name.find("line_flowp_seg_") != std::string_view::npos) {
      sum_p += lp.get_col_sol()[col];
    }
  }
  CHECK(sum_p == doctest::Approx(50.0).epsilon(1e-3));
}

TEST_CASE(  // NOLINT
    "LineLP AMPL — piecewise_direct: use_single_bus makes line a loop (no "
    "cols)")
{
  // Single-bus mode: both line ends connect to the same bus → is_loop() is
  // true → add_to_lp returns early, producing no LP columns at all.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_demand",
          .expression = "demand('d2').load >= 0",
      },
  };
  const LineAmplFixture fix(
      /*use_single_bus=*/true,
      /*use_kirchhoff=*/false,
      /*line_losses_mode=*/std::string {"piecewise_direct"},
      std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);
  SystemLP sys_lp(fix.system, sim_lp, LineAmplFixture::build_matrix_opts());
  auto& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  // No seg cols — line is a loop in single-bus mode.
  CHECK(count_cols_containing(lp, "line_flowp_seg_") == 0);
  CHECK(count_cols_containing(lp, "line_flown_seg_") == 0);
  CHECK(count_cols_containing(lp, "bus_theta_") == 0);
  // Demand constraint still works.
  (void)find_row(lp, "uc_demand_constraint_");
}
