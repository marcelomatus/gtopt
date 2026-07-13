// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_mip_start_roundtrip.cpp
 * @brief     Per-solver round-trip tests for the MIP-start loading mechanism:
 *            solve a tiny UC MIP, capture the COMPLETE optimal solution,
 *            reload it as a MIP start, and verify it is accepted and
 *            re-solves to the same optimum.
 * @date      2026-07-13
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * Invariant under test (docs/tasks/mipstart-roundtrip-prototype.md):
 * reloading a complete, consistent, OPTIMAL solution as a MIP start MUST be
 * accepted by every MIP-capable backend and re-solve to the same optimum
 * with ~zero extra work.  On the real CEN UC MIP this round trip failed
 * ("No solution found from 1 MIP starts") for two composing reasons:
 *   1. the CPLEX backend filtered EVERY start down to a sparse integer-only
 *      start, and CPLEX cannot check the feasibility of a partial start —
 *      CHECKFEAS discards it even when the integers are the exact optimum;
 *   2. `FileMipStart` overlaid the dumped optimal integers onto the
 *      LP-RELAXATION continuous values — a mutually inconsistent start.
 * The fixes: `check_feasibility`/`no_check` starts go to CPLEX DENSE
 * (complete), and the dump/replay persists the COMPLETE solution.
 *
 * Fixture: 2-generator / 2-block unit commitment (closed form).
 *   Gen A: Pmin 20, Pmax 60,  fixed cost 100/on-block, variable cost 10
 *   Gen B: Pmin 40, Pmax 100, fixed cost  40/on-block, variable cost 50
 *   Demand: block 1 = 50, block 2 = 90.
 * Columns per block t: uA_t, uB_t binary; pA_t, pB_t continuous.
 * Rows per block t:
 *   balance : pA + pB == d_t
 *   capX    : pX - Pmax_X uX <= 0
 *   minX    : pX - Pmin_X uX >= 0
 * MIP optimum (unique): u = (1,0,1,1), p = (50,0,50,40), obj = 3240.
 * LP relaxation (unique, fractional): uA1=5/6, uB1=0, uA2=1, uB2=0.3,
 * p = (50,0,60,30), obj = 8386/3 ≈ 2795.33 — the relaxed continuous
 * dispatch (pA2=60, pB2=30) DIFFERS from the MIP-optimal one (50, 40),
 * so any integers-over-relaxation overlay is provably inconsistent.
 */

#include <cctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

constexpr double kOptObj = 3240.0;
constexpr double kRelaxObj = 8386.0 / 3.0;
constexpr std::size_t kNumCols = 8;

struct TinyUc
{
  // column indices, blocks 1..2
  ColIndex ua1 {}, ub1 {}, pa1 {}, pb1 {};
  ColIndex ua2 {}, ub2 {}, pa2 {}, pb2 {};
};

/// Build the 2-gen / 2-block UC fixture into `lp` (8 cols, 10 rows).
TinyUc build_tiny_uc(LinearInterface& lp)
{
  TinyUc m;

  auto add_block =
      [&lp](
          double demand, ColIndex& ua, ColIndex& ub, ColIndex& pa, ColIndex& pb)
  {
    ua = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 100.0,
    });
    ub = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 40.0,
    });
    pa = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 10.0,
    });
    pb = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 50.0,
    });
    lp.set_integer(ua);
    lp.set_integer(ub);

    SparseRow balance;
    balance[pa] = 1.0;
    balance[pb] = 1.0;
    balance.equal(demand);
    (void)lp.add_row(balance);

    SparseRow cap_a;
    cap_a[pa] = 1.0;
    cap_a[ua] = -60.0;
    cap_a.less_equal(0.0);
    (void)lp.add_row(cap_a);

    SparseRow min_a;
    min_a[pa] = 1.0;
    min_a[ua] = -20.0;
    min_a.greater_equal(0.0);
    (void)lp.add_row(min_a);

    SparseRow cap_b;
    cap_b[pb] = 1.0;
    cap_b[ub] = -100.0;
    cap_b.less_equal(0.0);
    (void)lp.add_row(cap_b);

    SparseRow min_b;
    min_b[pb] = 1.0;
    min_b[ub] = -40.0;
    min_b.greater_equal(0.0);
    (void)lp.add_row(min_b);
  };

  add_block(50.0, m.ua1, m.ub1, m.pa1, m.pb1);
  add_block(90.0, m.ua2, m.ub2, m.pa2, m.pb2);
  return m;
}

