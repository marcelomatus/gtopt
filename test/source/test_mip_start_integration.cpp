/**
 * @file      test_mip_start_integration.cpp
 * @brief     End-to-end test of the MIP-start hook in SystemLP::resolve
 * @date      2026-06-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Exercises the one path the solver-independent unit tests in
 * `test_mip_start.cpp` cannot reach: the `SystemLP::resolve` hook that
 * (1) gathers per-(scenario, commitment) `CommitmentRunInfo` via
 * `CommitmentLP::find_status_cols`, (2) calls `apply_mip_start` before the
 * MIP solve, and (3) drives the CPLEX backend `set_mip_start` /
 * `restore_integers` round-trip.  A small monolithic unit-commitment MIP is
 * built (mirroring `test_mip_solvers.cpp`'s build), the MIP-start method is
 * enabled, the case is solved through `SystemLP::resolve`, and the
 * commitment status binaries are checked to be integral in the solution.
 *
 * Gated on `SolverRegistry::has_mip_solver()` (CPLEX here) — without a MIP
 * solver the whole feature is a no-op and the test self-skips.
 */

#include <optional>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/commitment_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/monolithic_enums.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace mip_start_integration_test  // NOLINT(misc-use-anonymous-namespace)
{

namespace
{

const Array<Bus> single_bus = {
    {
        .uid = Uid {1},
        .name = "b1",
    },
};

// Three chronological 1-hour blocks so the commitment has a status binary in
// each block (and run-length info would be populated when min-up/down is set).
const Simulation three_block_simulation = {
    .block_array =
        {
            {
                .uid = Uid {0},
                .duration = 1.0,
            },
            {
                .uid = Uid {1},
                .duration = 1.0,
            },
            {
                .uid = Uid {2},
                .duration = 1.0,
            },
        },
    .stage_array =
        {
            {
                .uid = Uid {0},
                .first_block = 0,
                .count_block = 3,
                .chronological = true,
            },
        },
    .scenario_array =
        {
            {
                .uid = Uid {0},
            },
        },
};

// A single generator with a startup-costed commitment and a demand profile
// (0, 60, 60) so the optimum starts the unit at block 1 and keeps it on: the
// status binaries are 0, 1, 1.  Shared by the warmstart and scip_repair tests.
[[nodiscard]] System make_commitment_system()
{
  System system;
  system.name = "MipStartIntegration";
  system.bus_array = single_bus;
  system.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = TBRealFieldSched {std::vector<std::vector<Real>> {
              {
                  0.0,
                  60.0,
                  60.0,
              },
          }},
          .capacity = 100.0,
      },
  };
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "cmt1",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .shutdown_cost = 50.0,
          .pmin = 30.0,
          .initial_status = 0.0,
      },
  };
  return system;
}

}  // namespace

