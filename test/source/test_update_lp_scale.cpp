// SPDX-License-Identifier: BSD-3-Clause
//
// LP-semantic regression tests for the update_lp() callers that suffered
// from the inverted col_scale direction in `set_coeff` (commit ae85477a,
// fix(linear-interface): correct col_scale direction in set_coeff/get_coeff).
//
// Background
// ──────────
// Pre-fix, `LinearInterface::set_coeff(row, col, phys)` stored the raw
// LP coefficient as `phys / cs / rs` instead of `phys * cs / rs`.  With
// `Reservoir.energy` `var_scale = 10`, every update_lp() call (seepage,
// discharge_limit, production_factor) wrote a coefficient ~100× too
// small — the LP enforced a constraint two orders of magnitude weaker
// than intended, which is the root cause of the juan/gtopt_iplp SDDP
// LB-overshoot regression.
//
// Why the existing test suite missed it
// ─────────────────────────────────────
// Tests in `test_linear_interface_scale.cpp` (1-8) cover the LinearInterface
// mutators in isolation.  This file complements those by exercising the
// real callers end-to-end through `SystemLP::update_lp()`, which dispatches
// to `ReservoirSeepageLP::update_lp` (source/reservoir_seepage_lp.cpp:204,
// 207), `ReservoirDischargeLimitLP::update_lp`
// (source/reservoir_discharge_limit_lp.cpp:167-172), and
// `ReservoirProductionFactorLP::update_lp`
// (source/reservoir_production_factor_lp.cpp:135).
//
// Test strategy
// ─────────────
// The post-update LP is solved and the **physical objective value** is
// checked for invariance with respect to `Reservoir.energy` `var_scale`.
// Pre-fix, with var_scale=10 the constraint coefficient was 100× weaker,
// so the optimal solution diverged from the var_scale=1 reference by a
// large margin.  Post-fix the LP is invariant under col_scale, so the
// objective is the same regardless of var_scale.
//
// Tests 1-3 mirror the four-SUBCASE matrix used in
// test_linear_interface_scale.cpp:
//   • col_scale=1, scale_obj=1 ............... (sanity)
//   • col_scale=10, scale_obj=1 ............... (the original bug regime)
//   • col_scale=1, scale_obj=1000 ............ (scale_obj-only)
//   • col_scale=10, scale_obj=1000 + ruiz .... (juan-scale)
//
// Test 4 is a pure LinearInterface variant that mirrors
// test_linear_interface_scale.cpp Test 7 in a 2-variable form, providing
// independent coverage of the round-trip-via-LP-solve path.

#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/reservoir_discharge_limit_lp.hpp>
#include <gtopt/reservoir_production_factor_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

/// Bundle of LP knobs exercised by the 4-SUBCASE matrix.
struct ScaleConfig
{
  double energy_scale {1.0};  ///< Reservoir.energy var_scale
  double scale_objective {1.0};  ///< PlanningOptions::scale_objective
  LpEquilibrationMethod equilibration {LpEquilibrationMethod::none};
};

