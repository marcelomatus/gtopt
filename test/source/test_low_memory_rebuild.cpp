/**
 * @file      test_low_memory_rebuild.cpp
 * @brief     Tests for LowMemoryMode::rebuild — lazy, snapshot-free LP rebuild
 * @date      2026-04-16
 * @copyright BSD-3-Clause
 *
 * Covers the rebuild-mode invariants:
 *  - SystemLP constructor skips create_lp() in rebuild mode (the
 *    LinearInterface stays unloaded; no FlatLinearProblem is held).
 *  - SystemLP::ensure_lp_built() re-runs the full assembly + load_flat
 *    on the first call and on every call after a release_backend().
 *  - The persistent SDDP state (m_dynamic_cols_ / m_active_cuts_ /
 *    m_base_numrows_) survives every rebuild so SDDP convergence is
 *    unaffected.
 *  - LinearInterface::release_backend() under rebuild keeps m_snapshot_
 *    empty (no FlatLinearProblem retained between solves).
 *  - LinearInterface::ensure_backend() short-circuits silently under
 *    rebuild — only SystemLP owns the rebuild path.
 *  - LinearInterface::reconstruct_backend() must never be reached under
 *    rebuild (debug-asserted; covered by the SystemLP path test).
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_system.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
/// Build a minimal feasible single-bus, single-stage System + Simulation
/// pair suitable for stress-testing SystemLP under low_memory modes.
struct MiniWorld
{
  Simulation simulation;
  System system;
};

[[nodiscard]] MiniWorld make_mini_world()
{
  return MiniWorld {
      .simulation =
          Simulation {
              .block_array =
                  {
                      {
                          .uid = Uid {1},
                          .duration = 1,
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
          },
      .system =
          System {
              .name = "MINI",
              .bus_array =
                  {
                      {
                          .uid = Uid {1},
                          .name = "b1",
                      },
                  },
              .demand_array =
                  {
                      {
                          .uid = Uid {1},
                          .name = "d1",
                          .bus = Uid {1},
                          .forced = true,
                          .capacity = 100.0,
                      },
                  },
              .generator_array =
                  {
                      {
                          .uid = Uid {1},
                          .name = "g1",
                          .bus = Uid {1},
                          .gcost = 50.0,
                          .capacity = 1000.0,
                      },
                  },
          },
  };
}
}  // namespace

// ── LinearInterface low-level rebuild-mode behavior ────────────────────────

TEST_CASE(  // NOLINT
    "LinearInterface — rebuild mode: release_backend does not retain snapshot")
{
  // Build a tiny LP and load it into a LinearInterface configured for
  // rebuild mode.  After release_backend() the snapshot must remain empty
  // (rebuild explicitly avoids holding a FlatLinearProblem) — confirming
  // the only way back to a live backend is SystemLP::ensure_lp_built().
  LinearProblem lp;
  const auto c = lp.add_col({
      .lowb = 0.0,
      .uppb = 5.0,
      .cost = 1.0,
  });
  const auto r = lp.add_row({
      .lowb = 1.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c, 1.0);

  LinearInterface li;
  li.set_low_memory(LowMemoryMode::rebuild);
  auto flat = lp.flatten({});
  li.load_flat(flat);

  CHECK(li.low_memory_mode() == LowMemoryMode::rebuild);
  CHECK_FALSE(li.is_backend_released());
  CHECK_FALSE(li.has_snapshot_data());

  auto solve = li.resolve();
  REQUIRE(solve.has_value());
  CHECK(li.is_optimal());

  li.release_backend();
  CHECK(li.is_backend_released());
  CHECK_FALSE(li.has_backend());
  // The defining invariant of rebuild: NO snapshot is held after release.
  CHECK_FALSE(li.has_snapshot_data());
}

TEST_CASE(  // NOLINT
    "LinearInterface — rebuild mode: ensure_backend is a no-op")
{
  // ensure_backend() is the path snapshot/compress modes use to lazily
  // reconstruct from m_snapshot_.  Under rebuild there is no snapshot to
  // reconstruct from, so ensure_backend() must short-circuit silently —
  // only SystemLP::ensure_lp_built() owns the rebuild dispatch.
  LinearInterface li;
  li.set_low_memory(LowMemoryMode::rebuild);
  // Mimic SystemLP::ctor under rebuild: drop the default-constructed
  // backend so we are in the canonical "released, no LP loaded" state.
  li.mark_released();
  REQUIRE(li.is_backend_released());

  // Must not crash and must not flip the released flag — there is no
  // snapshot to reconstruct from, so the call short-circuits.
  li.set_warm_start_solution({}, {});  // routes through ensure_backend
  CHECK(li.is_backend_released());
  CHECK_FALSE(li.has_snapshot_data());
}

TEST_CASE(  // NOLINT
    "LinearInterface — take/restore dynamic cols and active cuts")
{
  LinearInterface li;
  li.set_low_memory(LowMemoryMode::rebuild);

  const SparseCol alpha {
      .uppb = 1e6,
      .cost = 0.0,
  };
  li.record_dynamic_col(alpha);
  li.record_dynamic_col(alpha);

  SparseRow cut;
  cut.lowb = 0.0;
  cut.uppb = SparseRow::DblMax;
  li.record_cut_row(cut);

  auto cols = li.take_dynamic_cols();
  auto cuts = li.take_active_cuts();
  CHECK(cols.size() == 2);
  CHECK(cuts.size() == 1);

  // After take_*, the internal vectors are empty (move semantics).
  CHECK(li.take_dynamic_cols().empty());
  CHECK(li.take_active_cuts().empty());

  li.restore_dynamic_cols(std::move(cols));
  li.restore_active_cuts(std::move(cuts));

  // Restored vectors are observable via a fresh take_*.
  CHECK(li.take_dynamic_cols().size() == 2);
  CHECK(li.take_active_cuts().size() == 1);
}

TEST_CASE(
    "LinearInterface — set_base_numrows / base_numrows accessor")  // NOLINT
{
  LinearInterface li;
  CHECK(li.base_numrows() == 0);
  li.set_base_numrows(42);
  CHECK(li.base_numrows() == 42);
}

// ── SystemLP rebuild-mode integration ──────────────────────────────────────

TEST_CASE(  // NOLINT
    "SystemLP — rebuild mode: ctor defers LP build, ensure_lp_built builds")
{
  // Verifies the central invariant: under rebuild the SystemLP shell
  // (collections + AMPL registries + flat_opts) is constructed eagerly,
  // but the FlatLinearProblem and the solver backend are NOT created
  // until the first ensure_lp_built() call (which the SDDP forward
  // pass / aperture pass triggers per task).  This is exactly what
  // skips the up-front "Build LP" loop under rebuild.
  auto world = make_mini_world();

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(world.simulation, options);

  LpMatrixOptions flat_opts;
  flat_opts.low_memory_mode = LowMemoryMode::rebuild;

  SystemLP system_lp(world.system, simulation_lp, flat_opts);

  // After construction: shell is live, backend is not.
  CHECK(system_lp.low_memory_mode() == LowMemoryMode::rebuild);
  CHECK(system_lp.is_backend_released());
  CHECK_FALSE(system_lp.linear_interface().has_backend());
  CHECK_FALSE(system_lp.linear_interface().has_snapshot_data());

  // First ensure_lp_built() runs the full assembly + load_flat.
  system_lp.ensure_lp_built();
  CHECK_FALSE(system_lp.is_backend_released());
  CHECK(system_lp.linear_interface().has_backend());
  // Rebuild mode never installs a snapshot, even after a build.
  CHECK_FALSE(system_lp.linear_interface().has_snapshot_data());

  // Solve to confirm the freshly built LP is correct.
  auto& li = system_lp.linear_interface();
  const auto solve = li.resolve();
  REQUIRE(solve.has_value());
  REQUIRE(li.is_optimal());
  // 1 block × 100 MW demand × 50 $/MWh = 5000 (descaled by scale_objective).
  CHECK(li.get_obj_value_physical() == doctest::Approx(5000.0));
}

TEST_CASE(  // NOLINT
    "SystemLP — rebuild mode: release/rebuild cycle preserves dynamic cols + "
    "cuts")
{
  // The rebuild path must replay m_dynamic_cols_ and m_active_cuts_ on
  // every fresh build — otherwise SDDP convergence breaks (alpha cols
  // would reappear with a different index, and accumulated cuts would
  // be lost between iterations).  This test exercises the same code
  // path SDDP hits between two consecutive iterations.
  auto world = make_mini_world();

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(world.simulation, options);

  LpMatrixOptions flat_opts;
  flat_opts.low_memory_mode = LowMemoryMode::rebuild;

  SystemLP system_lp(world.system, simulation_lp, flat_opts);
  system_lp.ensure_lp_built();

  auto& li = system_lp.linear_interface();
  const auto base_cols = li.get_numcols();
  const auto base_rows = li.get_numrows();

  // Simulate SDDP wiring: add an alpha column and one Benders cut, then
  // mark them so the rebuild path re-applies them.
  const SparseCol alpha {
      .lowb = 0.0,
      .uppb = 1e9,
      .cost = 1.0,
  };
  const auto alpha_idx = li.add_col(alpha);
  li.record_dynamic_col(alpha);
  li.save_base_numrows();
  const auto base_after_alpha = li.base_numrows();

  SparseRow cut;
  cut.lowb = -1.0;
  cut.uppb = SparseRow::DblMax;
  cut[alpha_idx] = 1.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  CHECK(li.get_numcols() == base_cols + 1);
  CHECK(li.get_numrows() == base_rows + 1);

  // Release (rebuild mode: drops backend + snapshot stays empty), then
  // re-build.  Persistent SDDP state must survive intact.
  system_lp.release_backend();
  CHECK(system_lp.is_backend_released());
  CHECK_FALSE(system_lp.linear_interface().has_snapshot_data());

  system_lp.ensure_lp_built();
  CHECK_FALSE(system_lp.is_backend_released());

  auto& nli = system_lp.linear_interface();
  CHECK(nli.get_numcols() == base_cols + 1);
  CHECK(nli.get_numrows() == base_rows + 1);
  CHECK(nli.base_numrows() == base_after_alpha);
}

TEST_CASE(  // NOLINT
    "SystemLP — rebuild mode: solve count grows with each rebuild")
{
  // Under rebuild, each ensure_lp_built() that follows a release_backend()
  // re-runs flatten() + load_flat() — so SolverStats.load_problem_calls
  // grows by one per cycle.  Snapshot/compress modes load_flat exactly
  // once at first reconstruction.  This counter is the canonical signal
  // that rebuild is in effect on a per-iteration basis.
  auto world = make_mini_world();

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(world.simulation, options);

  LpMatrixOptions flat_opts;
  flat_opts.low_memory_mode = LowMemoryMode::rebuild;

  SystemLP system_lp(world.system, simulation_lp, flat_opts);

  constexpr std::size_t n_cycles = 3;
  for (std::size_t i = 0; i < n_cycles; ++i) {
    system_lp.ensure_lp_built();
    auto solve = system_lp.linear_interface().resolve();
    REQUIRE(solve.has_value());
    REQUIRE(system_lp.linear_interface().is_optimal());
    system_lp.release_backend();
  }

  // Every rebuild calls load_flat once.  Expect at least n_cycles calls;
  // the implementation is free to do more (e.g. one extra during the
  // initial set_warm_start_solution path), but never fewer.
  CHECK(system_lp.solver_stats().load_problem_calls >= n_cycles);
}

TEST_CASE(  // NOLINT
    "SystemLP — rebuild parity: same objective as snapshot mode")
{
  // Sanity: rebuild and snapshot must produce the exact same optimal
  // objective on a deterministic LP.  If they ever diverge, the rebuild
  // path has lost or mis-replayed some piece of LP state.
  auto world = make_mini_world();

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(world.simulation, options);

  double obj_snapshot {};
  double obj_rebuild {};

  {
    LpMatrixOptions opts;
    opts.low_memory_mode = LowMemoryMode::snapshot;
    SystemLP sys(world.system, simulation_lp, opts);
    sys.ensure_lp_built();
    auto r = sys.linear_interface().resolve();
    REQUIRE(r.has_value());
    obj_snapshot = sys.linear_interface().get_obj_value_physical();
  }
  {
    SimulationLP sim2(world.simulation, options);
    LpMatrixOptions opts;
    opts.low_memory_mode = LowMemoryMode::rebuild;
    SystemLP sys(world.system, sim2, opts);
    sys.ensure_lp_built();
    auto r = sys.linear_interface().resolve();
    REQUIRE(r.has_value());
    obj_rebuild = sys.linear_interface().get_obj_value_physical();
  }

  CHECK(obj_rebuild == doctest::Approx(obj_snapshot));
}
