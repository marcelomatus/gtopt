// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_fixedmilp_warmstart.cpp
 * @brief     CPLEX-only: the MIP->dual fixed pass warm-starts (primal
 *            simplex off the incumbent basis) instead of a cold barrier.
 * @date      2026-06-07
 * @author    claude
 * @copyright BSD-3-Clause
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <unistd.h>

using namespace gtopt;

namespace test_fixedmilp_warmstart_ns
{
// Anonymous namespace for internal linkage; the uniquely-named parent keeps
// these helpers from colliding with other test files under unity builds.
namespace
{

std::filesystem::path make_tmp_dir()
{
  const char* tmp = std::getenv("TMPDIR");
  std::filesystem::path base = (tmp != nullptr && *tmp != '\0')
      ? std::filesystem::path {tmp}
      : std::filesystem::path {std::getenv("HOME")} / "tmp";  // NOLINT
  const auto dir = base
      / std::format("gtopt-fixedmilp-warm-{}",
                    static_cast<std::uintptr_t>(::getpid()));
  std::filesystem::create_directories(dir / "solvers");
  return dir;
}

std::string slurp(const std::filesystem::path& p)
{
  std::ifstream in(p);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

}  // namespace test_fixedmilp_warmstart_ns

TEST_CASE("fix_mip_and_resolve_duals warm-starts on CPLEX")  // NOLINT
{
  using namespace test_fixedmilp_warmstart_ns;

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.supports_mip("cplex")) {
    MESSAGE("CPLEX MIP backend not available — skipping warm-start test");
    return;
  }

  const auto dir = make_tmp_dir();
  const auto prm = dir / "solvers" / "cplex.prm";
  const auto prm_fixed = dir / "solvers" / "cplex_warmstart.prm";
  const auto logbase = dir / "cplex";  // backend appends ".log"

  {
    std::ofstream f(prm);
    f << "CPLEX Parameter File Version 22.1.1\n";
    f << "CPXPARAM_LPMethod 4\n";  // barrier for the cold MIP-root path
  }
  {
    std::ofstream f(prm_fixed);
    f << "CPLEX Parameter File Version 22.1.1\n";
    f << "CPXPARAM_LPMethod 1\n";  // primal simplex
    f << "CPXPARAM_Advance 1\n";  // use the incumbent's advanced basis
    f << "CPXPARAM_ScreenOutput 1\n";  // emit the fixed-pass algorithm
  }

  // A transport-style MILP large enough that a COLD solve of the fixed LP
  // would log a Barrier / Crossover / second presolve, but a warm primal
  // pass off the incumbent basis would not.  S supply nodes -> D demand
  // nodes; each arc is a continuous flow x_sd >= 0, gated by a binary
  // y_sd in {0,1} (open/closed) and a per-source open-count constraint.
  constexpr int S = 12;
  constexpr int D = 20;

  LinearInterface lp("cplex");

  const auto at = [](int s, int d)
  { return (static_cast<std::size_t>(s) * D) + static_cast<std::size_t>(d); };