/// Cold MIP solve.  `initial_solve` + `resolve`: on CBC `initial_solve` only
/// solves the LP relaxation (branch-and-bound lives in `resolve()` — see
/// OsiSolverBackend::initial_solve), and on every other backend the extra
/// resolve is a cheap warm re-solve of the same MIP.
void cold_solve(LinearInterface& lp, const SolverOptions& opts = {})
{
  REQUIRE(lp.initial_solve(opts).has_value());
  REQUIRE(lp.resolve(opts).has_value());
  REQUIRE(lp.is_optimal());
}

/// Verify a dense raw solution vector is the unique MIP optimum of the
/// fixture: u = (1,0,1,1), p = (50,0,50,40).
void check_is_mip_optimum(const TinyUc& m, std::span<const double> sol)
{
  const auto at = [&sol](ColIndex c)
  { return sol[static_cast<std::size_t>(static_cast<int>(c))]; };
  CHECK(at(m.ua1) == doctest::Approx(1.0));
  CHECK(at(m.ub1) == doctest::Approx(0.0));
  CHECK(at(m.pa1) == doctest::Approx(50.0));
  CHECK(at(m.pb1) == doctest::Approx(0.0));
  CHECK(at(m.ua2) == doctest::Approx(1.0));
  CHECK(at(m.ub2) == doctest::Approx(1.0));
  CHECK(at(m.pa2) == doctest::Approx(50.0));
  CHECK(at(m.pb2) == doctest::Approx(40.0));
}

/// Read a whole text file (a solver native log / a dump) into a string.
std::string slurp(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// CPLEX prints the MIP-start processing outcome at the head of every
/// mipopt: `1 of 1 MIP starts provided solutions.` on acceptance vs
/// `Warning:  No solution found from 1 MIP starts.` on rejection.  This is
/// the sharp, official acceptance signal — the objective alone cannot
/// discriminate (a rejected start just solves cold to the same optimum).
constexpr std::string_view kCplexAccepted = "MIP starts provided solutions";
constexpr std::string_view kCplexRejected = "No solution found from";

/// SCIP prints the trusted-start `SCIPtrySol` outcome into its native log
/// (emitted by the scip plugin's `scip_install_mip_start`): the analog of the
/// CPLEX acceptance line, and just as sharp — a rejected start still solves
/// cold to the same optimum, so only the log discriminates.
constexpr std::string_view kScipAccepted = "gtopt MIP start accepted";
constexpr std::string_view kScipRejected = "gtopt MIP start rejected";

/// RAII: route the backend's native log to `<base>.log`, removing a stale
/// file first (the backend appends).
struct NativeLog
{
  std::filesystem::path log_path;

  explicit NativeLog(LinearInterface& li, const std::string& tag)
      : log_path(std::filesystem::temp_directory_path()
                 / ("gtopt_mipstart_rt_" + tag))
  {
    std::filesystem::remove(with_ext());
    li.set_log_file(log_path.string());
  }

  [[nodiscard]] std::filesystem::path with_ext() const
  {
    auto p = log_path;
    p += ".log";
    return p;
  }

  [[nodiscard]] std::string text() const { return slurp(with_ext()); }
};

/// Solver options that turn the backend's native file logging on (the
/// destination comes from `LinearInterface::set_log_file`).
[[nodiscard]] SolverOptions native_log_opts()
{
  SolverOptions opts;
  opts.log_level = 1;
  opts.log_mode = SolverLogMode::detailed;
  return opts;
}

/// Sharp per-backend acceptance check on the captured native log, for the
/// backends whose log carries an explicit MIP-start processing outcome
/// (cplex, scip).  Other backends expose no such log line — for them the
/// objective + optimality assertions in the caller remain the check.
void check_native_log_accepted(const std::string& name, const NativeLog& log)
{
  if (name == "cplex") {
    const auto text = log.text();
    CAPTURE(text);
    CHECK(text.contains(kCplexAccepted));
    CHECK_FALSE(text.contains(kCplexRejected));
  } else if (name == "scip") {
    const auto text = log.text();
    CAPTURE(text);
    CHECK(text.contains(kScipAccepted));
    CHECK_FALSE(text.contains(kScipRejected));
  }
}

}  // namespace