/// Apply a ScaleConfig to a PlanningOptions.
inline void apply_scale_config(PlanningOptions& opts, const ScaleConfig& cfg)
{
  opts.variable_scales = {
      VariableScale {
          .class_name = "Reservoir",
          .variable = "energy",
          .scale = cfg.energy_scale,
      },
  };
  opts.scale_objective = OptReal {cfg.scale_objective};
  opts.lp_matrix_options.equilibration_method = cfg.equilibration;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────
// Test 1 — ReservoirSeepageLP::update_lp under var_scale != 1
//
// Builds a 1-stage hydro+thermal system with multi-segment seepage and
// asserts that the *physical objective value* is invariant under the
// four standard scale configurations.  Exercises both the structural
// `add_to_lp` (which calls `lp.add_row(SparseRow{...})` — also fixed in
// the same commit's `add_rows` audit) and `update_lp` (which calls
// `set_coeff` and `set_rhs` at source/reservoir_seepage_lp.cpp:204,207).
//
// Pre-fix bug signature (col_scale=10): the seepage row's coefficient on
// the efin column is stored 100× too small, so `q_filt` is structurally
// disconnected from the reservoir volume — every var_scale != 1
// configuration computes a different LP objective.  Post-fix, the
// objective is the same in all four configurations.
//
// Caveat: at iter=0/phase=0 single-stage, `update_lp` for seepage uses
// the start-of-stage volume from `physical_eini`, which falls back to
// the JSON `eini`.  Because that matches the value used by `add_to_lp`,
// `update_lp` does not re-issue `set_coeff` (returns 0).  This means the
// test catches regressions in the **structural** add-row path of the
// same fix; coverage of the dynamic update-coefficient path is provided
// by Test 2 (ReservoirDischargeLimit, where the segment-selection
// volume is `(vini+vfin)/2` and `vfin` is set by the solver).
// ────────────────────────────────────────────────────────────────────
TEST_CASE("update_lp_scale - ReservoirSeepageLP under var_scale")  // NOLINT
{
  auto build_and_solve = [](const ScaleConfig& cfg) -> double
  {
    const Array<Bus> bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "hydro_gen",
            .bus = Uid {1},
            .gcost = 5.0,
            .capacity = 500.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 50.0,
        },
    };

    const Array<Junction> junction_array = {
        {
            .uid = Uid {1},
            .name = "j_up",
        },
        {
            .uid = Uid {2},
            .name = "j_down",
            .drain = true,
        },
    };

    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 500.0,
        },
    };

    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv1",
            .junction = Uid {1},
            .capacity = 10000.0,
            .emin = 0.0,
            .emax = 10000.0,
            .eini = 5000.0,
            .fmin = -1000.0,
            .fmax = 1000.0,
            .flow_conversion_rate = 1.0,
        },
    };

    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur1",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 1.0,
        },
    };

    // Multi-segment seepage so `update_lp` is exercised via the same
    // dynamic-coefficient code path as the juan bug (segment 2 selected
    // at eini=5000).  q_filt = 2.0 + 0.0002 * efin at V >= 500.
    const Array<ReservoirSeepage> reservoir_seepage_array = {
        {
            .uid = Uid {1},
            .name = "filt1",
            .waterway = Uid {1},
            .reservoir = Uid {1},
            .segments =
                {
                    {
                        .volume = 0.0,
                        .slope = 0.001,
                        .constant = 0.5,
                    },
                    {
                        .volume = 500.0,
                        .slope = 0.0002,
                        .constant = 2.0,
                    },
                },
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1.0,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "SeepageScaleTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .reservoir_seepage_array = reservoir_seepage_array,
        .turbine_array = turbine_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    apply_scale_config(opts, cfg);

    const PlanningOptionsLP options_lp(opts);
    SimulationLP simulation_lp(simulation, options_lp);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.scale_objective() == doctest::Approx(cfg.scale_objective));

    // 1. Initial structural-build solve.
    auto r0 = lp.resolve();
    REQUIRE(r0.has_value());
    REQUIRE(r0.value() == 0);

    // 2. update_lp: re-issues set_coeff/set_rhs on the seepage row when
    //    the segment-selection volume differs.  At single-stage/phase the
    //    selection volume == eini, so updated may be 0 — the test still
    //    catches the structural-build path of the same fix.
    [[maybe_unused]] const auto updated = system_lp.update_lp();

    // 3. Re-solve and read the physical objective value.
    auto r1 = lp.resolve();
    REQUIRE(r1.has_value());
    REQUIRE(r1.value() == 0);

    return lp.get_obj_value();  // physical units
  };

  SUBCASE("col_scale=1, scale_obj=1, no equilibration (sanity)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(baseline > 0.0);
  }

  SUBCASE("col_scale=10, scale_obj=1 (the original juan bug regime)")
  {
    // PRE-FIX: the seepage row's efin coefficient was 100× too small,
    // so the LP-solver behaviour diverged from the col_scale=1 baseline.
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 10.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }

  SUBCASE("col_scale=1, scale_obj=1000 (scale_obj only)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1000.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }

  SUBCASE("col_scale=10, scale_obj=1000 + ruiz (juan-scale)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 10.0,
        .scale_objective = 1000.0,
        .equilibration = LpEquilibrationMethod::ruiz,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 2 — ReservoirDischargeLimitLP::update_lp under var_scale != 1
//
// `ReservoirDischargeLimit::update_lp`
// (source/reservoir_discharge_limit_lp.cpp: 167-172) writes set_coeff on BOTH
// the eini and efin columns plus a set_rhs.  The segment-selection volume is
// `(vini+vfin)/2` where vfin comes from the previous solve, so
// single-stage/single-phase planning is sufficient to force `update_lp` to
// re-issue all three writes.
//
// Pre-fix bug signature (col_scale=10): both coefficient writes were
// 100× too small, so the volume-dependent discharge cap effectively
// vanished from the LP — the solver was free to discharge well above
// the intended limit, dropping the LP objective relative to the
// var_scale=1 baseline.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "update_lp_scale - ReservoirDischargeLimitLP under var_scale")  // NOLINT
{
  auto build_and_solve = [](const ScaleConfig& cfg) -> double
  {
    const Array<Bus> bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "hydro_gen",
            .bus = Uid {1},
            .gcost = 5.0,
            .capacity = 500.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 80.0,
        },
    };

    const Array<Junction> junction_array = {
        {
            .uid = Uid {1},
            .name = "j_up",
        },
        {
            .uid = Uid {2},
            .name = "j_down",
            .drain = true,
        },
    };

    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 500.0,
        },
    };

    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv1",
            .junction = Uid {1},
            .capacity = 10000.0,
            .emin = 0.0,
            .emax = 10000.0,
            .eini = 5000.0,
            .fmin = -1000.0,
            .fmax = 1000.0,
            .flow_conversion_rate = 1.0,
        },
    };

    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur1",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 2.0,
        },
    };

    // Two-segment discharge limit.  At average volume (eini+efin)/2,
    // segment selection lands inside seg2 (boundary 3000), forcing
    // update_lp to re-issue set_coeff/set_rhs.
    const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
        {
            .uid = Uid {1},
            .name = "ddl1",
            .waterway = Uid {1},
            .reservoir = Uid {1},
            .segments =
                {
                    {
                        .volume = 0.0,
                        .slope = 1e-4,
                        .intercept = 10.0,
                    },
                    {
                        .volume = 3000.0,
                        .slope = 2e-4,
                        .intercept = 30.0,
                    },
                },
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1.0,
                },
                {
                    .uid = Uid {2},
                    .duration = 2.0,
                },
                {
                    .uid = Uid {3},
                    .duration = 3.0,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 3,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "DDLScaleTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
        .turbine_array = turbine_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    apply_scale_config(opts, cfg);

    const PlanningOptionsLP options_lp(opts);
    SimulationLP simulation_lp(simulation, options_lp);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.scale_objective() == doctest::Approx(cfg.scale_objective));

    auto r0 = lp.resolve();
    REQUIRE(r0.has_value());
    REQUIRE(r0.value() == 0);

    // At iter=0/phase=0 single-stage: vini=eini=5000, vfin=solved
    // efin <= 5000.  When `(vini+vfin)/2` lands in a different segment
    // than `eini`, `update_lp` re-issues set_coeff(eini_col, -slope/2),
    // set_coeff(efin_col, -slope/2), set_rhs(intercept) — the exact set
    // of mutators the linear-interface fix corrected.
    [[maybe_unused]] const auto updated = system_lp.update_lp();

    auto r1 = lp.resolve();
    REQUIRE(r1.has_value());
    REQUIRE(r1.value() == 0);

    return lp.get_obj_value();
  };

  SUBCASE("col_scale=1, scale_obj=1, no equilibration (sanity)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(baseline > 0.0);
  }

  SUBCASE("col_scale=10, scale_obj=1 (the original juan bug regime)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 10.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }

  SUBCASE("col_scale=1, scale_obj=1000 (scale_obj only)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1000.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }

  SUBCASE("col_scale=10, scale_obj=1000 + ruiz (juan-scale)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 10.0,
        .scale_objective = 1000.0,
        .equilibration = LpEquilibrationMethod::ruiz,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 3 — ReservoirProductionFactorLP::update_lp under var_scale != 1
//
// `ReservoirProductionFactorLP::update_conversion_coeff`
// (source/reservoir_production_factor_lp.cpp:135) writes a single
// set_coeff on the turbine conversion row — the flow column
// coefficient `-(efficiency × prod_factor)`.  This row is owned by
// TurbineLP, NOT ReservoirLP, so the `Reservoir.energy` var_scale
// reaches it indirectly: the production-factor update is dispatched
// per-stage and the production_factor itself is volume-dependent.
//
// Even though the coefficient-bearing column here is the waterway
// flow (not the reservoir energy), this test still pins the same
// dimensional contract because the SystemLP build composes the
// scale vector across all elements.  Pre-fix, any non-unit
// col_scale on adjacent reservoir columns triggered the inverted
// `set_coeff` write; post-fix, the production-factor row is
// guaranteed correct under all four scale configurations.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "update_lp_scale - ReservoirProductionFactorLP under var_scale")  // NOLINT
{
  auto build_and_solve = [](const ScaleConfig& cfg) -> double
  {
    const Array<Bus> bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "hydro_gen",
            .bus = Uid {1},
            .gcost = 5.0,
            .capacity = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 50.0,
        },
    };

    const Array<Junction> junction_array = {
        {
            .uid = Uid {1},
            .name = "j_upstream",
        },
        {
            .uid = Uid {2},
            .name = "j_downstream",
            .drain = true,
        },
    };

    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 100.0,
        },
    };

    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv1",
            .junction = Uid {1},
            .capacity = 1000.0,
            .emin = 0.0,
            .emax = 1000.0,
            .eini = 500.0,
        },
    };

    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur1",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 1.0,
            .main_reservoir = Uid {1},
        },
    };

    // Multi-segment production factor (concave envelope).  At eini=500
    // the LP solves with one rate; update_lp evaluates at the average
    // (vini+vfin)/2 — different from eini — and writes a new rate via
    // set_coeff on the turbine conversion row.
    const Array<ReservoirProductionFactor> reservoir_production_factor_array = {
        {
            .uid = Uid {1},
            .name = "eff1",
            .turbine = Uid {1},
            .reservoir = Uid {1},
            .mean_production_factor = 1.5,
            .segments =
                {
                    {
                        .volume = 0.0,
                        .slope = 0.001,
                        .constant = 1.0,
                    },
                    {
                        .volume = 800.0,
                        .slope = 0.0001,
                        .constant = 1.5,
                    },
                },
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1.0,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "ProdFactorScaleTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .reservoir_production_factor_array = reservoir_production_factor_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    apply_scale_config(opts, cfg);

    const PlanningOptionsLP options_lp(opts);
    SimulationLP simulation_lp(simulation, options_lp);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.scale_objective() == doctest::Approx(cfg.scale_objective));

    auto r0 = lp.resolve();
    REQUIRE(r0.has_value());
    REQUIRE(r0.value() == 0);

    // Segment-selection volume is (vini+vfin)/2: differs from eini=500
    // when the LP uses any hydro, so `update_lp` writes a new
    // -(efficiency × rate) coefficient on the turbine conversion row.
    const auto updated = system_lp.update_lp();
    CHECK(updated > 0);  // confirm the set_coeff path actually runs

    auto r1 = lp.resolve();
    REQUIRE(r1.has_value());
    REQUIRE(r1.value() == 0);

    return lp.get_obj_value();
  };

  SUBCASE("col_scale=1, scale_obj=1, no equilibration (sanity)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(baseline > 0.0);
  }

  SUBCASE("col_scale=10, scale_obj=1 (the original juan bug regime)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 10.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }

  SUBCASE("col_scale=1, scale_obj=1000 (scale_obj only)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1000.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }

  SUBCASE("col_scale=10, scale_obj=1000 + ruiz (juan-scale)")
  {
    const auto baseline = build_and_solve({
        .energy_scale = 1.0,
        .scale_objective = 1.0,
        .equilibration = LpEquilibrationMethod::none,
    });
    const auto scaled = build_and_solve({
        .energy_scale = 10.0,
        .scale_objective = 1000.0,
        .equilibration = LpEquilibrationMethod::ruiz,
    });
    CHECK(scaled == doctest::Approx(baseline).epsilon(1e-6));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 4 — cross-LP set_coeff round-trip via LP solve (2-variable form)
//
// Pure LinearInterface test that mirrors test_linear_interface_scale.cpp
// Test 7 in a 2-variable form on a **2-variable** constraint.  Provides
// independent coverage of the round-trip-via-LP-solve invariant under
// col_scale=10, scale_obj=1000 — the exact regime where the juan bug
// manifested.
//
// Setup:    min x + y  s.t.  a*x + b*y >= rhs,  0 <= x,y <= 50
// Phase 1:  Build with a = 5.  Solve.  Capture (x_opt1, y_opt1).
// Phase 2:  set_coeff(row, x_col, 10).  Solve.  Capture (x_opt2, y_opt2).
//
// Pre-fix at col_scale=10: set_coeff stored 10/(10*1) = 1 in raw, but
// reading back via get_coeff returned 1*1*1 = 1 — both sides of the
// round-trip were inverted so a self-consistent test PASSED while the
// SOLVER enforced the wrong constraint (1*x_LP instead of 10*x_LP).
// The LP-solver test below detects exactly that: with a=10 the optimum
// must shift relative to a=5 since x is cheaper-per-unit-coverage.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("update_lp_scale - set_coeff via LP solve (2-variable)")  // NOLINT
{
  // min x + y  s.t.  5x + 3y >= 100,  0 <= x,y <= 50
  // Optimal: x=20, y=0 (x is cheaper per unit of constraint slack: 5/1 vs 3/1).
  // After updating x's coefficient to 10:
  //   min x + y  s.t. 10x + 3y >= 100  → optimal x=10, y=0, obj=10.
  constexpr double kColScale = 10.0;
  constexpr double kScaleObj = 1000.0;

  LinearProblem lp("setcoeff_solve");
  const auto x_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 50.0,
      .cost = 1.0,
      .scale = kColScale,
  });
  const auto y_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 50.0,
      .cost = 1.0,
  });

  auto r0 = SparseRow {};
  r0[x_col] = 5.0;  // physical coefficient
  r0[y_col] = 3.0;
  r0.greater_equal(100.0);
  const auto row_idx = lp.add_row(std::move(r0));

  LinearInterface li("",
                     lp.flatten({
                         .equilibration_method = LpEquilibrationMethod::none,
                         .scale_objective = kScaleObj,
                     }));

  REQUIRE(li.get_col_scale(x_col) == doctest::Approx(kColScale));
  REQUIRE(li.scale_objective() == doctest::Approx(kScaleObj));

  // ── Phase 1: solve with a = 5 ──
  const auto status1 = li.initial_solve({});
  REQUIRE((status1 && *status1 == 0));
  REQUIRE(li.is_optimal());

  const auto sol1 = li.get_col_sol();  // physical
  const double x1 = sol1[x_col];
  const double y1 = sol1[y_col];
  const double obj1 = li.get_obj_value();  // physical

  // Sanity: 5*x1 + 3*y1 >= 100 (the physical constraint is honoured).
  CHECK((5.0 * x1) + (3.0 * y1) >= doctest::Approx(100.0).epsilon(1e-6));
  CHECK(obj1 == doctest::Approx(20.0).epsilon(1e-6));  // x=20, y=0
  CHECK(x1 == doctest::Approx(20.0).epsilon(1e-6));

  // ── Phase 2: change physical coefficient on x to 10 via set_coeff ──
  li.set_coeff(row_idx, x_col, 10.0);

  // get_coeff round-trip: must return the just-written physical value.
  CHECK(li.get_coeff(row_idx, x_col) == doctest::Approx(10.0).epsilon(1e-9));

  const auto status2 = li.resolve({});
  REQUIRE((status2 && *status2 == 0));
  REQUIRE(li.is_optimal());

  const auto sol2 = li.get_col_sol();
  const double x2 = sol2[x_col];
  const double y2 = sol2[y_col];
  const double obj2 = li.get_obj_value();

  // The new constraint is 10*x + 3*y >= 100; optimum is x=10, y=0.
  // Pre-fix at col_scale=10 the solver enforced (10*0.1)*x + 3*y >= 100,
  // i.e. x_LP + 3*y >= 100 → x_phys = x_LP * 10 = up to 500 needed,
  // landing the LP at the box bound x=50 with y bridging the rest, or
  // diverging entirely from the intended optimum x_phys=10.
  CHECK((10.0 * x2) + (3.0 * y2) >= doctest::Approx(100.0).epsilon(1e-6));
  CHECK(obj2 == doctest::Approx(10.0).epsilon(1e-6));
  CHECK(x2 == doctest::Approx(10.0).epsilon(1e-6));
  CHECK(y2 == doctest::Approx(0.0).epsilon(1e-6));
}
