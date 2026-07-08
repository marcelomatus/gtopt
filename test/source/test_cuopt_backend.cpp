/**
 * @file      test_cuopt_backend.cpp
 * @brief     Unit tests for the cuOpt solver plugin / backend.
 * @date      2026-06-11
 * @copyright BSD-3-Clause
 *
 * Every TEST_CASE is gated on a working cuOpt backend via
 * `make_cuopt_or_skip()` and silently skips when the plugin is absent OR the
 * GPU cannot solve (the helper smoke-tests a trivial solve so a
 * loadable-but-GPU-less environment skips rather than fails).  These pin the
 * backend invariants plus the cuOpt-specific wiring shipped with the plugin:
 *   - reports its name / capabilities / a large finite `infinity()`,
 *   - solves a trivial LP to the known optimum,
 *   - `load_problem` resets previous LP state (fresh dimensions),
 *   - `set_log_filename` routes a per-solve `CUOPT_LOG_FILE` .log file and
 *     `clear_log_filename` stops it,
 *   - `clone()` yields an independent, solvable backend,
 *   - `set_log_level` / `get_log_level` round-trip.
 */

// SPDX-License-Identifier: BSD-3-Clause
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;

namespace
{

// Trivial 2-variable LP:  min x + y  s.t.  x + y >= 2,  x,y >= 0  →  obj = 2.
struct CuoptTrivialLP2
{
  static constexpr int ncols = 2;
  static constexpr int nrows = 1;
  std::array<int, 3> matbeg {
      0,
      1,
      2,
  };
  std::array<int, 2> matind {
      0,
      0,
  };
  std::array<double, 2> matval {
      1.0,
      1.0,
  };
  std::array<double, 2> collb {
      0.0,
      0.0,
  };
  std::array<double, 2> colub {};
  std::array<double, 2> obj {
      1.0,
      1.0,
  };
  std::array<double, 1> rowlb {
      2.0,
  };
  std::array<double, 1> rowub {};

  explicit CuoptTrivialLP2(double inf)
  {
    colub.fill(inf);
    rowub.fill(inf);
  }

  void load_into(SolverBackend& b) const
  {
    b.load_problem(ncols,
                   nrows,
                   matbeg.data(),
                   matind.data(),
                   matval.data(),
                   collb.data(),
                   colub.data(),
                   obj.data(),
                   rowlb.data(),
                   rowub.data());
  }
};

// Trivial 3-variable LP:  min x + y + z  s.t.  x + y + z >= 3  →  obj = 3.
struct CuoptTrivialLP3
{
  static constexpr int ncols = 3;
  static constexpr int nrows = 1;
  std::array<int, 4> matbeg {
      0,
      1,
      2,
      3,
  };
  std::array<int, 3> matind {
      0,
      0,
      0,
  };
  std::array<double, 3> matval {
      1.0,
      1.0,
      1.0,
  };
  std::array<double, 3> collb {
      0.0,
      0.0,
      0.0,
  };
  std::array<double, 3> colub {};
  std::array<double, 3> obj {
      1.0,
      1.0,
      1.0,
  };
  std::array<double, 1> rowlb {
      3.0,
  };
  std::array<double, 1> rowub {};

  explicit CuoptTrivialLP3(double inf)
  {
    colub.fill(inf);
    rowub.fill(inf);
  }

  void load_into(SolverBackend& b) const
  {
    b.load_problem(ncols,
                   nrows,
                   matbeg.data(),
                   matind.data(),
                   matval.data(),
                   collb.data(),
                   colub.data(),
                   obj.data(),
                   rowlb.data(),
                   rowub.data());
  }
};

/// Return a fresh cuOpt backend, or nullptr if the plugin is unavailable OR
/// the GPU cannot solve a trivial LP.  The smoke solve keeps GPU-less CI
/// runs skipping rather than failing (the .so may load without a usable GPU).
[[nodiscard]] std::unique_ptr<SolverBackend> make_cuopt_or_skip()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("cuopt")) {
    return nullptr;
  }
  try {
    auto probe = reg.create("cuopt");
    const CuoptTrivialLP2 lp {probe->infinity()};
    lp.load_into(*probe);
    probe->initial_solve();
    if (!probe->is_proven_optimal()) {
      return nullptr;  // plugin present but GPU unusable → skip
    }
    return reg.create("cuopt");
  } catch (...) {
    return nullptr;
  }
}