  // Flow vars x_sd (continuous) and gate vars y_sd (binary).
  std::vector<ColIndex> x(static_cast<std::size_t>(S * D));
  std::vector<ColIndex> y(static_cast<std::size_t>(S * D));
  for (int s = 0; s < S; ++s) {
    for (int d = 0; d < D; ++d) {
      const double cost = 1.0 + static_cast<double>(((s * 7) + (d * 3)) % 11);
      x[at(s, d)] =
          lp.add_col(SparseCol {.lowb = 0.0, .uppb = 100.0, .cost = cost});
      y[at(s, d)] =
          lp.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.5});
      lp.set_integer(y[at(s, d)]);
    }
  }

  // Demand: sum_s x_sd >= demand_d   (must serve each demand)
  std::vector<RowIndex> demand_row(static_cast<std::size_t>(D));
  for (int d = 0; d < D; ++d) {
    SparseRow row;
    for (int s = 0; s < S; ++s) {
      row[x[at(s, d)]] = 1.0;
    }
    const double demand = 5.0 + static_cast<double>(d % 4);
    row.greater_equal(demand);
    demand_row[static_cast<std::size_t>(d)] = lp.add_row(row);
  }

  // Supply cap: sum_d x_sd <= cap_s
  for (int s = 0; s < S; ++s) {
    SparseRow row;
    for (int d = 0; d < D; ++d) {
      row[x[at(s, d)]] = 1.0;
    }
    row.less_equal(40.0);
    (void)lp.add_row(row);
  }

  // Gating: x_sd <= 100 * y_sd  ->  x_sd - 100 y_sd <= 0
  for (int s = 0; s < S; ++s) {
    for (int d = 0; d < D; ++d) {
      SparseRow row;
      row[x[at(s, d)]] = 1.0;
      row[y[at(s, d)]] = -100.0;
      row.less_equal(0.0);
      (void)lp.add_row(row);
    }
  }

  // Per-source fan-out cap: sum_d y_sd <= 8 (forces the MIP to branch).
  for (int s = 0; s < S; ++s) {
    SparseRow row;
    for (int d = 0; d < D; ++d) {
      row[y[at(s, d)]] = 1.0;
    }
    row.less_equal(8.0);
    (void)lp.add_row(row);
  }

  lp.set_log_file(logbase.string());

  SolverOptions opts;
  opts.param_file = prm.string();
  opts.log_mode = SolverLogMode::detailed;
  opts.log_level = 1;

  auto solved = lp.initial_solve(opts);
  REQUIRE(solved.has_value());
  REQUIRE(lp.is_optimal());
  REQUIRE(lp.has_integer_cols());

  const double mip_obj = lp.get_obj_value();
  const auto mip_sol = lp.get_col_sol();

  // Capture the fixed-pass CPLEX screen output (the fixed prm sets
  // CPXPARAM_ScreenOutput 1) by redirecting BOTH stdout (fd 1, where CPLEX
  // routes its screen channel) and stderr (fd 2) into one file.
  const auto fixed_capture = (dir / "fixed_screen.txt").string();
  (void)std::fflush(stdout);
  (void)std::fflush(stderr);
  const int saved_out = ::dup(1);  // NOLINT(android-cloexec-dup)
  const int saved_err = ::dup(2);  // NOLINT(android-cloexec-dup)
  {
    // NOLINTNEXTLINE(android-cloexec-fopen,cppcoreguidelines-owning-memory)
    FILE* redir = std::fopen(fixed_capture.c_str(), "w");
    REQUIRE(redir != nullptr);
    (void)::dup2(::fileno(redir), 1);
    (void)::dup2(::fileno(redir), 2);
    (void)std::fclose(redir);  // NOLINT(cppcoreguidelines-owning-memory)
  }

  auto fix = lp.fix_integers_and_resolve(opts);

  (void)std::fflush(stdout);
  (void)std::fflush(stderr);
  (void)::dup2(saved_out, 1);
  (void)::dup2(saved_err, 2);
  (void)::close(saved_out);
  (void)::close(saved_err);

  REQUIRE(fix.has_value());
  CHECK(fix->fixed_columns == S * D);
  REQUIRE(fix->status.has_value());
  CHECK(*fix->status == 0);
  REQUIRE(lp.is_optimal());

  // Primal unchanged; duals now well-defined.
  CHECK(lp.get_obj_value() == doctest::Approx(mip_obj).epsilon(1e-6));
  {
    const auto sol = lp.get_col_sol();
    for (int s = 0; s < S; ++s) {
      for (int d = 0; d < D; ++d) {
        CHECK(sol[x[at(s, d)]]
              == doctest::Approx(mip_sol[x[at(s, d)]]).epsilon(1e-5));
      }
    }
  }
  const auto duals = lp.get_row_dual();
  REQUIRE(duals.size() >= static_cast<std::size_t>(D));
  for (int d = 0; d < D; ++d) {
    CHECK(std::isfinite(duals[demand_row[static_cast<std::size_t>(d)]]));
  }

  // ── Log fingerprint check.  The CPLEX 22.1.1 log strings asserted below
  //    were verified against the REAL captured output, not guessed:
  //
  //  MIP log (cplex.log) — the COLD MIP-root fingerprint, present because
  //  the case is a non-trivial MIP:
  //      "MIP Presolve"
  //      "Reduced MIP has 284 rows, 480 columns, ..."
  //      "240 binaries"
  //      "Root relaxation solution time = ..."
  //
  //  Fixed-pass screen capture — the WARM fingerprint:
  //      "CPXPARAM_LPMethod                                1"   (primal)
  //      "Using devex."                                          (primal
  //                                                               simplex
  //                                                               pricing)
  //    and crucially NONE of the cold barrier / crossover / re-presolve
  //    strings:  "Barrier", "Crossover", "MIP Presolve", "binaries".
  const auto contains = [](const std::string& hay, std::string_view needle)
  { return hay.contains(needle); };

  const auto logpath = logbase.string() + ".log";
  const auto mip_log = slurp(logpath);
  const auto fixed_log = slurp(fixed_capture);

  INFO("MIP log:\n" << mip_log);
  INFO("Fixed-pass capture:\n" << fixed_log);

  // The MIP solve really exercised the cold MIP machinery (sanity: the
  // fixture is big enough that a cold fixed-LP solve would be visibly
  // different from the warm one).
  CHECK(contains(mip_log, "MIP Presolve"));
  CHECK(contains(mip_log, "binaries"));
  CHECK(contains(mip_log, "Root relaxation"));

  // The fixed pass warm-started off the incumbent basis: primal simplex,
  // not a cold barrier.  CPLEX 22.1.1 logs primal simplex as
  // "CPXPARAM_LPMethod ... 1" + "Using devex." and never emits a
  // Barrier/Crossover block nor a second MIP presolve.
  REQUIRE_FALSE(fixed_log.empty());
  CHECK(contains(fixed_log, "CPXPARAM_LPMethod"));
  CHECK(contains(fixed_log, "Using devex."));

  // Cold-barrier / cold-MIP fingerprints MUST be absent from the warm pass.
  CHECK_FALSE(contains(fixed_log, "Barrier"));
  CHECK_FALSE(contains(fixed_log, "Crossover"));
  CHECK_FALSE(contains(fixed_log, "MIP Presolve"));
  CHECK_FALSE(contains(fixed_log, "binaries"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
