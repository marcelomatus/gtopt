// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_relax_and_fix.cpp
 * @brief     Relax-and-fix rolling-window integrality tests
 * @date      2026-07-14
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * Three properties of `solve_relax_and_fix` (relax_and_fix.hpp), each on a
 * tiny closed-form fixture (test_mip_start_roundtrip.cpp style):
 *
 *   1. The relax-and-fix solution is INTEGER-FEASIBLE and its objective is
 *      an upper bound on (here: equal to) the full-MIP optimum — 3 windows
 *      over a 6-block, 2-generator UC.
 *   2. A storage-like intertemporal constraint (SOC chain + end-horizon
 *      target) is NEVER violated by the window rolling: the model stays
 *      whole, so every window MIP sees the full future physics.
 *   3. A window spanning the whole horizon reproduces the plain MIP
 *      exactly (single window ⇒ the plain `resolve` path).
 *
 * Plus: the `make_relax_and_fix_cols` partition builder (pure, no solver)
 * and the SystemLP-level orchestration via
 * `monolithic_options.relax_and_fix_window`.
 */

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/relax_and_fix.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/system_lp.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

// ── Fixture 1: 2-generator / 6-block UC (three 2 h windows) ────────────────
//
// Per-block economics identical to the round-trip TinyUc fixture:
//   Gen A: Pmin 20, Pmax 60,  commit cost 100/on-block, variable cost 10
//   Gen B: Pmin 40, Pmax 100, commit cost  40/on-block, variable cost 50
// Demand alternates {50, 90} over six 1 h blocks.  No intertemporal rows —
// the MIP separates per block, so the closed-form optimum is
//   d=50 → uA only  (100 + 500        =  600)
//   d=90 → uA + uB  (140 + 500 + 2000 = 2640)
// and the 6-block total is 3 × 3240 = 9720 with u_A ≡ 1, u_B = (d == 90).
constexpr int rf6_blocks = 6;
constexpr double rf6_opt_obj = 9720.0;

struct Rf6Uc
{
  std::vector<ColIndex> ua;  // per block
  std::vector<ColIndex> ub;
  std::vector<ColIndex> pa;
  std::vector<ColIndex> pb;
};

[[nodiscard]] double rf6_demand(int t)
{
  return (t % 2 == 0) ? 50.0 : 90.0;
}

Rf6Uc build_rf6_uc(LinearInterface& lp)
{
  Rf6Uc m;
  for (int t = 0; t < rf6_blocks; ++t) {
    const auto ua = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 100.0,
    });
    const auto ub = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 40.0,
    });
    const auto pa = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 10.0,
    });
    const auto pb = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 50.0,
    });
    lp.set_integer(ua);
    lp.set_integer(ub);

    SparseRow balance;
    balance[pa] = 1.0;
    balance[pb] = 1.0;
    balance.equal(rf6_demand(t));
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

    m.ua.push_back(ua);
    m.ub.push_back(ub);
    m.pa.push_back(pa);
    m.pb.push_back(pb);
  }
  return m;
}

/// The window partition for the rf6 fixture: block t starts at hour t; both
/// generators' u columns share the block's start hour.
[[nodiscard]] std::vector<RelaxAndFixCol> rf6_window_cols(const Rf6Uc& m)
{
  std::vector<RelaxAndFixCol> cols;
  for (int t = 0; t < rf6_blocks; ++t) {
    cols.push_back({
        .col = static_cast<int>(m.ua[static_cast<std::size_t>(t)]),
        .start_hour = static_cast<double>(t),
    });
    cols.push_back({
        .col = static_cast<int>(m.ub[static_cast<std::size_t>(t)]),
        .start_hour = static_cast<double>(t),
    });
  }
  return cols;
}

[[nodiscard]] double rf_at(std::span<const double> sol, ColIndex c)
{
  return sol[static_cast<std::size_t>(static_cast<int>(c))];
}

/// Every listed binary is within tolerance of an integer value.
void rf_check_integral(std::span<const double> sol,
                       const std::vector<ColIndex>& cols)
{
  for (const auto& c : cols) {
    const double v = rf_at(sol, c);
    CHECK(std::abs(v - std::round(v)) <= 1e-6);
  }
}

