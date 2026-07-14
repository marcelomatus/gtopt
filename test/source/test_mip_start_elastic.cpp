// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_mip_start_elastic.cpp
 * @brief     Elastic in-process seed completion (`mip_start.elastic`):
 *            imperfect seeds are repaired against the FULL LP and injected
 *            as complete, accepted starts — warm-starts never depend on a
 *            perfectly feasible seed.
 * @date      2026-07-14
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * Empirical motivation (CEN cases): an integer-only commitment seed under
 * `effort=solve_fixed` requires the u-fixed LP to be feasible WITHOUT slack,
 * and real seeds keep failing constraint families that are NOT derivable
 * offline (hydro water coupling, network evacuation limits).  The elastic
 * completion repairs in-process with exactly TWO internal LP solves: one ±M
 * objective-bias LP (a feasible seed comes out unchanged, deviation = 0 —
 * there is NO dedicated u-fixed probe LP) → threshold → one u-fixed re-fix
 * for a consistent dispatch → inject the COMPLETE start under
 * `check_feasibility`.  Every test asserts `elastic_lp_solves == 2` — the
 * stage-count proof that the former probe-LP stage stays gone.
 *
 * Fixture: the tiny 2-generator / 2-block UC from
 * test_mip_start_roundtrip.cpp PLUS a per-block u-order coupling row
 * u_B,t <= u_A,t (B may only run when A runs) — the stand-in for the
 * non-derivable-offline families.  The coupling holds at the original
 * optimum, so the MIP optimum is unchanged:
 *   u = (1,0,1,1), p = (50,0,50,40), obj = 3240.
 * A seed with u_B=1, u_A=0 in some block violates the coupling ⇒ the seed
 * pattern is u-fixed infeasible and the bias LP must repair it.  Closed-form
 * bias outcome for the violating seed (1,0,0,1): block 2's ±M penalties
 * cancel along the binding coupling (u_A2 = u_B2 = x), leaving the true
 * costs to pick x = 0.9 ⇒ thresholds to (1,1) ⇒ repaired pattern (1,0,1,1)
 * — the exact MIP optimum, with ONE seeded flip (u_A2: 0 → 1).
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/domain_rules.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/solver_enums.hpp>
#include <gtopt/solver_options.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace mip_start_elastic_test  // NOLINT(misc-use-anonymous-namespace)
{
namespace
{

constexpr double kElasticOptObj = 3240.0;
constexpr std::size_t kElasticNumCols = 8;

struct ElasticUc
{
  // column indices, blocks 1..2
  ColIndex ua1 {}, ub1 {}, pa1 {}, pb1 {};
  ColIndex ua2 {}, ub2 {}, pa2 {}, pb2 {};
};

/// The round-trip tiny UC (8 cols, 10 rows) plus one u-order coupling row
/// per block: u_B,t - u_A,t <= 0.
ElasticUc build_elastic_uc(LinearInterface& lp)
{
  ElasticUc m;

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

    // Coupling (the non-derivable-offline family): B only runs with A.
    SparseRow order;
    order[ub] = 1.0;
    order[ua] = -1.0;
    order.less_equal(0.0);
    (void)lp.add_row(order);
  };

  add_block(50.0, m.ua1, m.ub1, m.pa1, m.pb1);
  add_block(90.0, m.ua2, m.ub2, m.pa2, m.pb2);
  return m;
}

/// Verify a dense raw solution vector is the unique MIP optimum:
/// u = (1,0,1,1), p = (50,0,50,40).
void check_elastic_optimum(const ElasticUc& m, std::span<const double> sol)
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

/// The elastic completion must restore integrality, the u-column bounds
/// ([0,1]) and every objective coefficient EXACTLY before returning.
void check_elastic_restored(LinearInterface& lp, const ElasticUc& m)
{
  const auto lb = lp.get_col_low_raw();
  const auto ub = lp.get_col_upp_raw();
  const auto obj = lp.get_obj_coeff();
  const auto at = [](std::span<const double> v, ColIndex c)
  { return v[static_cast<std::size_t>(static_cast<int>(c))]; };
  for (const ColIndex& c : {m.ua1, m.ub1, m.ua2, m.ub2}) {
    CHECK(lp.is_integer(c));
    CHECK(at(lb, c) == doctest::Approx(0.0));
    CHECK(at(ub, c) == doctest::Approx(1.0));
  }
  CHECK(at(obj, m.ua1) == doctest::Approx(100.0));
  CHECK(at(obj, m.ub1) == doctest::Approx(40.0));
  CHECK(at(obj, m.ua2) == doctest::Approx(100.0));
  CHECK(at(obj, m.ub2) == doctest::Approx(40.0));
  CHECK(at(obj, m.pa1) == doctest::Approx(10.0));
  CHECK(at(obj, m.pb2) == doctest::Approx(50.0));
}

/// One seed row of the `generator_uid,block_uid,u` CSV.
struct ElasticSeedRow
{
  int gen {};
  int blk {};
  double u {};
};

/// Write the commitment-seed CSV `load_seed_commitment` consumes.
void write_elastic_seed_csv(const std::filesystem::path& path,
                            std::span<const ElasticSeedRow> rows)
{
  std::ofstream out(path, std::ios::trunc);
  out << "generator_uid,block_uid,u\n";
  for (const auto& r : rows) {
    out << r.gen << ',' << r.blk << ',' << r.u << '\n';
  }
}

/// The (generator, block) identity map: gen A = uid 1, gen B = uid 2;
/// blocks 1..2.
[[nodiscard]] std::vector<CommitmentRunInfo> elastic_commitments(
    const ElasticUc& m)
{
  std::vector<CommitmentRunInfo> commitments;
  commitments.push_back(CommitmentRunInfo {
      .uid = Uid {1},
      .status_cols = {static_cast<int>(m.ua1), static_cast<int>(m.ua2)},
      .block_uids = {Uid {1}, Uid {2}},
  });
  commitments.push_back(CommitmentRunInfo {
      .uid = Uid {2},
      .status_cols = {static_cast<int>(m.ub1), static_cast<int>(m.ub2)},
      .block_uids = {Uid {1}, Uid {2}},
  });
  return commitments;
}

[[nodiscard]] std::string elastic_slurp(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// RAII native-log capture (same pattern as test_mip_start_roundtrip.cpp).
struct ElasticNativeLog
{
  std::filesystem::path log_path;

  explicit ElasticNativeLog(LinearInterface& li, const std::string& tag)
      : log_path(std::filesystem::temp_directory_path()
                 / ("gtopt_mipstart_elastic_" + tag))
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

  [[nodiscard]] std::string text() const { return elastic_slurp(with_ext()); }
};

[[nodiscard]] SolverOptions elastic_native_log_opts()
{
  SolverOptions opts;
  opts.log_level = 1;
  opts.log_mode = SolverLogMode::detailed;
  return opts;
}

/// CPLEX prints the sharp MIP-start acceptance signal into its native log
/// ("1 of 1 MIP starts provided solutions." vs "No solution found from ...").
void check_elastic_cplex_accepted(const std::string& name,
                                  const ElasticNativeLog& log)
{
  if (name != "cplex") {
    return;
  }
  const auto text = log.text();
  CAPTURE(text);
  CHECK(text.contains("MIP starts provided solutions"));
  CHECK_FALSE(text.contains("No solution found from"));
}

}  // namespace

TEST_CASE(  // NOLINT
    "mip_start elastic - violating seed is repaired in-process per MIP "
    "plugin")
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  const auto csv_path = std::filesystem::temp_directory_path()
      / "gtopt_mipstart_elastic_violating_seed.csv";
  // Seed (1,0,0,1): block 2 has u_B=1 with u_A=0 — violates the coupling
  // u_B <= u_A, so the u-fixed LP is INFEASIBLE and phase (b) must engage.
  const std::array<ElasticSeedRow, 4> rows {
      ElasticSeedRow {.gen = 1, .blk = 1, .u = 1.0},
      ElasticSeedRow {.gen = 2, .blk = 1, .u = 0.0},
      ElasticSeedRow {.gen = 1, .blk = 2, .u = 0.0},
      ElasticSeedRow {.gen = 2, .blk = 2, .u = 1.0},
  };
  write_elastic_seed_csv(csv_path, rows);

  for (const auto& name : solvers) {
    CAPTURE(name);
    const bool ran = gtopt::solver_test::run_or_skip_license(
        [&]
        {
          LinearInterface lp(name);
          const auto m = build_elastic_uc(lp);
          const auto commitments = elastic_commitments(m);
          const ElasticNativeLog log(lp, "repair_" + name);

          MipStartOptions ms;
          ms.enabled = true;
          ms.elastic = true;
          ms.seed_solution_file = csv_path.string();
          const auto report = apply_mip_start(
              lp, SolverOptions {.log_level = 0}, ms, commitments);
          REQUIRE(report.has_value());
          CHECK(report->relaxation_solved);
          CHECK(report->relaxation_feasible);
          CHECK(report->injected);
          CHECK(report->source == "elastic+seed");
          CHECK(report->elastic_repaired);
          CHECK(report->seed_deviation == 1);  // u_A2: seed 0 → repaired 1
          // Stage count: ONE bias LP + ONE u-fixed re-fix — the dedicated
          // u-fixed probe LP is gone.
          CHECK(report->elastic_lp_solves == 2);

          // Bounds / objective / integrality restored exactly.
          check_elastic_restored(lp, m);

          // The repaired start is the exact optimum: the MIP re-solves to
          // it, and on cplex the native log carries the sharp acceptance.
          REQUIRE(lp.resolve(elastic_native_log_opts()).has_value());
          REQUIRE(lp.is_optimal());
          CHECK(lp.get_obj_value()
                == doctest::Approx(kElasticOptObj).epsilon(1e-6));
          check_elastic_optimum(m, lp.get_col_sol_raw());
          check_elastic_cplex_accepted(name, log);
        });
    if (!ran) {
      MESSAGE("skipping ", name, " — license unavailable");
    }
  }
  std::filesystem::remove(csv_path);
}

