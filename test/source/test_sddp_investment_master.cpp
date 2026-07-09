// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_investment_master.cpp
 * @brief     OptGen-style investment-master Benders loop
 *            (`solve_investment_master`) — the deliverable-4 acceptance
 *            oracle of the SDDiP campaign (design in
 *            docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md
 *            §7; loop in `sddp_investment_master.cpp`).
 *
 * The acceptance test runs a SMALL pure-expansion fixture (single bus, NO
 * hydro so the capacity-space projection is EXACT — 0 dropped
 * coordinates, design §7.3 caveat 1) with one INTEGER expansion candidate
 * and 2 scenes / 2 stages, then asserts the master↔SDDP loop converges to
 * the MONOLITHIC MIP optimum on the same planning within tolerance.
 *
 * MIP-gated: the operational subproblems and the master both need an exact
 * MIP backend (`integer_expmod = true` makes the monolithic per-scene
 * problem a MIP, and the master is an integer program), so the test skips
 * when no exact MIP solver is loaded and treats a license failure as a
 * skip (`run_or_skip_license`), matching the unit-commitment conventions.
 */

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_investment_master.hpp>
#include <gtopt/sddp_types.hpp>

#include "fixture_helpers.hpp"
#include "solver_test_helpers.hpp"

using namespace gtopt;
using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

namespace
{

/// Pure-expansion, single-bus, no-hydro planning.
///
/// Economics (per stage, 1 block of 1 h, 2 stages, 2 equiprobable
/// scenes):
///   * demand `d1` = 100 MW, must be served or pay `demand_fail_cost`.
///   * base thermal `thermal` = 100 MW at $80/MWh — EXPENSIVE but able to
///     cover the whole load on its own.
///   * candidate `cheap` — base capacity 0, ONE integer module of 100 MW
///     at $5/MWh dispatch, `annual_capcost` set so building the single
///     module pays for itself across the horizon.
///
/// With `integer_expmod = true` the build is 0 or 1 (no fractional
/// module).  Building 1 module lets the cheap unit serve all 100 MW at
/// $5/MWh instead of $80/MWh — an operating saving that dwarfs the module
/// carrying charge, so the monolithic MIP (and the master) build exactly
/// one module.
[[nodiscard]] Planning make_pure_expansion_planning(std::string solver_name)
{
  constexpr int num_stages = 2;
  constexpr int blocks_per_stage = 1;

  auto block_array = make_uniform_blocks(
      static_cast<std::size_t>(num_stages * blocks_per_stage), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_stages),
                          static_cast<std::size_t>(blocks_per_stage));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_stages));

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
      .phase_array = std::move(phase_array),
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = "scene1",
                  .active = true,
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = "scene2",
                  .active = true,
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "cheap",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 0.0,
          .expcap = 100.0,
          .expmod = 1.0,
          .annual_capcost = 8760.0,  // $1/MW-hour carrying charge
          .integer_expmod = true,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const System system = {
      .name = "invmaster_pure_expansion",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 10000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;
  options.lp_matrix_options.solver_name = std::move(solver_name);

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = system,
  };
}

/// Monolithic MIP total objective on @p planning: sum of every
/// (scene, phase) system's LP/MIP objective.  With `integer_expmod`
/// active each per-scene LP is a MIP, so this is the true two-stage
/// stochastic MIP optimum — the loop's convergence target.
[[nodiscard]] double monolithic_mip_objective(Planning planning)
{
  PlanningLP plp(std::move(planning));
  MonolithicMethod method;
  const auto r = method.solve(plp, {});
  REQUIRE(r.has_value());

  double total = 0.0;
  for (const auto& phase_systems : plp.systems()) {
    for (const auto& system : phase_systems) {
      total += system.linear_interface().get_obj_value();
    }
  }
  return total;
}

}  // namespace

TEST_CASE(  // NOLINT
    "solve_investment_master — converges to the monolithic MIP optimum on a "
    "pure-expansion fixture")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping investment-master test");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        const auto planning = make_pure_expansion_planning(mip_solver);

        // Candidate discovery finds exactly the cheap unit.
        const auto cands = find_expansion_candidates(planning);
        REQUIRE(cands.size() == 1);
        CHECK(cands.front().class_name == "Generator");
        CHECK(cands.front().uid == Uid {2});
        CHECK(cands.front().max_expmod == doctest::Approx(1.0));
        CHECK(cands.front().integer);

        // Ground truth: monolithic MIP on the SAME planning.
        const double mono = monolithic_mip_objective(planning);

        InvestmentMasterOptions opts;
        opts.max_iterations = 12;
        opts.tol = 1.0e-3;
        opts.sddp_options.max_iterations = 8;
        opts.sddp_options.convergence_tol = 1.0e-9;
        opts.sddp_options.stationary_tol = 0.0;
        opts.sddp_options.cut_sharing = CutSharingMode::none;
        opts.sddp_options.apertures = std::vector<Uid> {};
        opts.sddp_options.enable_api = false;

        const auto res = solve_investment_master(planning, opts);
        REQUIRE(res.has_value());

        // The loop closes its gap (UB − LB ≤ tol; the difference can go
        // slightly negative when the master LB nudges past the operational
        // UB, so the assertion is one-sided).
        CHECK(res->converged);
        CHECK((res->upper_bound - res->lower_bound) <= opts.tol);

        // Both bounds match the monolithic MIP optimum.
        const double reltol = 1.0e-3 * std::max(1.0, std::abs(mono));
        CHECK(res->lower_bound == doctest::Approx(mono).epsilon(1.0e-4));
        CHECK(res->upper_bound == doctest::Approx(mono).epsilon(1.0e-4));
        CHECK(std::abs(res->upper_bound - mono) <= reltol);

        // The master builds exactly one module — the integer optimum.
        REQUIRE(res->best_builds.size() == 1);
        CHECK(res->best_builds.front() == doctest::Approx(1.0));
      });

  if (!ran) {
    MESSAGE("MIP solver license unavailable — investment-master test skipped");
  }
}

TEST_CASE(  // NOLINT
    "find_expansion_candidates — no candidates when no element expands")
{
  const auto mip_solver = solver_test::first_mip_solver();
  const std::string solver = mip_solver.empty() ? std::string {} : mip_solver;
  auto planning = make_pure_expansion_planning(solver);
  // Strip the candidate's expansion headroom.
  for (auto& g : planning.system.generator_array) {
    if (g.uid == Uid {2}) {
      g.expcap = {};
      g.expmod = {};
    }
  }
  CHECK(find_expansion_candidates(planning).empty());

  InvestmentMasterOptions opts;
  const auto res = solve_investment_master(planning, opts);
  // No candidates → a clean error, not a crash.
  CHECK_FALSE(res.has_value());
}
