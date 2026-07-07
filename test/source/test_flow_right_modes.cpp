// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the FlowRight LP refactor that introduces three time-
// resolution modes — `per_block`, `stage_average`, `stage_uniform` —
// behind a single `flow_mode` JSON field (with `use_average` as legacy
// alias).  Each test pins the LP shape (which columns / rows exist),
// the slack/cost wiring, and the duration-weighted stage bounds for
// `qeh`.
//
// Companion to `test_flow_right.cpp` / `test_flow_right_unified_modes`
// — those files target the pre-`flow_mode` shape and contain stale
// failing tests that are tracked separately; this file is the green
// baseline for the refactor.

#include <cmath>
#include <optional>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/json/json_flow_right.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

using namespace gtopt;

// ── Shared fixtures (single-bus, single-stage hydro topology) ────────────

struct FrmHydroFixture
{
  Array<Bus> bus_array {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
  };
  Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  Array<Junction> junction_array {
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
  Array<Waterway> waterway_array {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };
  Array<Reservoir> reservoir_array {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };
  Array<Flow> flow_array {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };
  Array<Turbine> turbine_array {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };
};

// Single-block stage.
[[nodiscard]] Simulation make_frm_simulation(double block_duration = 1.0,
                                             double prob = 1.0)
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = block_duration,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
                  .month = MonthType::april,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
                  .probability_factor = prob,
              },
          },
  };
}

// Two-block stage with configurable per-block durations (defaults 1h + 3h
// so duration ratios are 0.25 / 0.75).
[[nodiscard]] Simulation make_frm_two_block_simulation(double dur1 = 1.0,
                                                       double dur2 = 3.0,
                                                       double prob = 1.0)
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = dur1,
              },
              {
                  .uid = Uid {2},
                  .duration = dur2,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
                  .month = MonthType::april,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
                  .probability_factor = prob,
              },
          },
  };
}

[[nodiscard]] System make_frm_system(const FrmHydroFixture& fx,
                                     const Array<FlowRight>& frs,
                                     std::string name)
{
  return {
      .name = std::move(name),
      .bus_array = fx.bus_array,
      .demand_array = fx.demand_array,
      .generator_array = fx.generator_array,
      .junction_array = fx.junction_array,
      .waterway_array = fx.waterway_array,
      .flow_array = fx.flow_array,
      .reservoir_array = fx.reservoir_array,
      .turbine_array = fx.turbine_array,
      .flow_right_array = frs,
  };
}