TEST_CASE(  // NOLINT
    "mip_start elastic - feasible seed is accepted without repair per MIP "
    "plugin")
{
  const auto solvers = gtopt::solver_test::exact_mip_solvers();
  if (solvers.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  const auto csv_path = std::filesystem::temp_directory_path()
      / "gtopt_mipstart_elastic_feasible_seed.csv";
  // Seed (1,0,1,1) — the optimal commitment: the u-fixed LP is feasible, so
  // the elastic completion is a no-op (no repair, zero deviation).
  const std::array<ElasticSeedRow, 4> rows {
      ElasticSeedRow {.gen = 1, .blk = 1, .u = 1.0},
      ElasticSeedRow {.gen = 2, .blk = 1, .u = 0.0},
      ElasticSeedRow {.gen = 1, .blk = 2, .u = 1.0},
      ElasticSeedRow {.gen = 2, .blk = 2, .u = 1.0},
  };
  write_elastic_seed_csv(csv_path, rows);

  for (const auto& name : solvers) {
    CAPTURE(name);
    const bool ran = gtopt::solver_test::run_or_skip_license(
        [&]
        {
          LinearInterface lp(name);
          const auto m = build_elastic_uc(lp);
          const auto commitments = elastic_commitments(m);
          const ElasticNativeLog log(lp, "noop_" + name);

          MipStartOptions ms;
          ms.enabled = true;
          ms.elastic = true;
          ms.seed_solution_file = csv_path.string();
          const auto report = apply_mip_start(
              lp, SolverOptions {.log_level = 0}, ms, commitments);
          REQUIRE(report.has_value());
          CHECK(report->relaxation_solved);
          CHECK(report->relaxation_feasible);
          CHECK(report->injected);
          CHECK(report->source == "elastic+seed");
          CHECK_FALSE(report->elastic_repaired);  // no repair needed
          CHECK(report->seed_deviation == 0);  // seed survived verbatim
          // Stage count: a FEASIBLE seed also takes exactly one bias LP +
          // one re-fix (deviation = 0 is detected from the bias solution;
          // no separate u-fixed probe LP is solved).
          CHECK(report->elastic_lp_solves == 2);

          check_elastic_restored(lp, m);

          REQUIRE(lp.resolve(elastic_native_log_opts()).has_value());
          REQUIRE(lp.is_optimal());
          CHECK(lp.get_obj_value()
                == doctest::Approx(kElasticOptObj).epsilon(1e-6));
          check_elastic_optimum(m, lp.get_col_sol_raw());
          check_elastic_cplex_accepted(name, log);
        });
    if (!ran) {
      MESSAGE("skipping ", name, " — license unavailable");
    }
  }
  std::filesystem::remove(csv_path);
}

TEST_CASE(  // NOLINT
    "mip_start elastic - infeasible complete dump is repaired (from_file)")
{
  const auto name = gtopt::solver_test::first_mip_solver();
  if (name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(name);

  // A hand-written COMPLETE dump whose commitment is infeasible: block 2 has
  // BOTH units off (demand 90 unserved) and stale continuous values.  The
  // elastic completion must repair it to the true optimum (2 seeded flips:
  // u_A2 and u_B2 both 0 → 1).
  const auto dump_path = std::filesystem::temp_directory_path()
      / "gtopt_mipstart_elastic_bad.dump";
  {
    std::ofstream out(dump_path, std::ios::trunc);
    out << "# gtopt mip_start complete solution (index value)\n";
    out << "ncols 8\n";
    out << "nint 4\n";
    out << "0 1\n";  // uA1
    out << "1 0\n";  // uB1
    out << "2 50\n";  // pA1
    out << "3 0\n";  // pB1
    out << "4 0\n";  // uA2 — flipped off
    out << "5 0\n";  // uB2 — flipped off
    out << "6 50\n";  // pA2 (stale)
    out << "7 40\n";  // pB2 (stale)
  }

  LinearInterface lp(name);
  const auto m = build_elastic_uc(lp);

  MipStartOptions ms;
  ms.enabled = true;
  ms.elastic = true;
  ms.from_file = dump_path.string();
  const auto report = apply_mip_start(lp, SolverOptions {.log_level = 0}, ms);
  REQUIRE(report.has_value());
  CHECK(report->relaxation_solved);
  CHECK(report->relaxation_feasible);
  CHECK(report->injected);
  CHECK(report->source == "elastic+file");
  CHECK(report->elastic_repaired);
  CHECK(report->seed_deviation == 2);  // u_A2 and u_B2: 0 → 1
  CHECK(report->elastic_lp_solves == 2);  // one bias LP + one re-fix

  check_elastic_restored(lp, m);

  REQUIRE(lp.resolve(SolverOptions {.log_level = 0}).has_value());
  REQUIRE(lp.is_optimal());
  CHECK(lp.get_obj_value() == doctest::Approx(kElasticOptObj).epsilon(1e-6));
  REQUIRE(lp.get_col_sol_raw().size() == kElasticNumCols);
  check_elastic_optimum(m, lp.get_col_sol_raw());

  std::filesystem::remove(dump_path);
}

TEST_CASE(  // NOLINT
    "mip_start elastic - unmatched seed falls back to the standard candidate")
{
  const auto name = gtopt::solver_test::first_mip_solver();
  if (name.empty()) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping");
    return;
  }
  CAPTURE(name);

  // The seed CSV names a generator that does not exist in this LP and no
  // commitment info is supplied: the elastic completion has nothing to
  // complete, so the pipeline must WARN and fall back to the standard
  // round+seed candidate — never abort the solve.
  const auto csv_path = std::filesystem::temp_directory_path()
      / "gtopt_mipstart_elastic_unmatched_seed.csv";
  const std::array<ElasticSeedRow, 2> rows {
      ElasticSeedRow {.gen = 99, .blk = 1, .u = 1.0},
      ElasticSeedRow {.gen = 99, .blk = 2, .u = 1.0},
  };
  write_elastic_seed_csv(csv_path, rows);

  LinearInterface lp(name);
  const auto m = build_elastic_uc(lp);

  MipStartOptions ms;
  ms.enabled = true;
  ms.elastic = true;
  ms.seed_solution_file = csv_path.string();
  const auto report = apply_mip_start(lp, SolverOptions {.log_level = 0}, ms);
  REQUIRE(report.has_value());
  CHECK(report->relaxation_solved);
  CHECK(report->relaxation_feasible);
  CHECK(report->source == "warmstart");  // the standard fallback candidate
  CHECK_FALSE(report->elastic_repaired);
  CHECK(report->seed_deviation == 0);
  CHECK(report->elastic_lp_solves == 0);  // elastic never solved anything

  // Nothing was left behind by the aborted elastic attempt.
  check_elastic_restored(lp, m);

  std::filesystem::remove(csv_path);
}

}  // namespace mip_start_elastic_test
