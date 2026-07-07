// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_water_fail_cost_magnitude.cpp
 * @brief     Magnitude-claim tests for `--auto-water-fail-cost` thresholds.
 * @date      2026-05-09
 *
 * Validates that the LP slack columns activate (or stay at zero) when the
 * `Reservoir.efin_cost` (storage-target slack) and `FlowRight.fcost`
 * (flow-target slack) cross the demand-fail-equivalent threshold predicted
 * by the helper that backs the `--auto-water-fail-cost` Python CLI option.
 *
 * Anchor formula (no losses, kept simple for the test):
 *
 *   water_fail_cost  =  max(falla.gcost across all demand failures)  +  $1
 *
 * Each test brackets the threshold at ±$1: the LP's optimum should flip
 * cleanly between "water target satisfied" and "water target violated"
 * across two cases that differ only by $2 in the underlying anchor.
 *
 * TEST_CASE A — Reservoir.efin_cost ($/hm³)
 *   threshold_efin_cost = demand_fail_cost × cascade_rendi × 10⁶ / 3600
 *   For demand_fail_cost=568 $/MWh, PF=10:  threshold ≈ 1,577,778 $/hm³
 *
 *     +$1 case: water_fail_cost = 569
 *               efin_cost ≈ 569 × 10 × 277.78 = 1,580,556 (ABOVE)
 *               LP fails demand, reservoir untouched, eos = eini, NOT emin.
 *
 *     −$1 case: water_fail_cost = 567
 *               efin_cost ≈ 567 × 10 × 277.78 = 1,575,000 (BELOW)
 *               LP drains the reservoir to serve demand, eos = emin.
 *
 * TEST_CASE B — FlowRight.fcost ($/(m³/s·h))
 *   threshold_flow_fail_cost = demand_fail_cost × PF
 *   For demand_fail_cost=568 $/MWh, PF=10:  threshold = 5,680 $/(m³/s·h)
 *
 *     +$1 case: water_fail_cost = 569 → fail_cost = 5,690 (ABOVE)
 *               LP honors FR (consumptive withdrawal), demand fails.
 *
 *     −$1 case: water_fail_cost = 567 → fail_cost = 5,670 (BELOW)
 *               LP fails FR (slack > 0), turbine runs, demand served.
 */

#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway.hpp>

using namespace gtopt;

namespace
{

using namespace gtopt;

/// Tiny single-bus 1-stage 2-block hydro topology used by both test cases.
/// Returns the simulation common to both reservoir/flow-right tests.
Simulation make_two_block_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
}