/// Full-MIP optimum of the same fixture, cold-solved on a fresh interface.
[[nodiscard]] double rf6_cold_optimum(const std::string& solver_name)
{
  LinearInterface lp(solver_name);
  (void)build_rf6_uc(lp);
  REQUIRE(lp.initial_solve().has_value());
  REQUIRE(lp.resolve().has_value());
  REQUIRE(lp.is_optimal());
  return lp.get_obj_value();
}

// relax-and-fix's in-place rolling loop needs a backend that re-solves a
// mutated MIP correctly.  CBC's OsiCbc does not (solve_relax_and_fix gates it
// to a plain full MIP; the dedicated "CBC gates to the plain full MIP" case
// below covers that), so the windowing tests exclude cbc.
[[nodiscard]] std::vector<std::string> rf_windowing_solvers()
{
  auto solvers = gtopt::solver_test::exact_mip_solvers();
  std::erase(solvers, "cbc");
  return solvers;
}

/// First windowing-capable MIP solver, or "" when only cbc is available (CI).
[[nodiscard]] std::string rf_first_windowing_solver()
{
  const auto s = gtopt::solver_test::first_mip_solver();
  return s == "cbc" ? std::string {} : s;
}

// ── Fixture 2: committed generator + storage with an end-horizon target ────
//
// Six 1 h blocks.  Gen G: u binary (commit cost 20/on-block), p ∈
// [10u, 50u], variable cost 10.  Storage: ch, dis ∈ [0, 30], SOC chain
//   soc_1 = ch_1 − dis_1,   soc_t = soc_{t−1} + ch_t − dis_t   (η = 1),
// soc ∈ [0, 100] and the END-HORIZON target soc_6 = 20 (bounds [20, 20]).
// Demand {10, 10, 10, 10, 55, 55}: blocks 5–6 exceed Pmax, so the storage
// MUST be charged in earlier windows — an intertemporal decision a myopic
// per-window decomposition would miss, but the whole-model relax-and-fix
// window solves see (the SOC rows and the end target are always present).
// Total generation is demand + end target = 170 regardless of commitment
// (η = 1), so the optimum drops G in exactly the two blocks the SOC
// physics allows: obj = 4·20 + 10·170 = 1780.
constexpr int rfs_blocks = 6;
constexpr double rfs_end_target = 20.0;

struct RfsUc
{
  std::vector<ColIndex> ug;
  std::vector<ColIndex> pg;
  std::vector<ColIndex> ch;
  std::vector<ColIndex> dis;
  std::vector<ColIndex> soc;
};

[[nodiscard]] double rfs_demand(int t)
{
  return (t < 4) ? 10.0 : 55.0;
}

RfsUc build_rfs_uc(LinearInterface& lp)
{
  RfsUc m;
  for (int t = 0; t < rfs_blocks; ++t) {
    const auto ug = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = 20.0,
    });
    const auto pg = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 10.0,
    });
    const auto ch = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 30.0,
    });
    const auto dis = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 30.0,
    });
    const bool last = t + 1 == rfs_blocks;
    const auto soc = lp.add_col(SparseCol {
        .lowb = last ? rfs_end_target : 0.0,
        .uppb = last ? rfs_end_target : 100.0,
    });
    lp.set_integer(ug);

    SparseRow balance;  // pg + dis − ch == d_t
    balance[pg] = 1.0;
    balance[dis] = 1.0;
    balance[ch] = -1.0;
    balance.equal(rfs_demand(t));
    (void)lp.add_row(balance);

    SparseRow cap_g;  // pg − 50 u ≤ 0
    cap_g[pg] = 1.0;
    cap_g[ug] = -50.0;
    cap_g.less_equal(0.0);
    (void)lp.add_row(cap_g);

    SparseRow min_g;  // pg − 10 u ≥ 0
    min_g[pg] = 1.0;
    min_g[ug] = -10.0;
    min_g.greater_equal(0.0);
    (void)lp.add_row(min_g);

    SparseRow soc_row;  // soc_t − soc_{t−1} − ch + dis == 0 (soc_0 = 0)
    soc_row[soc] = 1.0;
    if (t > 0) {
      soc_row[m.soc.back()] = -1.0;
    }
    soc_row[ch] = -1.0;
    soc_row[dis] = 1.0;
    soc_row.equal(0.0);
    (void)lp.add_row(soc_row);

    m.ug.push_back(ug);
    m.pg.push_back(pg);
    m.ch.push_back(ch);
    m.dis.push_back(dis);
    m.soc.push_back(soc);
  }
  return m;
}