TEST_CASE(  // NOLINT
    "MipStart integration: warmstart through SystemLP::resolve keeps "
    "commitment status integral")
{
  SolverRegistry& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP-start integration test — no MIP solver available");
    return;
  }

  System system = make_commitment_system();

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  poptions.lp_matrix_options.col_with_names = true;
  poptions.lp_matrix_options.col_with_name_map = true;
  // Enable the MIP-start hook: this is the surface under test.
  poptions.monolithic_options.mip_start.emplace();
  poptions.monolithic_options.mip_start->enabled = true;

  PlanningOptionsLP options(std::move(poptions));
  REQUIRE(options.mip_start_options().enabled.value_or(false) == true);

  SimulationLP simulation_lp(three_block_simulation, options);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  SystemLP system_lp(system, simulation_lp, flat_opts);

  auto&& lp = system_lp.linear_interface();

  // The monolithic build (low_memory off) retains the flat-LP snapshot
  // UNCOMPRESSED via save_snapshot — exactly what the scip_repair MIP-start
  // generator builds its second (SCIP) backend from, and what the
  // SystemLP::resolve hook passes to apply_mip_start as `flat_lp`.  Assert it
  // is present and decompressed so scip_repair receives a non-null flat LP.
  {
    const auto* flat = lp.flat_lp_snapshot();
    REQUIRE(flat != nullptr);
    CHECK(flat->ncols
          == static_cast<FlatLinearProblem::index_t>(lp.get_numcols()));
    CHECK(flat->nrows
          == static_cast<FlatLinearProblem::index_t>(lp.get_numrows()));
  }

  // Sanity: a commitment status binary exists before we solve.
  const auto& comm_lps = system_lp.elements<CommitmentLP>();
  REQUIRE(comm_lps.size() == 1);
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto* status_cols =
      comm_lps.front().find_status_cols(scenario_lp, stage_lp);
  REQUIRE(status_cols != nullptr);
  REQUIRE(status_cols->size() == 3);
  for (const auto& [block_uid, col] : *status_cols) {
    CHECK(lp.is_integer(col));
  }

  // Drive the SystemLP::resolve hook (NOT lp.resolve()) so apply_mip_start
  // runs: relaxation solve → warmstart generate → restore_integers →
  // set_mip_start → MIP solve.
  const auto result = system_lp.resolve(SolverOptions {.log_level = 0});
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  REQUIRE(lp.is_optimal());

  // The optimal status binaries must be clean 0/1 VALUES.
  //
  // NOTE on the is_integer flag: the default `write_out`
  // (solution|dual|reduced_cost) triggers the post-MIP fix-integers
  // dual-recovery pass in SystemLP::resolve, which pins every integer to its
  // MIP-optimal value and RELAXES integrality to recover LP duals.  So after
  // the full resolve the status columns are continuous (`is_integer == false`)
  // but pinned to their integral MIP values.  That is correct production
  // behaviour — apply_mip_start's own restore-integrality is exercised by the
  // unit tests in test_mip_start.cpp; here we assert the end-to-end VALUE
  // integrality, which is the property the MIP-start hook must preserve.
  const auto sol = lp.get_col_sol();
  for (const auto& [block_uid, col] : *status_cols) {
    const double v = sol[static_cast<std::size_t>(static_cast<int>(col))];
    const double rounded = (v >= 0.5) ? 1.0 : 0.0;
    CHECK(v == doctest::Approx(rounded).epsilon(1e-4));
  }

  // The block-0 unit is off (zero demand), blocks 1 & 2 on (demand 60).
  const auto& col_map = lp.col_name_map();
  auto find_status = [&](Uid block_uid) -> std::optional<ColIndex>
  {
    for (const auto& [name, idx] : col_map) {
      if (name.starts_with("commitment_")
          && name.contains(std::string(CommitmentLP::StatusName) + "_")
          && name.size() >= 2
          && name.substr(name.size() - 2)
              == std::string("_") + std::to_string(block_uid))
      {
        return idx;
      }
    }
    return std::nullopt;
  };
  const auto u0 = find_status(Uid {0});
  const auto u1 = find_status(Uid {1});
  const auto u2 = find_status(Uid {2});
  REQUIRE(u0.has_value());
  REQUIRE(u1.has_value());
  REQUIRE(u2.has_value());
  CHECK(sol[static_cast<std::size_t>(static_cast<int>(*u0))]
        == doctest::Approx(0.0).epsilon(1e-4));
  CHECK(sol[static_cast<std::size_t>(static_cast<int>(*u1))]
        == doctest::Approx(1.0).epsilon(1e-4));
  CHECK(sol[static_cast<std::size_t>(static_cast<int>(*u2))]
        == doctest::Approx(1.0).epsilon(1e-4));
}

TEST_CASE(  // NOLINT
    "MipStart integration: scip_repair stage is wired end-to-end and "
    "self-skips "
    "cleanly without the SCIP plugin")
{
  SolverRegistry& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP-start integration test — no MIP solver available");
    return;
  }
  // The OPTIONAL scip_repair STAGE composes on top of warmstart: round +
  // electric rules produce the candidate, then apply_mip_start hands it to
  // scip_repair_candidate, which receives the monolithic flat-LP snapshot.
  // With the SCIP plugin absent the stage self-skips at the has_solver("scip")
  // guard (keeping the round+rules candidate) and the normal MIP solve still
  // reaches the optimum; with SCIP present it repairs + injects.  Either way
  // the wiring (flag → stage → flat_lp threading) is exercised and the optimum
  // is unchanged.
  System system = make_commitment_system();

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  poptions.lp_matrix_options.col_with_names = true;
  poptions.lp_matrix_options.col_with_name_map = true;
  poptions.monolithic_options.mip_start.emplace();
  poptions.monolithic_options.mip_start->enabled = true;
  poptions.monolithic_options.mip_start->scip_repair.enabled = true;

  PlanningOptionsLP options(std::move(poptions));
  SimulationLP simulation_lp(three_block_simulation, options);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  SystemLP system_lp(system, simulation_lp, flat_opts);

  auto&& lp = system_lp.linear_interface();
  // The generator's precondition: the monolithic flat LP is retained.
  REQUIRE(lp.flat_lp_snapshot() != nullptr);

  const auto result = system_lp.resolve(SolverOptions {.log_level = 0});
  REQUIRE(result.has_value());
  REQUIRE(lp.is_optimal());

  // Same optimum as warmstart: blocks (0, 1, 2) → status (0, 1, 1).
  const auto sol = lp.get_col_sol();
  const auto& comm_lps = system_lp.elements<CommitmentLP>();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto* status_cols =
      comm_lps.front().find_status_cols(scenario_lp, stage_lp);
  REQUIRE(status_cols != nullptr);
  for (const auto& [block_uid, col] : *status_cols) {
    const double v = sol[static_cast<std::size_t>(static_cast<int>(col))];
    const double rounded = (v >= 0.5) ? 1.0 : 0.0;
    CHECK(v == doctest::Approx(rounded).epsilon(1e-4));
  }
}

}  // namespace mip_start_integration_test