TEST_CASE("mip_start roundtrip - fixture sanity per MIP plugin")  // NOLINT
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  for (const auto& name : solvers) {
    CAPTURE(name);
    LinearInterface lp(name);
    const auto m = build_tiny_uc(lp);

    cold_solve(lp);
    CHECK(lp.get_obj_value() == doctest::Approx(kOptObj).epsilon(1e-6));
    check_is_mip_optimum(m, lp.get_col_sol_raw());

    // Relaxation is fractional and its continuous dispatch differs from the
    // MIP optimum — the property that makes the round trip non-trivial (an
    // integers-over-relaxation overlay is NOT a feasible MIP solution).
    // Checked on a FRESH interface: CBC's branch-and-bound leaves fixed
    // bounds behind on the solved model, so relaxing in place is not a
    // clean relaxation there.
    LinearInterface rlp(name);
    (void)build_tiny_uc(rlp);
    (void)rlp.relax_integers();
    REQUIRE(rlp.initial_solve().has_value());
    REQUIRE(rlp.is_optimal());
    CHECK(rlp.get_obj_value() == doctest::Approx(kRelaxObj).epsilon(1e-6));
    const auto rsol = rlp.get_col_sol_raw();
    const auto at = [&rsol](ColIndex c)
    { return rsol[static_cast<std::size_t>(static_cast<int>(c))]; };
    CHECK(at(m.ua1) == doctest::Approx(5.0 / 6.0));
    CHECK(at(m.ub2) == doctest::Approx(0.3));
    CHECK(at(m.pa2) == doctest::Approx(60.0));
    CHECK(at(m.pb2) == doctest::Approx(30.0));
  }
}

TEST_CASE(  // NOLINT
    "mip_start roundtrip - complete optimal start is accepted per MIP plugin")
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  for (const auto& name : solvers) {
    CAPTURE(name);

    // 1. Cold solve: capture the COMPLETE optimal solution (integer AND
    //    continuous columns) plus the basis when the backend exposes one.
    LinearInterface cold(name);
    const auto m = build_tiny_uc(cold);
    cold_solve(cold);
    const double obj_cold = cold.get_obj_value();
    REQUIRE(obj_cold == doctest::Approx(kOptObj).epsilon(1e-6));
    const auto raw = cold.get_col_sol_raw();
    const std::vector<double> sol(raw.begin(), raw.end());
    REQUIRE(sol.size() == kNumCols);
    const auto basis = cold.get_basis();  // nullopt after a MIP on most

    // 2. Fresh interface, SAME model, NO cold solve: inject the complete
    //    solution under the strictest effort — an exact optimum is
    //    feasible, so a working loading path MUST be accepted.
    LinearInterface warm(name);
    (void)build_tiny_uc(warm);
    const NativeLog log(warm, "accept_" + name);
    REQUIRE(warm.set_mip_start(sol, MipStartEffort::check_feasibility));
    if (basis.has_value()) {
      (void)warm.set_basis(*basis);  // best-effort root warm start
    }
    REQUIRE(warm.resolve(native_log_opts()).has_value());
    REQUIRE(warm.is_optimal());
    CHECK(warm.get_obj_value() == doctest::Approx(obj_cold).epsilon(1e-6));
    check_is_mip_optimum(m, warm.get_col_sol_raw());

    // 3. Sharp acceptance assertion on the backends whose native log carries
    //    the official MIP-start processing outcome (cplex, scip).  Rejection
    //    re-solves cold to the same objective, so only the log discriminates
    //    — this is the CEN failure signature.
    check_native_log_accepted(name, log);
  }
}