[[nodiscard]] std::vector<RelaxAndFixCol> rfs_window_cols(const RfsUc& m)
{
  std::vector<RelaxAndFixCol> cols;
  for (int t = 0; t < rfs_blocks; ++t) {
    cols.push_back({
        .col = static_cast<int>(m.ug[static_cast<std::size_t>(t)]),
        .start_hour = static_cast<double>(t),
    });
  }
  return cols;
}

/// Recompute the storage fixture's physics from the primal: balance rows,
/// SOC chain, and the end-horizon target.
void rfs_check_physics(const RfsUc& m, std::span<const double> sol)
{
  double soc_prev = 0.0;
  for (int t = 0; t < rfs_blocks; ++t) {
    const auto u = static_cast<std::size_t>(t);
    const double pg = rf_at(sol, m.pg[u]);
    const double ch = rf_at(sol, m.ch[u]);
    const double dis = rf_at(sol, m.dis[u]);
    const double soc = rf_at(sol, m.soc[u]);
    CHECK(pg + dis - ch == doctest::Approx(rfs_demand(t)).epsilon(1e-6));
    CHECK(soc - soc_prev - ch + dis == doctest::Approx(0.0).epsilon(1e-6));
    soc_prev = soc;
  }
  CHECK(soc_prev == doctest::Approx(rfs_end_target).epsilon(1e-6));
}

}  // namespace

TEST_CASE(
    "relax_and_fix - make_relax_and_fix_cols partition builder")  // NOLINT
{
  // Two commitments over three blocks: unit 7's period column 11 spans
  // blocks 0–1 (listed once per member block → dedupe to the EARLIEST
  // hour); unit 8 has per-block columns.  Block 99 is absent from the hour
  // map → its column is skipped.
  std::vector<CommitmentRunInfo> commitments;
  commitments.push_back(CommitmentRunInfo {
      .uid = Uid {7},
      .status_cols = {11, 11, 12},
      .block_uids = {Uid {0}, Uid {1}, Uid {2}},
  });
  commitments.push_back(CommitmentRunInfo {
      .uid = Uid {8},
      .status_cols = {21, 22},
      .block_uids = {Uid {1}, Uid {99}},
  });

  const std::unordered_map<Uid, double> hours {
      {Uid {0}, 0.0},
      {Uid {1}, 1.0},
      {Uid {2}, 2.0},
  };

  const auto cols = make_relax_and_fix_cols(commitments, hours);
  REQUIRE(cols.size() == 3);
  // Sorted by (hour, col); the period column 11 keeps hour 0.
  CHECK(cols[0].col == 11);
  CHECK(cols[0].start_hour == doctest::Approx(0.0));
  CHECK(cols[1].col == 21);
  CHECK(cols[1].start_hour == doctest::Approx(1.0));
  CHECK(cols[2].col == 12);
  CHECK(cols[2].start_hour == doctest::Approx(2.0));
}

TEST_CASE(  // NOLINT
    "relax_and_fix - 3-window solution is integer-feasible at the full-MIP "
    "optimum per MIP plugin")
{
  const auto solvers = rf_windowing_solvers();
  if (solvers.empty()) {
    MESSAGE("no windowing-capable MIP solver plugin loaded — skipping");
    return;
  }
  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    const double opt = rf6_cold_optimum(solver_name);
    REQUIRE(opt == doctest::Approx(rf6_opt_obj).epsilon(1e-6));

    // No overlap: three disjoint 2 h windows.
    {
      LinearInterface lp(solver_name);
      const auto m = build_rf6_uc(lp);
      const auto cols = rf6_window_cols(m);

      const auto rep = solve_relax_and_fix(lp,
                                           SolverOptions {},
                                           cols,
                                           {
                                               .window_hours = 2.0,
                                               .overlap_hours = 0.0,
                                           });
      REQUIRE(rep.has_value());
      CHECK(rep->windows == 3);
      CHECK(rep->window_cols == 2 * rf6_blocks);
      CHECK_FALSE(rep->fallback);
      REQUIRE(lp.is_optimal());

      // Heuristic upper bound … that this separable fixture makes tight.
      const double obj = lp.get_obj_value();
      CHECK(obj >= opt - 1e-6);
      CHECK(obj == doctest::Approx(opt).epsilon(1e-6));

      const auto sol = lp.get_col_sol_raw();
      rf_check_integral(sol, m.ua);
      rf_check_integral(sol, m.ub);
      for (int t = 0; t < rf6_blocks; ++t) {
        const auto u = static_cast<std::size_t>(t);
        CHECK(rf_at(sol, m.ua[u]) == doctest::Approx(1.0));
        CHECK(rf_at(sol, m.ub[u])
              == doctest::Approx(rf6_demand(t) > 50.0 ? 1.0 : 0.0));
      }
    }

    // 1 h overlap re-optimizes the previous window's tail.
    {
      LinearInterface lp(solver_name);
      const auto m = build_rf6_uc(lp);
      const auto cols = rf6_window_cols(m);

      const auto rep = solve_relax_and_fix(lp,
                                           SolverOptions {},
                                           cols,
                                           {
                                               .window_hours = 2.0,
                                               .overlap_hours = 1.0,
                                           });
      REQUIRE(rep.has_value());
      CHECK(rep->windows == 3);
      CHECK_FALSE(rep->fallback);
      REQUIRE(lp.is_optimal());
      CHECK(lp.get_obj_value() == doctest::Approx(opt).epsilon(1e-6));

      const auto sol = lp.get_col_sol_raw();
      rf_check_integral(sol, m.ua);
      rf_check_integral(sol, m.ub);
    }
  }
}

