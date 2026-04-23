// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_error_lp.cpp
 * @brief     Doctest fixture pinning the two SDDP forward-pass
 *            "unrecoverable infeasibility" exit branches.
 * @date      2026-04-21
 *
 * Exercises the std::unexpected(Error{SolverError, ...}) return paths
 * currently at source/sddp_forward_pass.cpp:393-429:
 *
 *   Branch A — phase_index == 0 with infeasibility.  elastic_solve()
 *              short-circuits to std::nullopt because there is no
 *              predecessor phase on which to install a feasibility cut.
 *
 *   Branch B — phase_index > 0 but the state-variable-relaxed clone is
 *              itself infeasible (e.g. demand balance cannot be met even
 *              after relaxing every state-var bound).
 *
 * Both branches log a WARN line ("elastic filter produced no feasibility
 * cut ...") and return a SolverError from forward_pass.  The single-scene
 * fixtures below propagate that up through run_forward_pass_all_scenes
 * (scenes_solved == 0  ->  "SDDP: all scenes infeasible in forward pass")
 * out of SDDPMethod::solve().
 *
 * We pin only what is observable today:
 *   - solve() returns !has_value() with ErrorCode::SolverError.
 *   - the per-scene WARN log contains the branch-specific reason,
 *     captured via the LogCapture ring-buffer sink.
 *
 * Upcoming plan item E-1 will add error_scene_<S>_phase_<P>.lp writes on
 * these paths; that assertion will be grafted onto these tests (see the
 * TODO markers below).
 */

#include <filesystem>
#include <string>
#include <system_error>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "fixture_helpers.hpp"
#include "log_capture.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

/// Minimal single-phase planning whose LP is unconditionally infeasible.
///
/// A single `forced = true` demand of 100 MW is paired with a generator
/// of capacity 10 MW.  `forced` demands bypass the failure-variable
/// branch in `DemandLP::add_to_lp`, so the bus balance row is
///   load = 100   and   load <= generator_output <= 10
/// with no slack — the LP cannot be satisfied at any cost.  Because
/// phase 0 is infeasible from the outset, `elastic_solve()` returns
/// std::nullopt immediately (no predecessor phase to cut on) and
/// forward_pass hits Branch A at source/sddp_forward_pass.cpp:393-429.
/// Uses 2 phases because SDDP validation rejects num_phases < 2.
auto make_phase0_infeasible_planning() -> Planning
{
  constexpr int num_phases = 2;
  constexpr int blocks_per_phase = 2;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
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
          .name = "tiny_gen",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 10.0,  // far below forced demand
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d_forced",
          .bus = Uid {1},
          .forced = true,  // no failure variable on the bus balance row
          .capacity = 100.0,
      },
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;  // ignored when demand is forced
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_phase0_infeas",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
          },
  };
}

// Branch B uses `make_forced_infeasibility_planning()` from
// `sddp_helpers.hpp` — a two-reservoir forced-flow fixture whose
// phase-1 state-var-relaxed clone is driven infeasible by `multi_cut`
// bound rows at iter 1.  See `test_sddp_fcut_audit.cpp:632-634` for
// the comment that originally documented this behaviour.

// ═══════════════════════════════════════════════════════════════════════════
// Branch A — phase_index == 0 with infeasibility
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP forward: unrecoverable infeasibility at phase 0 returns error")
{
  const auto log_dir =
      std::filesystem::temp_directory_path() / "gtopt_sddp_err_lp_branchA";
  std::error_code ec;
  std::filesystem::remove_all(log_dir, ec);
  std::filesystem::create_directories(log_dir, ec);

  auto planning = make_phase0_infeasible_planning();
  // LinearInterface::write_lp refuses to write when row names aren't
  // available; pass col/row name flags at planning-LP build time so
  // the E-1 error-LP dump has labels to emit.
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;  // one iteration is all we need
  sddp_opts.log_directory = log_dir.string();
  sddp_opts.enable_api = false;  // keep the test hermetic

  gtopt::test::LogCapture logs;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();

  // Single-scene: the infeasible scene sinks the whole solve.  The
  // aggregate error produced by run_forward_pass_all_scenes when
  // scenes_solved == 0 is a SolverError.
  REQUIRE_FALSE(results.has_value());
  CHECK(results.error().code == ErrorCode::SolverError);

  // The per-scene WARN line (source/sddp_forward_pass.cpp:407) carries
  // the branch-specific reason — that is what pins Branch A here.
  CAPTURE(results.error().message);
  CHECK(logs.contains("elastic filter produced no feasibility cut"));
  CHECK(logs.contains("no predecessor phase to cut on"));

  // E-1: error LP is saved to <log_dir>/error_s<scene_uid>_p<phase_uid>
  //      _i<iteration_index>.lp.  The fixture builds its scenes without
  //      explicit UIDs, so scene_uid=0; phase UIDs are 1-based via
  //      `make_single_stage_phases`.  Branch A fires at phase 0
  //      (phase_uid=1) at iteration 0.
  const auto error_lp_path = log_dir / "error_s0_p1_i0.lp";
  CHECK(std::filesystem::exists(error_lp_path));
  CHECK(logs.contains("[LP saved to"));
}

// ═══════════════════════════════════════════════════════════════════════════
// Branch B — infeasibility elsewhere than phase 0 where state-variable
// relaxation alone cannot produce a feasible elastic cut.  Uses the
// shared `make_forced_infeasibility_planning` fixture (same one the
// fcut_audit suite drives).  This test is a **common-observable**
// characterization: it pins the outer behaviour (Error returned, WARN
// emitted) without committing to whether the exact exit path is A or
// B — both depend on when multi_cut's bound rows fire relative to the
// infeasibility, and that interplay is the subject of the follow-up
// E-1 work.  Any unrecoverable-infeasibility exit must end up in this
// assertion set.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP forward: unrecoverable infeasibility with state-variable fixture "
    "returns error")
{
  const auto log_dir =
      std::filesystem::temp_directory_path() / "gtopt_sddp_err_lp_branchB";
  std::error_code ec;
  std::filesystem::remove_all(log_dir, ec);
  std::filesystem::create_directories(log_dir, ec);

  auto planning = make_forced_infeasibility_planning();
  // write_lp requires row names — enable at LP build time.
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.log_directory = log_dir.string();
  sddp_opts.enable_api = false;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;

  gtopt::test::LogCapture logs;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();

  // Post-D3 slack-bound swap fix (2026-04-23): the
  // `make_forced_infeasibility_planning` fixture is genuinely
  // unrecoverable when D3 finite slack bounds are installed
  // correctly.  Phase 0 drains the reservoir to eini=0 via cheap
  // hydro dispatch (α=0 bootstrap), leaving phase 1 with 4 hm³ of
  // inflow but a mandatory 8 hm³ discharge → infeasible.  The
  // elastic filter installs an fcut on phase 0, but phase 0 is the
  // boundary phase (no predecessor to recurse on), so recovery
  // stops.  This matches PLP's behavior when reaching the boundary
  // stage of an unrecoverable case.
  //
  // An intermediate 2026-04-22 commit observed a misleading
  // "success" caused by a latent D3 sign-bug that made slacks
  // degenerate → elastic_filter_solve returned `nullopt` → no
  // cuts were ever installed → the forward pass ran oblivious.
  // That was corrected in the 2026-04-23 D3 swap fix.
  REQUIRE_FALSE(results.has_value());
  CHECK(results.error().code == ErrorCode::SolverError);
  CHECK(logs.contains("elastic filter produced no feasibility cut"));
}

}  // namespace