TEST_CASE(  // NOLINT
    "mip_start roundtrip - perturbed complete start is repaired per MIP "
    "plugin")
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  for (const auto& name : solvers) {
    CAPTURE(name);

    LinearInterface cold(name);
    const auto m = build_tiny_uc(cold);
    cold_solve(cold);
    const auto raw = cold.get_col_sol_raw();
    std::vector<double> start(raw.begin(), raw.end());
    REQUIRE(start.size() == kNumCols);

    // Flip ONE binary: uA2 1→0 leaves a fixed-integer-feasible commitment
    // (B alone covers block 2) whose continuous entries are now stale — a
    // "near-feasible complete start" the backend must repair back to the
    // true optimum, not reject.
    start[static_cast<std::size_t>(static_cast<int>(m.ua2))] = 0.0;

    LinearInterface warm(name);
    (void)build_tiny_uc(warm);
    REQUIRE(warm.set_mip_start(start, MipStartEffort::repair));
    REQUIRE(warm.resolve().has_value());
    REQUIRE(warm.is_optimal());
    CHECK(warm.get_obj_value() == doctest::Approx(kOptObj).epsilon(1e-6));
    check_is_mip_optimum(m, warm.get_col_sol_raw());
  }
}

TEST_CASE(  // NOLINT
    "mip_start roundtrip - dump file replays through apply_mip_start per MIP "
    "plugin")
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  for (const auto& name : solvers) {
    CAPTURE(name);
    const auto dump_path = std::filesystem::temp_directory_path()
        / ("gtopt_mipstart_rt_" + name + ".dump");
    std::filesystem::remove(dump_path);

    // Solve cold and dump the COMPLETE solution.
    LinearInterface src(name);
    const auto m = build_tiny_uc(src);
    cold_solve(src);
    REQUIRE(dump_integer_solution(src, dump_path.string()).has_value());

    // The dump must carry EVERY column (integer and continuous), not just
    // the integers — a complete dump is what makes the replayed start
    // consistent.
    {
      const auto text = slurp(dump_path);
      CAPTURE(text);
      CHECK(text.contains("ncols 8"));
      CHECK(text.contains("nint 4"));
      std::istringstream in(text);
      std::string line;
      std::size_t value_lines = 0;
      while (std::getline(in, line)) {
        if (!line.empty() && (std::isdigit(line.front()) != 0)) {
          ++value_lines;
        }
      }
      CHECK(value_lines == kNumCols);
    }

    SUBCASE("full pipeline (relaxation + file replay + inject)")
    {
      LinearInterface dst(name);
      (void)build_tiny_uc(dst);
      const NativeLog log(dst, "file_" + name);

      MipStartOptions ms;
      ms.enabled = true;
      ms.from_file = dump_path.string();
      ms.inject.effort = MipStartEffort::check_feasibility;
      const auto report = apply_mip_start(dst, SolverOptions {}, ms);
      REQUIRE(report.has_value());
      CHECK(report->relaxation_solved);
      CHECK(report->relaxation_feasible);
      CHECK(report->injected);
      CHECK(report->source == "file");

      REQUIRE(dst.resolve(native_log_opts()).has_value());
      REQUIRE(dst.is_optimal());
      CHECK(dst.get_obj_value() == doctest::Approx(kOptObj).epsilon(1e-6));
      check_is_mip_optimum(m, dst.get_col_sol_raw());
      check_native_log_accepted(name, log);
    }

    SUBCASE("skip_relaxation fast path (no throwaway relaxation solve)")
    {
      LinearInterface dst(name);
      (void)build_tiny_uc(dst);
      const NativeLog log(dst, "skip_" + name);

      MipStartOptions ms;
      ms.enabled = true;
      ms.from_file = dump_path.string();
      ms.skip_relaxation = true;
      ms.inject.effort = MipStartEffort::check_feasibility;
      const auto report = apply_mip_start(dst, SolverOptions {}, ms);
      REQUIRE(report.has_value());
      CHECK_FALSE(report->relaxation_solved);
      CHECK(report->injected);
      CHECK(report->source == "file");

      REQUIRE(dst.resolve(native_log_opts()).has_value());
      REQUIRE(dst.is_optimal());
      CHECK(dst.get_obj_value() == doctest::Approx(kOptObj).epsilon(1e-6));
      check_is_mip_optimum(m, dst.get_col_sol_raw());
      check_native_log_accepted(name, log);
    }

    SUBCASE("file generator reconstructs the dump without a relaxation")
    {
      // Destination NEVER solved: `get_col_sol_raw()` has no relaxation
      // primal.  The generator must still produce the dumped solution
      // (pre-fix it silently returned nullopt here, and the pipeline fell
      // back to the round candidate — the CEN silent-fallback bug).
      LinearInterface dst(name);
      (void)build_tiny_uc(dst);
      std::vector<int> int_cols;
      for (int i = 0; i < static_cast<int>(kNumCols); ++i) {
        if (dst.is_integer(ColIndex {i})) {
          int_cols.push_back(i);
        }
      }
      REQUIRE(int_cols.size() == 4);

      MipStartOptions ms;
      ms.from_file = dump_path.string();
      const SolverOptions relax_opts;
      MipStartContext ctx {
          .li = dst,
          .relax_opts = relax_opts,
          .int_cols = int_cols,
          .opts = ms,
          .commitments = {},
          .injections = {},
      };
      auto gen = make_mip_start_generator(ms);
      REQUIRE(gen != nullptr);
      CHECK(gen->name() == "file");
      const auto start = gen->generate(ctx);
      REQUIRE(start.has_value());
      REQUIRE(start->size() == kNumCols);
      check_is_mip_optimum(m, *start);
    }

    std::filesystem::remove(dump_path);
  }
}

