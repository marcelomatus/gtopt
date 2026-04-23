// SPDX-License-Identifier: BSD-3-Clause

/// @file test_line_ampl_compound.cpp
/// @brief Unit tests for the `line.flow` AMPL compound across loss
///        modes and for missing-variable behaviour (direct refs to
///        `line.flown` in lossless mode and `bus.theta` in single-bus
///        mode).
///
/// Two distinct resolver code paths are exercised:
///
/// 1. **Compound** — `line.flow = +flowp − flown` is a class-level
///    recipe registered once by `SystemLP`.  For lossy modes both
///    legs resolve to non-negative LP columns; for the `none` mode
///    only `flowp` exists (signed, in `[-tmax_ba, +tmax_ab]`) and
///    `flown` is absent.  `resolve_col_to_row` silently skips missing
///    legs so the compound still evaluates to `flow = flowp`.
///
/// 2. **Bare attribute** — a direct reference like `line.flown` or
///    `bus.theta` is NOT a compound.  When the attribute has no LP
///    column and the default strict `constraint_mode` is active,
///    `user_constraint_lp.cpp:327` throws a `std::runtime_error`.
///    This guard catches author errors (typos, inactive elements,
///    single-bus mode accidentally used with DC-OPF refs) that would
///    otherwise silently drop the term and leave the constraint
///    vacuously satisfied.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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
    opts.use_single_bus = use_single_bus;
    opts.use_kirchhoff = use_kirchhoff;
    opts.demand_fail_cost = 1000.0;
    opts.scale_objective = 1.0;

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
    // Compound expands to +1·flowp − 1·flown; the flown leg silently
    // drops because no `flown` column exists, so the row contains a
    // single +1 entry on flowp.
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
  // A user constraint that references `line('l1_2').flown` directly
  // (not via the compound) in lossless mode.  No `flown` column
  // exists, so the default strict `constraint_mode` rejects the
  // reference at build time — this is the intentional guard against
  // silent-skip failure modes (user_constraint_lp.cpp:327).
  //
  // The compound `line.flow` path (tested above) is the one that
  // silently skips missing legs; a bare `flown` reference is not a
  // compound and must error.
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
  const LineAmplFixture fix(/*use_single_bus=*/false,
                            /*use_kirchhoff=*/true,
                            /*line_losses_mode=*/std::string {"none"},
                            std::move(ucs));

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
    "AMPL independence — `bus('nope').theta` throws under Kirchhoff multi-bus")
{
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_bad_bus_id",
          .expression = "bus('b_nope').theta <= 1.0",
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