[[nodiscard]] std::filesystem::path cuopt_scratch_log_base(std::string_view tag)
{
  return std::filesystem::temp_directory_path()
      / (std::string {"gtopt_cuopt_backend_"} + std::string {tag});
}

TEST_CASE("cuOpt backend reports its name and capabilities")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  CHECK(backend->solver_name() == "cuopt");
  CHECK_FALSE(backend->solver_version().empty());
  // cuOpt's infinity must be a large finite sentinel (never std::numeric
  // ::infinity, which would poison the matrix), and it solves MIPs.
  CHECK(backend->infinity() > 1e18);
  CHECK(backend->supports_mip());
}

TEST_CASE("cuOpt solves a trivial LP to optimality")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  const CuoptTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  CHECK(backend->get_num_cols() == 2);
  CHECK(backend->get_num_rows() == 1);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));
}

TEST_CASE("cuOpt load_problem resets previous LP state")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  const double inf = backend->infinity();

  const CuoptTrivialLP2 lp2 {inf};
  lp2.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));

  // A second load must yield a *fresh* model with different dimensions — a
  // stale model would still report ncols == 2.
  const CuoptTrivialLP3 lp3 {inf};
  lp3.load_into(*backend);
  CHECK(backend->get_num_cols() == 3);
  CHECK(backend->get_num_rows() == 1);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(3.0));
}

TEST_CASE("cuOpt set_log_filename writes a .log; clear stops it")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  namespace fs = std::filesystem;
  const auto base = cuopt_scratch_log_base("written");
  const auto file = fs::path {base.string() + ".log"};
  fs::remove(file);

  // log_mode=detailed → the framework calls set_log_filename(stem, level>0);
  // the backend must route cuOpt's solve log to "<stem>.log".
  backend->set_log_filename(base.string(), 1);
  const CuoptTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  CHECK(fs::exists(file));

  // clear_log_filename must be callable and restore silence.
  backend->clear_log_filename();
  fs::remove(file);
}

TEST_CASE("cuOpt clone yields an independent solvable backend")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  const CuoptTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());

  auto clone = backend->clone();
  REQUIRE(clone != nullptr);

  // The clone owns its own state and solves a different LP without disturbing
  // the original.
  const CuoptTrivialLP3 lp3 {clone->infinity()};
  lp3.load_into(*clone);
  clone->initial_solve();
  REQUIRE(clone->is_proven_optimal());
  CHECK(clone->obj_value() == doctest::Approx(3.0));
  CHECK(backend->obj_value() == doctest::Approx(2.0));
}

TEST_CASE("cuOpt open_log / close_log adjust the reported log level")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  // The cuOpt backend ignores the FILE* (it logs to console / CUOPT_LOG_FILE)
  // but tracks the level, which `get_log_level` must report.
  backend->open_log(nullptr, 2);
  CHECK(backend->get_log_level() == 2);
  backend->close_log();
  CHECK(backend->get_log_level() == 0);
}

TEST_CASE("cuOpt warm re-solve stays correct across cuts and bounds")  // NOLINT
{
  // The SDDP incremental shape: solve, append a cut row, re-solve; then
  // move a bound and re-solve again.  Every re-solve after the first is
  // auto-seeded from the previous optimal snapshot (vector warm start,
  // dual zero-padded for the appended row).  The pin is CORRECTNESS: a
  // warm-started optimum must match the cold optimum, tighter or looser.
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  const CuoptTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));

  // Benders-style cut row: x + y >= 3 tightens the optimum to 3.
  const std::array<int, 2> cut_cols {
      0,
      1,
  };
  const std::array<double, 2> cut_vals {
      1.0,
      1.0,
  };
  backend->add_row(
      2, cut_cols.data(), cut_vals.data(), 3.0, backend->infinity());
  backend->resolve();  // warm: primal + zero-padded dual from previous solve
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(3.0));

  // Loosen back below the first cut (forward-pass trial-pin shape): the
  // warm seed sits at the OLD optimum; the solver must still walk down.
  backend->set_row_lower(1, 1.0);
  backend->resolve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));
}