/// Return a base PlanningOptions with `scale_objective = 1` so raw
/// objective coefficients can be asserted exactly.  The caller wraps it
/// in a `PlanningOptionsLP` *locally* — wrapping inside this helper and
/// returning by value would invalidate the wrapped map's internal
/// `string_view`s into `variable_scales`.
[[nodiscard]] PlanningOptions make_frm_unscaled_options()
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;  // keep raw obj coeffs.
  return popts;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// TEST_CASE: JSON resolution of flow_mode / use_average
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("FlowRight flow_mode JSON resolution")  // NOLINT
{
  // Resolution semantics live in `resolve_flow_mode()` inside an
  // anonymous namespace.  We exercise them indirectly by building the
  // SystemLP and checking which LP entities exist (per-block flow cols
  // only for `per_block`; qeh col present for `stage_average` and
  // `stage_uniform`; qavg row present only for `stage_average`).
  const FrmHydroFixture fx;

  // Callback-style: `SystemLP` holds a non-owning `SimulationLP&`, so
  // returning it from a helper would dangle.  Run all assertions inside
  // the lambda body while both objects are alive on the stack.
  auto with_mode = [&fx](const Array<FlowRight>& frs, auto&& body)
  {
    const auto sim = make_frm_simulation();
    const auto sys = make_frm_system(fx, frs, "FlowModeJsonRes");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);
    body(system_lp);
  };

  SUBCASE("default (neither flow_mode nor use_average set) ⇒ per_block")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_default",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(!fr_lp.flow_cols_at(s, t).empty());
                CHECK_FALSE(fr_lp.has_qeh(s, t));
                CHECK_FALSE(fr_lp.has_qavg_row(s, t));
              });
  }

  SUBCASE("flow_mode: per_block ⇒ per_block")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_per_block",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "per_block",
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(!fr_lp.flow_cols_at(s, t).empty());
                CHECK_FALSE(fr_lp.has_qeh(s, t));
              });
  }

  SUBCASE("flow_mode: stage_average ⇒ stage_average")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_sa",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "stage_average",
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(!fr_lp.flow_cols_at(s, t).empty());
                CHECK(fr_lp.has_qeh(s, t));
                CHECK(fr_lp.has_qavg_row(s, t));
              });
  }

  SUBCASE("flow_mode: stage_uniform ⇒ stage_uniform")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_su",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "stage_uniform",
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                // No per-block flow column in stage_uniform.
                CHECK(fr_lp.flow_cols_at(s, t).empty());
                CHECK(fr_lp.has_qeh(s, t));
                // No qavg row in stage_uniform.
                CHECK_FALSE(fr_lp.has_qavg_row(s, t));
              });
  }

  SUBCASE("legacy use_average: true ⇒ stage_average")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_legacy",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .use_average = true,
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(fr_lp.has_qeh(s, t));
                CHECK(fr_lp.has_qavg_row(s, t));
                // No target / no fcost → no kink slacks even at stage.
                CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
                CHECK_FALSE(fr_lp.has_qkink_row(s, t));
              });
  }

  SUBCASE("flow_mode wins over use_average")
  {
    // flow_mode = per_block must take precedence even when use_average
    // is true.  Exercises the explicit-string short-circuit in
    // `resolve_flow_mode`.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_priority",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "per_block",
            .use_average = true,
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(!fr_lp.flow_cols_at(s, t).empty());
                CHECK_FALSE(fr_lp.has_qeh(s, t));
              });
  }

  SUBCASE("unknown flow_mode falls back to use_average")
  {
    // "bogus" is not in {per_block, stage_average, stage_uniform};
    // resolve_flow_mode logs a warning and falls back to the
    // use_average switch (true → stage_average here).
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_bogus",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "bogus",
            .use_average = true,
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(fr_lp.has_qeh(s, t));
              });
  }

  SUBCASE("unknown flow_mode + no use_average ⇒ per_block")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_bogus2",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "bogus",
        },
    };
    with_mode(frs,
              [&](SystemLP& sys)
              {
                const auto& fr_lp = sys.elements<FlowRightLP>().front();
                const auto& s = sys.scene().scenarios()[0];
                const auto& t = sys.phase().stages()[0];
                CHECK(!fr_lp.flow_cols_at(s, t).empty());
                CHECK_FALSE(fr_lp.has_qeh(s, t));
              });
  }
}