TEST_CASE("relax_and_fix - CBC gates to the plain full MIP")  // NOLINT
{
  // CBC cannot re-solve a mutated MIP in place, so solve_relax_and_fix must
  // detect the backend and fall back to a single plain full-MIP solve —
  // correct optimum, `windows == 1`, no rolling, no corruption.
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (std::ranges::find(solvers, "cbc") == solvers.end()) {
    MESSAGE("cbc plugin not loaded — skipping");
    return;
  }

  const double opt = rf6_cold_optimum("cbc");
  REQUIRE(opt == doctest::Approx(rf6_opt_obj).epsilon(1e-6));

  LinearInterface lp("cbc");
  const auto m = build_rf6_uc(lp);
  const auto cols = rf6_window_cols(m);

  const auto rep = solve_relax_and_fix(lp,
                                       SolverOptions {},
                                       cols,
                                       {
                                           .window_hours = 2.0,
                                           .overlap_hours = 0.0,
                                       });
  REQUIRE(rep.has_value());
  CHECK(rep->windows == 1);  // gated: plain full MIP, no rolling
  CHECK_FALSE(rep->fallback);
  REQUIRE(lp.is_optimal());
  CHECK(lp.get_obj_value() == doctest::Approx(opt).epsilon(1e-6));
}

TEST_CASE(  // NOLINT
    "relax_and_fix - storage end-horizon consistency survives the rolling "
    "windows")
{
  const auto solver_name = rf_first_windowing_solver();
  if (solver_name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(solver_name);

  // Full-MIP optimum for reference.
  LinearInterface cold(solver_name);
  (void)build_rfs_uc(cold);
  REQUIRE(cold.initial_solve().has_value());
  REQUIRE(cold.resolve().has_value());
  REQUIRE(cold.is_optimal());
  const double opt = cold.get_obj_value();
  REQUIRE(opt == doctest::Approx(1780.0).epsilon(1e-6));

  LinearInterface lp(solver_name);
  const auto m = build_rfs_uc(lp);
  const auto rep = solve_relax_and_fix(lp,
                                       SolverOptions {},
                                       rfs_window_cols(m),
                                       {
                                           .window_hours = 2.0,
                                           .overlap_hours = 0.0,
                                       });
  REQUIRE(rep.has_value());
  CHECK(rep->windows == 3);
  CHECK(rep->window_cols == rfs_blocks);
  CHECK_FALSE(rep->fallback);
  REQUIRE(lp.is_optimal());

  // Integer-feasible, at (never below) the full optimum, and the SOC
  // physics — including the end-horizon target the early windows can only
  // honour through the whole model — hold exactly.
  const double obj = lp.get_obj_value();
  CHECK(obj >= opt - 1e-6);
  CHECK(obj == doctest::Approx(opt).epsilon(1e-6));
  const auto sol = lp.get_col_sol_raw();
  rf_check_integral(sol, m.ug);
  rfs_check_physics(m, sol);
}

TEST_CASE(  // NOLINT
    "relax_and_fix - whole-horizon window reproduces the plain MIP")
{
  const auto solver_name = rf_first_windowing_solver();
  if (solver_name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(solver_name);

  const double opt = rf6_cold_optimum(solver_name);

  for (const double whole_window : {6.0, 1000.0}) {
    CAPTURE(whole_window);
    LinearInterface lp(solver_name);
    const auto m = build_rf6_uc(lp);

    const auto rep = solve_relax_and_fix(lp,
                                         SolverOptions {},
                                         rf6_window_cols(m),
                                         {
                                             .window_hours = whole_window,
                                             .overlap_hours = 0.0,
                                         });
    REQUIRE(rep.has_value());
    CHECK(rep->windows == 1);  // single window ⇒ the plain-MIP path
    CHECK(rep->window_cols == 2 * rf6_blocks);
    CHECK_FALSE(rep->fallback);
    REQUIRE(lp.is_optimal());
    CHECK(lp.get_obj_value() == doctest::Approx(opt).epsilon(1e-6));

    const auto sol = lp.get_col_sol_raw();
    rf_check_integral(sol, m.ua);
    rf_check_integral(sol, m.ub);
  }
}

// ── SystemLP-level orchestration (monolithic_options.relax_and_fix_window) ─

namespace
{

const Simulation raf_three_block_simulation = {
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

[[nodiscard]] System make_raf_commitment_system()
{
  System system;
  system.name = "RelaxAndFix";
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
/// relax-and-fix window; return the optimal objective.
[[nodiscard]] double resolve_raf_commitment_system(
    const std::string& solver_name, int window, int overlap = 0)
{
  System system = make_raf_commitment_system();

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  poptions.lp_matrix_options.solver_name = solver_name;
  poptions.monolithic_options.relax_and_fix_window = window;
  if (overlap > 0) {
    poptions.monolithic_options.relax_and_fix_overlap = overlap;
  }

  PlanningOptionsLP options(std::move(poptions));
  SimulationLP simulation_lp(raf_three_block_simulation, options);
  SystemLP system_lp(system, simulation_lp, LpMatrixOptions {});

  const auto result = system_lp.resolve(SolverOptions {.log_level = 0});
  REQUIRE(result.has_value());
  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.is_optimal());
  return lp.get_obj_value();
}

}  // namespace

TEST_CASE(  // NOLINT
    "relax_and_fix - SystemLP monolithic orchestration matches the plain "
    "solve")
{
  const auto solver_name = rf_first_windowing_solver();
  if (solver_name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(solver_name);

  // Plain MIP (feature off), then 1 h rolling windows (3 windows over the
  // 3 chronological blocks), then a 1 h overlap variant, then a window
  // wider than the horizon (single window ⇒ plain path).  This fixture's
  // optimum is reachable greedily, so every variant lands the same
  // objective.
  const double obj_plain = resolve_raf_commitment_system(solver_name, 0);
  const double obj_raf = resolve_raf_commitment_system(solver_name, 1);
  CHECK(obj_raf == doctest::Approx(obj_plain).epsilon(1e-6));

  const double obj_overlap = resolve_raf_commitment_system(solver_name, 2, 1);
  CHECK(obj_overlap == doctest::Approx(obj_plain).epsilon(1e-6));

  const double obj_whole = resolve_raf_commitment_system(solver_name, 24);
  CHECK(obj_whole == doctest::Approx(obj_plain).epsilon(1e-6));
}

TEST_CASE("relax_and_fix - options parse from JSON and merge")  // NOLINT
{
  constexpr std::string_view json =
      R"({"relax_and_fix_window": 24, "relax_and_fix_overlap": 4})";
  const auto opts = daw::json::from_json<MonolithicOptions>(json);
  CHECK(opts.relax_and_fix_window.value_or(0) == 24);
  CHECK(opts.relax_and_fix_overlap.value_or(0) == 4);

  // Round trip.
  const auto round = daw::json::to_json(opts);
  const auto rt =
      daw::json::from_json<MonolithicOptions>(std::string_view {round});
  CHECK(rt.relax_and_fix_window.value_or(0) == 24);
  CHECK(rt.relax_and_fix_overlap.value_or(0) == 4);

  // merge(): an incoming value overrides, absent values keep the base.
  MonolithicOptions base;
  base.relax_and_fix_window = 12;
  MonolithicOptions incoming;
  incoming.relax_and_fix_overlap = 2;
  base.merge(std::move(incoming));
  CHECK(base.relax_and_fix_window.value_or(0) == 12);
  CHECK(base.relax_and_fix_overlap.value_or(0) == 2);
}