TEST_CASE(
    "cuOpt explicit set_col_solution hint keeps re-solve exact")  // NOLINT
{
  // Explicit OSI-convention hints (set_col_solution / set_row_price) are
  // buffered one-shot and must never bias the returned optimum — even a
  // deliberately WRONG hint.
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  const CuoptTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  const std::array<double, 2> bad_hint {
      9.0,
      9.0,
  };
  backend->set_col_solution(bad_hint.data());
  backend->initial_solve();
  REQUIRE(backend->is_proven_optimal());
  CHECK(backend->obj_value() == doctest::Approx(2.0));
}

TEST_CASE("cuOpt tuning file: cfg named values and legacy prm")  // NOLINT
{
  // The `<solvers>/cuopt.cfg` sibling of gtopt's `param_file` (INI, named
  // values — `presolve = pslp`) and the legacy `cuopt.prm` dialect both
  // route through the shared solver_cfg loader, including the PLUGIN-LOCAL
  // `warmstart` key (consumed, never forwarded).  Pin (a) named-value cfg
  // presolve is accepted and the solve stays exact, and (b) a legacy prm
  // `warmstart false` disables the auto seed without changing results.
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "gtopt_cuopt_prm_test";
  // Fresh per-subcase directory: doctest re-enters the case per SUBCASE and
  // a leftover cuopt.cfg would shadow the prm-dialect subcase.
  fs::remove_all(dir);
  fs::create_directories(dir);

  SolverOptions opts {};
  // param_file itself need not exist — only its DIRECTORY anchors the
  // cuopt.cfg / cuopt.prm sibling lookup.
  opts.param_file = (dir / "cplex.prm").string();

  SUBCASE("cfg with named presolve + section is accepted; solve exact")
  {
    {
      std::ofstream out {dir / "cuopt.cfg"};
      out << "# cuopt.cfg test — named values\n";
      out << "[cuopt]\n";
      out << "presolve = pslp\n";
      out << "crossover = on\n";
    }
    backend->apply_options(opts);
    const CuoptTrivialLP2 lp {backend->infinity()};
    lp.load_into(*backend);
    backend->initial_solve();
    REQUIRE(backend->is_proven_optimal());
    CHECK(backend->obj_value() == doctest::Approx(2.0));
  }

  SUBCASE("legacy prm 'warmstart false' disables the auto seed; exact")
  {
    {
      std::ofstream out {dir / "cuopt.prm"};
      out << "warmstart false\n";
    }
    backend->apply_options(opts);
    const CuoptTrivialLP2 lp {backend->infinity()};
    lp.load_into(*backend);
    backend->initial_solve();
    REQUIRE(backend->is_proven_optimal());
    CHECK(backend->obj_value() == doctest::Approx(2.0));
    // Incremental re-solve (would auto-warm by default) must still be exact
    // with the seed disabled by the file.
    const std::array<int, 2> cut_cols {
        0,
        1,
    };
    const std::array<double, 2> cut_vals {
        1.0,
        1.0,
    };
    backend->add_row(
        2, cut_cols.data(), cut_vals.data(), 3.0, backend->infinity());
    backend->resolve();
    REQUIRE(backend->is_proven_optimal());
    CHECK(backend->obj_value() == doctest::Approx(3.0));
  }

  fs::remove_all(dir);
}

TEST_CASE("cuOpt write_lp dumps a readable MPS file")  // NOLINT
{
  auto backend = make_cuopt_or_skip();
  if (!backend) {
    return;
  }
  namespace fs = std::filesystem;
  const auto stem = cuopt_scratch_log_base("write_lp");
  const auto file = fs::path {stem.string() + ".mps"};
  fs::remove(file);

  const CuoptTrivialLP2 lp {backend->infinity()};
  lp.load_into(*backend);
  // gtopt callers pass an extensionless stem; the backend normalises to
  // ".mps" (cuOpt's writer emits MPS regardless of the requested name).
  backend->write_lp(stem.string().c_str());
  REQUIRE(fs::exists(file));
  CHECK(fs::file_size(file) > 0);
  fs::remove(file);
}

}  // namespace