// ─────────────────────────────────────────────────────────────────────────
// TEST_CASE: per_block LP shape
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("FlowRight per_block LP shape")  // NOLINT
{
  // Two-block stage so duration-weighting matters.  All subcases here
  // use `flow_mode = per_block` (the default) and inspect bounds, costs
  // and slack columns on the per-block `flow_b` / `fail_b` / `excess_b`.
  const FrmHydroFixture fx;

  SUBCASE("hard band, no target ⇒ flow_b ∈ [10, 50], zero obj cost, no slacks")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_band",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = 10.0,
            .fmax = 50.0,
        },
    };
    const auto sim = make_frm_two_block_simulation();
    const auto sys = make_frm_system(fx, frs, "PerBlockHardBand");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];

    const auto col_low = lp.get_col_low();
    const auto col_upp = lp.get_col_upp();
    const auto obj = lp.get_obj_coeff();
    const auto& fcols = fr_lp.flow_cols_at(s, t);
    REQUIRE(fcols.size() == 2);
    for (const auto& [buid, col] : fcols) {
      CHECK(col_low[col] == doctest::Approx(10.0));
      CHECK(col_upp[col] == doctest::Approx(50.0));
      CHECK(obj[col] == doctest::Approx(0.0));
    }
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qeh(s, t));
  }

  SUBCASE("forced exact fmin == fmax = 20")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_exact",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = 20.0,
            .fmax = 20.0,
        },
    };
    const auto sim = make_frm_two_block_simulation();
    const auto sys = make_frm_system(fx, frs, "PerBlockForcedExact");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    auto& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());

    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    const auto col_sol = lp.get_col_sol();
    for (const auto& [buid, col] : fr_lp.flow_cols_at(s, t)) {
      CHECK(col_sol[col] == doctest::Approx(20.0));
    }
    // No slack columns since no target.
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
  }

  SUBCASE("irrigation soft target — fail slack carries +fcost·cf coeff")
  {
    // fmin=0, target=30, fmax=defaults to target=30, fcost=100.
    // Per block: sn_cost_cf = fcost · prob · disc · block_duration.
    // With prob=1, disc=1, block durations (1, 3) we expect coeffs
    // (100, 300) on the two `fail_b` slacks.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_irrig",
            .junction_a = Uid {1},
            .direction = -1,
            .target = 30.0,
            .fcost = 100.0,
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "PerBlockIrrigSoft");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];

    const auto obj = lp.get_obj_coeff();
    const auto& fcols = fr_lp.flow_cols_at(s, t);
    REQUIRE(fcols.size() == 2);
    // Under the one-sided fcost-only substitution (`fail = target − flow`)
    // the explicit fail slack column is folded into the primary flow
    // column: flow cost becomes `−fcost·dur` (was 0), the fail_col is
    // not created, and an obj_constant `+fcost·dur·target` rides on
    // `lp.add_obj_constant`.  Same algebraic objective.
    for (const auto& [buid, fcol] : fcols) {
      // Find the matching block duration.
      const auto& block = *std::ranges::find_if(
          t.blocks(), [&](const auto& blk) { return blk.uid() == buid; });
      const auto expected_flow_cost = -100.0 * block.duration();
      CHECK(obj[fcol] == doctest::Approx(expected_flow_cost));
    }
    // No explicit slack columns under the substitution.
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
    for (const auto& block : t.blocks()) {
      CHECK_FALSE(fr_lp.fail_col_at(s, t, block).has_value());
      CHECK_FALSE(fr_lp.excess_col_at(s, t, block).has_value());
    }
  }

  SUBCASE("target with bonus uvalue installs both slacks with opposite signs")
  {
    // fmin=0, target=30, fmax=50, fcost=100, uvalue=80.
    // sn_cost_cf = +100 · dur (penalty on deficit).
    // sp_cost_cf = -80  · dur (bonus / negative cost on excess).
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_bonus",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 50.0,
            .target = 30.0,
            .fcost = 100.0,
            .uvalue = 80.0,
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "PerBlockBonus");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    auto& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());

    const auto obj = lp.get_obj_coeff();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    for (const auto& block : t.blocks()) {
      const auto fail_col = fr_lp.fail_col_at(s, t, block);
      const auto excess_col = fr_lp.excess_col_at(s, t, block);
      REQUIRE(fail_col.has_value());
      REQUIRE(excess_col.has_value());
      const auto dur = block.duration();
      CHECK(obj[*fail_col] == doctest::Approx(100.0 * dur));
      CHECK(obj[*excess_col] == doctest::Approx(-80.0 * dur));
    }
    // With abundant supply and a positive uvalue the LP drives flow to
    // fmax; excess_sol_at must report excess = fmax - target = 20.
    const auto col_sol = lp.get_col_sol();
    for (const auto& block : t.blocks()) {
      const auto ex = fr_lp.excess_sol_at(s, t, block, col_sol);
      const auto fs = fr_lp.fail_sol_at(s, t, block, col_sol);
      CHECK(ex == doctest::Approx(20.0));
      CHECK(fs == doctest::Approx(0.0));
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// TEST_CASE: stage_average LP shape
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("FlowRight stage_average LP shape")  // NOLINT
{
  const FrmHydroFixture fx;

  SUBCASE("per-block flow has hard bounds, zero obj cost, no per-block slacks")
  {
    // fmin_b varies between blocks (10, 20), fmax_b = 100.  No target →
    // no kink anywhere (block or stage).
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_sa_band",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = std::vector<std::vector<Real>> {{10.0, 20.0}},
            .fmax = std::vector<std::vector<Real>> {{100.0, 100.0}},
            .flow_mode = "stage_average",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageAvgBand");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];

    const auto obj = lp.get_obj_coeff();
    const auto col_low = lp.get_col_low();
    const auto col_upp = lp.get_col_upp();
    const auto& fcols = fr_lp.flow_cols_at(s, t);
    REQUIRE(fcols.size() == 2);
    // The fixture stores fmin/fmax block-major: row 0 = stage 0, with
    // block 0 → 10 and block 1 → 20.  Verify the band+zero-cost shape
    // without depending on the per-block ordering of the map.
    std::vector<double> lows, ups;
    for (const auto& [buid, col] : fcols) {
      lows.push_back(col_low[col]);
      ups.push_back(col_upp[col]);
      CHECK(obj[col] == doctest::Approx(0.0));
    }
    std::ranges::sort(lows);
    std::ranges::sort(ups);
    CHECK(lows[0] == doctest::Approx(10.0));
    CHECK(lows[1] == doctest::Approx(20.0));
    CHECK(ups[0] == doctest::Approx(100.0));
    CHECK(ups[1] == doctest::Approx(100.0));
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
  }

  SUBCASE("qeh is bounded by duration-weighted fmin_e / fmax_e")
  {
    // fmin = (10, 20), fmax = (40, 40), block durations (1, 3) ⇒
    // dur_ratios (0.25, 0.75).  Expected:
    //   fmin_e = 0.25 * 10 + 0.75 * 20 = 17.5
    //   fmax_e = 0.25 * 40 + 0.75 * 40 = 40.0
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_sa_qeh",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = std::vector<std::vector<Real>> {{10.0, 20.0}},
            .fmax = std::vector<std::vector<Real>> {{40.0, 40.0}},
            .flow_mode = "stage_average",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageAvgQeh");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    REQUIRE(fr_lp.has_qeh(s, t));
    const auto qeh = fr_lp.qeh_col_at(s, t);
    const auto col_low = lp.get_col_low();
    const auto col_upp = lp.get_col_upp();
    CHECK(col_low[qeh] == doctest::Approx(17.5));
    CHECK(col_upp[qeh] == doctest::Approx(40.0));
    // No kink at stage scope (no target / no cost).
    CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qkink_row(s, t));
    // qavg row exists (stage_average).
    CHECK(fr_lp.has_qavg_row(s, t));
  }

  SUBCASE("infinite fmax_b propagates fmax_e → +inf (no overflow)")
  {
    // Set the larger block's fmax to LP infinity (DblMax sentinel),
    // which add_to_lp's `is_infinity` guard must propagate to fmax_e
    // without summing through DblMax (which would overflow to +inf or
    // NaN under floating arithmetic).
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_sa_inf",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = std::vector<std::vector<Real>> {{0.0, 0.0}},
            .fmax =
                std::vector<std::vector<Real>> {{20.0, LinearProblem::DblMax}},
            .flow_mode = "stage_average",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageAvgInf");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];

    const auto qeh = fr_lp.qeh_col_at(s, t);
    const auto col_upp = lp.get_col_upp();
    const double upper = col_upp[qeh];
    // The upper bound must be the LinearInterface's solver-side
    // infinity (or larger), never a finite arithmetic of DblMax.
    CHECK(lp.is_pos_inf(upper));
    CHECK_FALSE(std::isnan(upper));
  }

  SUBCASE("stage kink slacks installed when target + fcost are both set")
  {
    // target=30, fcost=100 in stage_average ⇒ slacks live on qeh, with
    // sn_cost_cf = fcost · prob · disc · stage_duration.  Stage duration
    // = 1 + 3 = 4 h.  Expected qeh_sn obj coeff = 400.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_sa_kink",
            .junction_a = Uid {1},
            .direction = -1,
            .target = 30.0,
            .flow_mode = "stage_average",
            .fcost = 100.0,
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageAvgKink");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    CHECK(fr_lp.has_qeh(s, t));
    // Under the one-sided fcost-only substitution the stage-level kink
    // slack + row are folded into the qeh column (cost = `−fcost·dur`,
    // upper bound clamped at target).  Algebraically equivalent.
    CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qkink_row(s, t));
    // Per-block slacks are NOT installed in stage_average (target=nullopt
    // at the block call to attach_flow).
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
  }

  SUBCASE("stage kink NOT installed when target set but both costs are zero")
  {
    // attach_flow's optimisation: with target set but `sp_cost_cf` and
    // `sn_cost_cf` both 0, no slacks / row are emitted.  In
    // stage_average this means qeh still exists but no qeh_sp /
    // qeh_sn / qkink.  We use fcost=0, uvalue=0 explicitly and pin
    // hydro_fail_cost to 0 to defeat the global fallback that would
    // otherwise activate the kink.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_sa_no_cost",
            .junction_a = Uid {1},
            .direction = -1,
            .target = 30.0,
            .flow_mode = "stage_average",
            .fcost = 0.0,
            .uvalue = 0.0,
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageAvgNoCost");
    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    popts.model_options.scale_objective = 1.0;
    popts.model_options.hydro_spill_cost = 0.0;
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    CHECK(fr_lp.has_qeh(s, t));
    CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qkink_row(s, t));
  }
}