namespace
{

// ── SystemLP-level fixture: a 1-bus / 1-generator / 3-block commitment MIP
// (the same shape as test_mip_start_integration.cpp).  Demand (0, 60, 60)
// with startup cost ⇒ status binaries (0, 1, 1).

const Simulation rt_three_block_simulation = {
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

[[nodiscard]] System make_rt_commitment_system()
{
  System system;
  system.name = "MipStartRoundtrip";
  system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
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

/// Build + resolve one SystemLP over the commitment fixture with the given
/// MIP-start options; return the optimal objective.
[[nodiscard]] double resolve_rt_commitment_system(
    const std::string& solver_name, const MipStartOptions& ms)
{
  System system = make_rt_commitment_system();

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  poptions.lp_matrix_options.solver_name = solver_name;
  poptions.monolithic_options.mip_start = ms;

  PlanningOptionsLP options(std::move(poptions));
  SimulationLP simulation_lp(rt_three_block_simulation, options);
  SystemLP system_lp(system, simulation_lp, LpMatrixOptions {});

  const auto result = system_lp.resolve(SolverOptions {.log_level = 0});
  REQUIRE(result.has_value());
  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.is_optimal());
  return lp.get_obj_value();
}

}  // namespace

TEST_CASE(  // NOLINT
    "mip_start roundtrip - SystemLP dump_file to from_file round trip")
{
  // The exact path that broke on the CEN UC MIP: solve once dumping the
  // solution via `mip_start.dump_file` (SystemLP::resolve hook), then solve
  // a second, identical SystemLP replaying it via `mip_start.from_file`
  // under the strictest inject effort — the replayed optimum must be
  // accepted and land the same objective.
  const auto solver_name = gtopt::solver_test::first_mip_solver();
  if (solver_name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(solver_name);

  const auto dump_path = std::filesystem::temp_directory_path()
      / ("gtopt_mipstart_rt_systemlp_" + solver_name + ".dump");
  std::filesystem::remove(dump_path);

  // Run 1 — cold solve, dump the complete solution.
  MipStartOptions dump_opts;
  dump_opts.dump_file = dump_path.string();
  const double obj_cold = resolve_rt_commitment_system(solver_name, dump_opts);
  REQUIRE(std::filesystem::exists(dump_path));
  {
    const auto text = slurp(dump_path);
    CAPTURE(text);
    CHECK(text.contains("ncols"));
    CHECK(text.contains("nint"));
  }

  // Run 2 — replay the dump as the MIP start (full pipeline).
  MipStartOptions replay_opts;
  replay_opts.enabled = true;
  replay_opts.from_file = dump_path.string();
  replay_opts.inject.effort = MipStartEffort::check_feasibility;
  const double obj_warm =
      resolve_rt_commitment_system(solver_name, replay_opts);
  CHECK(obj_warm == doctest::Approx(obj_cold).epsilon(1e-6));

  // Run 3 — replay without the throwaway Stage-0 relaxation solve.
  MipStartOptions skip_opts = replay_opts;
  skip_opts.skip_relaxation = true;
  const double obj_skip = resolve_rt_commitment_system(solver_name, skip_opts);
  CHECK(obj_skip == doctest::Approx(obj_cold).epsilon(1e-6));

  std::filesystem::remove(dump_path);
}