/// Common PlanningOptions for the magnitude tests.
///
/// `demand_fail_cost = 568` matches the "juan tier-4" magnitude that the
/// Python `_water_value` helper assumes.  `scale_objective = 1.0` so that
/// LP cost coefficients aren't divided down (we only assert on slack
/// primal values, not on the objective, but a 1.0 scale keeps things
/// straightforward to reason about).
PlanningOptions make_magnitude_options()
{
  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 568.0;
  opts.model_options.use_single_bus = OptBool {true};
  opts.model_options.use_kirchhoff = OptBool {false};
  opts.model_options.scale_objective = OptReal {1.0};
  return opts;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// TEST_CASE A — Reservoir.efin_cost slack threshold (±$1 around max(falla))
// ─────────────────────────────────────────────────────────────────────────────
//
// Topology (single-bus, 1-stage, 2 blocks of 1 h each):
//
//   bus b1 ── demand 1389 MW            (1389 × 2 h = 2778 MWh over stage)
//   bus b1 ── hydro_gen (gcost = 0, capacity high enough)
//   junction j_up ── reservoir rsv1   (eini = efin = 1.0 hm³, emin = 0)
//   junction j_up ──ww1──> junction j_down (drain=true)
//   turbine on ww1 (PF = 10 MW/(m³/s), waterway fmax = 1000 m³/s)
//   reservoir flow_conversion_rate = default 0.0036 (hm³ ↔ m³/s · h)
//
// Sized so that fully draining the reservoir (1 hm³) generates exactly
//   1 hm³ × 10 MW/(m³/s) × 10⁶/3600 = 2778 MWh
// of energy — exactly the stage demand.  This makes the LP's optimum
// a binary choice:
//
//   * No drain:    eos = eini = 1.0, slack = max(0, efin − eos) = 0,
//                  fail = 2778 MWh, cost = 568 × 2778 = 1,577,704 $.
//   * Full drain:  eos = emin = 0,   slack = efin − 0           = 1.0,
//                  fail = 0,         cost = efin_cost × 1.0.
//
// LP picks "full drain" iff efin_cost < 1,577,704 $/hm³ — exactly the
// threshold predicted by `demand_fail_cost × PF × 10⁶/3600`.
//
// The test brackets that threshold at ±$1 in the underlying anchor:
//
//   max(falla.gcost) = 568        (demand_fail_cost in this test)
//   water_fail_cost  = 568 ± 1    (the ±$1 design)
//   efin_cost        = water_fail_cost × PF × 277.78
//                    = (569 or 567) × 10 × 277.78
//                    = 1,580,556 (above) or 1,575,000 (below)
//
// Below threshold the reservoir reaches emin; above, it does not.

TEST_CASE(  // NOLINT
    "Reservoir.efin_cost slack threshold (auto-water-fail-cost magnitude)")
{
  using namespace gtopt;

  constexpr double kDemandFailCost = 568.0;  // max(falla.gcost)
  constexpr double kProdFactor = 10.0;  // MW per m³/s
  constexpr double kVolumeToFlowH = 1.0e6 / 3600.0;  // (m³/s · h) per hm³

  struct Subcase
  {
    const char* name;
    double water_fail_cost;
    /// true → below threshold (LP drains to emin, slack > 0).
    /// false → above threshold (LP keeps water, slack = 0).
    bool below_threshold;
  };

  // ±$1 around the exact demand-fail price — the LP's optimum should
  // flip between these two cases despite their underlying $/MWh
  // anchor differing by only $2.
  const Subcase subcases[] = {
      {
          "anchor +$1 (= 569) -> ABOVE threshold, eos = eini, slack = 0",
          kDemandFailCost + 1.0,
          /*below_threshold=*/false,
      },
      {
          "anchor -$1 (= 567) -> BELOW threshold, eos = emin, slack = eini",
          kDemandFailCost - 1.0,
          /*below_threshold=*/true,
      },
  };

  for (const auto& sc_param : subcases) {
    SUBCASE(sc_param.name)
    {
      const double efin_cost =
          sc_param.water_fail_cost * kProdFactor * kVolumeToFlowH;

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
              .gcost = 0.0,
              .capacity = 10'000.0,
          },
      };

      // demand = 1389 MW × 2 blocks × 1 h = 2778 MWh, exactly what 1 hm³
      // produces at PF=10 (= eini × kProdFactor × kVolumeToFlowH).
      const Array<Demand> demand_array = {
          {
              .uid = Uid {1},
              .name = "d1",
              .bus = Uid {1},
              .lmax = 1389.0,
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

      // fmax = 1000 m³/s gives ample per-block capacity to drain 0.5 hm³
      // per 1 h block (waterway is not the binding constraint).
      const Array<Waterway> waterway_array = {
          {
              .uid = Uid {1},
              .name = "ww1",
              .junction_a = Uid {1},
              .junction_b = Uid {2},
              .fmin = 0.0,
              .fmax = 1000.0,
          },
      };

      // eini = efin = 1.0 (no inherent gap), emin = 0.  Slack is therefore
      // a clean indicator of whether the LP drained anything: slack = 0
      // when no drain, slack = drained volume when LP releases water.
      const Array<Reservoir> reservoir_array = {
          {
              .uid = Uid {1},
              .name = "rsv1",
              .junction = Uid {1},
              .capacity = 1.0,
              .emin = 0.0,
              .emax = 1.0,
              .eini = 1.0,
              .efin = 1.0,
              .efin_cost = efin_cost,
          },
      };

      const Array<Turbine> turbine_array = {
          {
              .uid = Uid {1},
              .name = "tur1",
              .waterway = Uid {1},
              .generator = Uid {1},
              .production_factor = kProdFactor,
          },
      };

      const System system = {
          .name = "EfinCostMagnitude",
          .bus_array = bus_array,
          .demand_array = demand_array,
          .generator_array = generator_array,
          .junction_array = junction_array,
          .waterway_array = waterway_array,
          .reservoir_array = reservoir_array,
          .turbine_array = turbine_array,
      };

      auto opts = make_magnitude_options();
      const PlanningOptionsLP options {opts};
      const Simulation simulation = make_two_block_simulation();
      SimulationLP sim_lp(simulation, options);
      SystemLP sys_lp(system, sim_lp);

      auto& li = sys_lp.linear_interface();
      const auto result = li.resolve();
      REQUIRE(result.has_value());
      REQUIRE(result.value() == 0);

      const auto& rsv_lps = sys_lp.elements<ReservoirLP>();
      REQUIRE(rsv_lps.size() == 1);
      const auto& rsv_lp = rsv_lps.front();

      const auto& scenario_lp = sim_lp.scenarios().front();
      const auto& stage_lp = sim_lp.stages().front();

      const auto slack_opt = rsv_lp.efin_slack_col_at(scenario_lp, stage_lp);
      REQUIRE(slack_opt.has_value());

      const double slack_val = li.get_col_sol()[*slack_opt];
      const double eos =
          rsv_lp.physical_efin(li, scenario_lp, stage_lp, /*default_efin=*/1.0);

      MESSAGE("water_fail_cost=",
              sc_param.water_fail_cost,
              " efin_cost=",
              efin_cost,
              " slack=",
              slack_val,
              " eos=",
              eos);

      if (sc_param.below_threshold) {
        // Reservoir drained to emin — eos at the lower bound, slack ≈ eini.
        CHECK(eos == doctest::Approx(0.0).epsilon(1e-3));
        CHECK(slack_val == doctest::Approx(1.0).epsilon(1e-3));
      } else {
        // Reservoir untouched — eos at eini=efin, slack = 0.
        CHECK(eos == doctest::Approx(1.0).epsilon(1e-3));
        CHECK(slack_val == doctest::Approx(0.0).epsilon(1e-3));
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST_CASE B — FlowRight.fcost slack threshold
// ─────────────────────────────────────────────────────────────────────────────
//
// Topology mirrors TEST_CASE A but adds a FlowRight at j_up that demands
// some flow consumptively (subtracted from the junction balance).  This
// creates a tradeoff:
//
//   - Honor FlowRight: extracted flow leaves via the FR (consumptive),
//     less flow reaches the turbine -> some demand unserved.
//   - Fail FlowRight: pay fail_cost × shortfall, but full flow goes
//     through the turbine -> demand served at gcost=0.
//
// Threshold (no 277.78 factor — fail_cost is per (m³/s·h)):
//   threshold = demand_fail_cost × PF
//             = 568 × 10 = 5,680 $/(m³/s · h)
// Auto-derived value: 626.24 × 10 = 6,262 $/(m³/s · h) (above threshold).

TEST_CASE(  // NOLINT
    "FlowRight.fcost slack threshold (auto-water-fail-cost magnitude)")
{
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-unchecked-optional-access,
  // cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,modernize-use-designated-initializers)

  constexpr double kDemandFailCost = 568.0;  // max(falla.gcost) in this test
  constexpr double kProdFactor = 10.0;  // MW per m³/s

  struct Subcase
  {
    const char* name;
    double water_fail_cost;
    /// true → below threshold (FR fails fully, slack > 0).
    /// false → above threshold (FR honored, slack = 0).
    bool below_threshold;
  };

  // ±$1 around max(falla.gcost) — fail_cost = water_fail_cost × PF, so the
  // threshold of 5,680 $/(m³/s·h) is bracketed by 5,690 (above) and
  // 5,670 (below) — a $20-wide window around the LP's tipping point.
  const Subcase subcases[] = {
      {
          "anchor +$1 (= 569) -> fail_cost 5690 ABOVE -> FR honored, slack = 0",
          kDemandFailCost + 1.0,
          /*below_threshold=*/false,
      },
      {
          "anchor -$1 (= 567) -> fail_cost 5670 BELOW -> FR fails, slack > 0",
          kDemandFailCost - 1.0,
          /*below_threshold=*/true,
      },
  };

  for (const auto& sc_param : subcases) {
    SUBCASE(sc_param.name)
    {
      const double fail_cost = sc_param.water_fail_cost * kProdFactor;

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
              .gcost = 0.0,
              .capacity = 1000.0,
          },
      };

      // Demand 100 MW × 2 h = 200 MWh.
      const Array<Demand> demand_array = {
          {
              .uid = Uid {1},
              .name = "d1",
              .bus = Uid {1},
              .lmax = 100.0,
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
              .fmax = 100.0,
          },
      };

      // Natural inflow of 10 m³/s into j_up — exactly enough to drive
      // the turbine at the level required to serve 100 MW demand
      // (PF=10 → 100 MW needs 10 m³/s).  When the FlowRight diverts
      // any of this water consumptively, demand becomes unserved.
      const Array<Flow> flow_array = {
          {
              .uid = Uid {1},
              .name = "inflow_up",
              .direction = 1,
              .junction = Uid {1},
              .discharge = 10.0,
          },
      };

      const Array<Turbine> turbine_array = {
          {
              .uid = Uid {1},
              .name = "tur1",
              .waterway = Uid {1},
              .generator = Uid {1},
              .production_factor = 10.0,
          },
      };

      // FlowRight requesting 5 m³/s consumptively at j_up.  When
      // fail_cost is high enough, the LP allocates water to the FR
      // (turbine starves, demand fails); when low, the FR fails
      // (turbine runs full, demand served).
      const Array<FlowRight> flow_right_array = {
          {
              .uid = Uid {1},
              .name = "fr1",
              .junction_a = Uid {1},
              .direction = OptInt {-1},  // consumptive withdrawal
              .target = 5.0,
              .fcost = fail_cost,
          },
      };

      const System system = {
          .name = "FlowRightFailCostMagnitude",
          .bus_array = bus_array,
          .demand_array = demand_array,
          .generator_array = generator_array,
          .junction_array = junction_array,
          .waterway_array = waterway_array,
          .flow_array = flow_array,
          .turbine_array = turbine_array,
          .flow_right_array = flow_right_array,
      };

      auto opts = make_magnitude_options();
      const PlanningOptionsLP options {opts};
      const Simulation simulation = make_two_block_simulation();
      SimulationLP sim_lp(simulation, options);
      SystemLP sys_lp(system, sim_lp);

      auto& li = sys_lp.linear_interface();
      const auto result = li.resolve();
      REQUIRE(result.has_value());
      REQUIRE(result.value() == 0);

      const auto& fr_lps = sys_lp.elements<FlowRightLP>();
      REQUIRE(fr_lps.size() == 1);
      const auto& fr_lp = fr_lps.front();

      const auto& scenario_lp = sim_lp.scenarios().front();
      const auto& stage_lp = sim_lp.stages().front();

      // Post-P0: the `fail` LP column was substituted away
      // (`fail = discharge − flow`).  Reconstruct the per-block
      // shortfall via `fail_sol_at` and sum across blocks.
      double total_fail = 0.0;
      const auto& col_sol = li.get_col_sol();
      for (const auto& block : stage_lp.blocks()) {
        total_fail += fr_lp.fail_sol_at(scenario_lp, stage_lp, block, col_sol);
      }

      MESSAGE("water_fail_cost=",
              sc_param.water_fail_cost,
              " fail_cost=",
              fail_cost,
              " total_fail=",
              total_fail);

      if (sc_param.below_threshold) {
        // FR fails fully — slack absorbs the entire 5 m³/s × 2 h = 10
        // (m³/s · h) of unmet rights flow.  LP prefers this over the
        // demand-failure cost it would pay if the turbine had to share
        // water with the FR.
        CHECK(total_fail == doctest::Approx(10.0).epsilon(1e-3));
      } else {
        // FR honored — turbine starves, demand fails 50 MWh per block.
        // FR slack stays at zero.
        CHECK(total_fail == doctest::Approx(0.0).epsilon(1e-3));
      }
    }
  }
}

// NOLINTEND(bugprone-unchecked-optional-access,
// cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,modernize-use-designated-initializers)