// ─────────────────────────────────────────────────────────────────────────
// TEST_CASE: stage_uniform LP shape
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("FlowRight stage_uniform LP shape")  // NOLINT
{
  const FrmHydroFixture fx;

  SUBCASE("no per-block flow columns are created")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_su_only",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 40.0,
            .flow_mode = "stage_uniform",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageUniNoBlocks");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    // The inner BIndexHolder for stage_uniform is still stored (always
    // emplaced by add_to_lp) but contains zero entries.
    CHECK(fr_lp.flow_cols_at(s, t).empty());
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
  }

  SUBCASE("qeh exists with duration-weighted [fmin_e, fmax_e]")
  {
    // fmin = (10, 20), fmax = (40, 40), durations (1, 3).  Same weighted
    // formula as stage_average: fmin_e = 17.5, fmax_e = 40.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_su_qeh",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = std::vector<std::vector<Real>> {{10.0, 20.0}},
            .fmax = std::vector<std::vector<Real>> {{40.0, 40.0}},
            .flow_mode = "stage_uniform",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageUniQehBounds");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    REQUIRE(fr_lp.has_qeh(s, t));
    const auto qeh = fr_lp.qeh_col_at(s, t);
    const auto col_low = lp.get_col_low();
    const auto col_upp = lp.get_col_upp();
    CHECK(col_low[qeh] == doctest::Approx(17.5));
    CHECK(col_upp[qeh] == doctest::Approx(40.0));
  }

  SUBCASE("qeh appears in every block balance with coefficient -1")
  {
    // The defining feature of stage_uniform: the same `qeh` column is
    // subtracted from every block's junction balance row (rather than
    // per-block flow_b).  We check the coefficient via lp.get_coeff().
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_su_balance",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 30.0,
            .flow_mode = "stage_uniform",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageUniBalance");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    const auto qeh = fr_lp.qeh_col_at(s, t);

    // Find the upstream junction (uid=1) and inspect its balance rows.
    const auto& j_lp_vec = system_lp.elements<JunctionLP>();
    REQUIRE(!j_lp_vec.empty());
    const JunctionLP* upstream = nullptr;
    for (const auto& j : j_lp_vec) {
      if (j.uid() == Uid {1}) {
        upstream = &j;
      }
    }
    REQUIRE(upstream != nullptr);
    const auto& brows = upstream->balance_rows_at(s, t);
    REQUIRE(brows.size() == 2);
    for (const auto& [buid, row] : brows) {
      CHECK(lp.get_coeff(row, qeh) == doctest::Approx(-1.0));
    }
  }

  SUBCASE("no qavg row created in stage_uniform")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_su_no_qavg",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 25.0,
            .flow_mode = "stage_uniform",
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageUniNoQavg");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    CHECK(fr_lp.has_qeh(s, t));
    CHECK_FALSE(fr_lp.has_qavg_row(s, t));
  }

  SUBCASE("stage_uniform with target + fcost installs only the stage kink")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_su_kink",
            .junction_a = Uid {1},
            .direction = -1,
            .target = 20.0,
            .flow_mode = "stage_uniform",
            .fcost = 50.0,
        },
    };
    const auto sim = make_frm_two_block_simulation(1.0, 3.0);
    const auto sys = make_frm_system(fx, frs, "StageUniKink");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    // One-sided fcost-only substitution: qeh col carries cost
    // `−fcost·dur` and an upper bound clamped at target; explicit
    // slacks + row are elided.
    CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qkink_row(s, t));
    // No per-block flow cols, hence no per-block slacks either.
    CHECK(fr_lp.flow_cols_at(s, t).empty());
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
  }
}

