// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_block_state_sddp_cuts.cpp
 * @brief     SDDP cut persistence and state-gradient (water-value) sign for the
 *            AMPL `block_state` reservoir.
 * @date      2026-06-29
 * @copyright BSD-3-Clause
 *
 * Two checks the forward-trajectory / bounds-equivalence tests don't cover:
 *
 *  1. CUT I/O ROUND-TRIP — a `block_state` reservoir registers its cross-phase
 *     state under the dedicated class name `UserReservoirState`.  SDDP cuts are
 *     persisted to Parquet keyed by that class name; if it did not survive the
 *     save/load round-trip the loaded cuts would not match the state variables,
 *     the future-cost columns would free-fall, and the lower bound would
 *     collapse.  We solve to convergence, save the cuts, hot-start a fresh
 *     solver from them, and assert the loaded run reproduces the converged
 *     lower bound (and the cut count is preserved).
 *
 *  2. WATER-VALUE SIGN — the SDDP cut gradient with respect to the reservoir
 *     state IS the marginal value of stored water (the state dual).  Rather
 *     than reach into private incoming-column indices, we verify the sign
 *     through LB sensitivity: on a water-SCARCE case the converged cost must
 *     strictly DECREASE as the initial volume increases (more water displaces
 *     expensive thermal), i.e. ∂cost/∂eini < 0 — a negative state dual, the
 *     correct sign for a minimisation.
 */

#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/user_constraint.hpp>

#include "fixture_helpers.hpp"

using namespace gtopt;

namespace block_state_sddp_cuts_test
{
namespace
{

using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

constexpr int num_stages = 4;

/// A water-SCARCE single-reservoir AMPL `block_state` SDDP case: one hydro
/// generator (cheap) drawing from `vol1`, an expensive thermal backup, a
/// constant 30 MW load, no inflow, and a hard terminal `vol1 >= 20`.  With a
/// limited initial volume the reservoir must ration water across the horizon,
/// so cuts (the future water value) genuinely bind and `eini` moves the cost.
[[nodiscard]] auto make_scarce_block_state_planning(double eini) -> Planning
{
  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(num_stages), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_stages), 1);
  for (auto& st : stage_array) {
    st.chronological = OptBool {true};
  }
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_stages));

  std::vector<std::vector<double>> lmax_per_stage(num_stages,
                                                  std::vector<double> {30.0});

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen_1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 30.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,  // expensive backup the reservoir displaces
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .lmax = lmax_per_stage,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {.uid = Uid {1}, .probability_factor = 0.5},
              {.uid = Uid {2}, .probability_factor = 0.5},
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

  PlanningOptions options;
  options.model_options.demand_fail_cost = 10'000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;
  options.method = MethodType::sddp;

  Planning p {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "block_state_scarce",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
          },
  };

  p.system.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {101},
      .name = "vol1",
      .lower_bound = OptReal {0.0},
      .upper_bound = OptReal {1000.0},
      .scope = OptName {"block"},
      .link = OptBool {true},
      .block_state = OptBool {true},
      .initial_value = OptReal {eini},
  });
  // Balance: vol1 - prev(vol1) + generation = 0 (inflow 0, duration 1).
  p.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {201},
      .name = "vol1_balance",
      .expression = "decision_variable('vol1').value "
                    "- prev(decision_variable('vol1').value) "
                    "+ generator('hydro_gen_1').generation = 0",
  });
  // Hard terminal target on the last stage.
  p.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {202},
      .name = "vol1_terminal",
      .expression = "decision_variable('vol1').value >= 20, for(stage in {4})",
  });

  return p;
}

[[nodiscard]] auto sddp_opts(int max_iters) -> SDDPOptions
{
  SDDPOptions opts;
  opts.max_iterations = max_iters;
  opts.convergence_tol = 1.0e-6;
  opts.cut_sharing = CutSharingMode::none;
  opts.enable_api = false;
  return opts;
}

/// Converged lower bound for a scarce case with the given initial volume.
[[nodiscard]] auto converged_lb(double eini) -> double
{
  PlanningLP plp(make_scarce_block_state_planning(eini));
  SDDPMethod sddp(plp, sddp_opts(16));
  auto r = sddp.solve();
  REQUIRE(r.has_value());
  REQUIRE_FALSE(r->empty());
  return r->back().lower_bound;
}

}  // namespace
}  // namespace block_state_sddp_cuts_test

TEST_CASE(
    "block_state SDDP: water-value state gradient has the right sign "
    "(more initial volume ⇒ strictly lower cost)")
{
  using namespace block_state_sddp_cuts_test;

  // Scarce case: usable water = eini − terminal(20).  eini=50 ⇒ 30 usable;
  // eini=80 ⇒ 60 usable.  Each extra unit of water displaces 50 $/MWh thermal
  // with 1 $/MWh hydro, so the converged cost must fall as eini rises.
  const double lb_low = converged_lb(50.0);
  const double lb_high = converged_lb(80.0);

  CHECK(std::isfinite(lb_low));
  CHECK(std::isfinite(lb_high));
  // ∂cost/∂eini < 0 — the state dual (water value) is negative, the correct
  // sign for a minimisation.  Gap is large and unambiguous (~30 units × ~49).
  CHECK(lb_high < lb_low - 100.0);
}

TEST_CASE(
    "block_state SDDP: cuts round-trip through Parquet (UserReservoirState "
    "class name survives save/load)")
{
  using namespace block_state_sddp_cuts_test;

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file =
      (tmp_dir / "gtopt_test_block_state_cut_roundtrip.parquet").string();

  double final_lb = 0.0;
  int num_saved = 0;

  // Phase 1: solve to convergence and persist the cuts.
  {
    PlanningLP plp(make_scarce_block_state_planning(60.0));
    auto opts = sddp_opts(16);
    opts.cuts_output_file = cuts_file;
    SDDPMethod sddp(plp, opts);
    auto r = sddp.solve();
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->empty());
    REQUIRE_FALSE(sddp.stored_cuts().empty());
    final_lb = r->back().lower_bound;
    num_saved = static_cast<int>(sddp.stored_cuts().size());
  }
  REQUIRE(std::filesystem::exists(cuts_file));
  REQUIRE(num_saved > 0);

  // Phase 2: hot-start a fresh solver from the saved cuts with a single
  // iteration.  If `UserReservoirState` did not round-trip, the loaded cuts
  // would not bind the reservoir's future-cost column and the LB would be far
  // below the converged value; instead it must reproduce it.
  {
    PlanningLP plp2(make_scarce_block_state_planning(60.0));
    auto opts = sddp_opts(1);
    opts.cuts_input_file = cuts_file;
    opts.cut_recovery_mode = HotStartMode::replace;
    opts.recovery_mode = RecoveryMode::cuts;
    SDDPMethod sddp2(plp2, opts);
    auto r2 = sddp2.solve();
    REQUIRE(r2.has_value());
    REQUIRE_FALSE(r2->empty());

    // Cuts were loaded (the loader may prune dominated cuts, so the count can
    // be < num_saved — the binding test is the bound reproduction below).
    const auto num_loaded = static_cast<int>(sddp2.stored_cuts().size());
    CHECK(num_loaded > 0);

    const double lb2 = r2->back().lower_bound;
    CHECK(std::isfinite(lb2));
    // Loaded cuts reproduce the converged lower bound — the decisive check that
    // the `UserReservoirState` class name round-tripped and the cuts bind the
    // reservoir's future-cost column (a mismatch would collapse the LB).
    CHECK(lb2 == doctest::Approx(final_lb).epsilon(1e-3));
  }

  std::filesystem::remove(cuts_file);
}