// ─────────────────────────────────────────────────────────────────────────
// TEST_CASE: attach_flow edge cases — slack-installation guard
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("FlowRight attach_flow edge cases")  // NOLINT
{
  const FrmHydroFixture fx;

  SUBCASE("per_block, no target ⇒ no slacks emitted")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_no_target",
            .junction_a = Uid {1},
            .direction = -1,
            .fmax = 10.0,
            .flow_mode = "per_block",
        },
    };
    const auto sim = make_frm_simulation();
    const auto sys = make_frm_system(fx, frs, "AttachNoTarget");
    const auto popts = make_frm_unscaled_options();
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
    CHECK_FALSE(fr_lp.has_qkink_row(s, t));
  }

  SUBCASE("per_block, target set but both costs zero ⇒ no slacks emitted")
  {
    // attach_flow's `(sp_cost_cf != 0 || sn_cost_cf != 0)` guard: with
    // target set and BOTH costs zero the kink slacks/row are skipped.
    // We zero out the global hydro_fail_cost so the per-element 0
    // fcost isn't backfilled.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_zero_costs",
            .junction_a = Uid {1},
            .direction = -1,
            .target = 25.0,
            .flow_mode = "per_block",
            .fcost = 0.0,
            .uvalue = 0.0,
        },
    };
    const auto sim = make_frm_simulation();
    const auto sys = make_frm_system(fx, frs, "AttachZeroCosts");
    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    popts.model_options.scale_objective = 1.0;
    popts.model_options.hydro_spill_cost = 0.0;
    const PlanningOptionsLP opts {popts};
    SimulationLP simulation_lp(sim, opts);
    SystemLP system_lp(sys, simulation_lp);

    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    CHECK_FALSE(fr_lp.has_block_slacks(s, t));
    // Per-block flow column still installed (just the slacks/row skip).
    CHECK(!fr_lp.flow_cols_at(s, t).empty());
  }
}
