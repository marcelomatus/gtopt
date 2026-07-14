/**
 * @file      test_mip_start.cpp
 * @brief     Unit tests for the initial-MIP-solution (warm-start) framework
 * @date      2026-06-23
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers the solver-independent surface of the MIP-start framework: the
 * staged `MipStartOptions` JSON contract, the option enums (`MipStartEffort`,
 * `RelaxInfeasibleAction`) and their aliases, and the generator factory.  The
 * end-to-end relaxation→round→inject pipeline is validated separately by the
 * benchmark on a real cliff case.
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_enums.hpp>
#include <gtopt/solver_enums.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;

namespace mip_start_test  // NOLINT(misc-use-anonymous-namespace)
{

TEST_CASE("MipStart option enums round-trip by name")  // NOLINT
{
  SUBCASE("MipStartEffort")
  {
    CHECK(enum_name(MipStartEffort::check_feasibility) == "check_feasibility");
    CHECK(enum_name(MipStartEffort::solve_fixed) == "solve_fixed");
    CHECK(enum_name(MipStartEffort::repair) == "repair");
    CHECK(enum_from_name<MipStartEffort>("repair").value_or(
              MipStartEffort::check_feasibility)
          == MipStartEffort::repair);
  }

  SUBCASE("RelaxInfeasibleAction + diagnose alias")
  {
    CHECK(enum_name(RelaxInfeasibleAction::stop) == "stop");
    CHECK(enum_name(RelaxInfeasibleAction::feasopt) == "feasopt");
    CHECK(enum_from_name<RelaxInfeasibleAction>("warn").value_or(
              RelaxInfeasibleAction::stop)
          == RelaxInfeasibleAction::warn);
    // `diagnose` is an alias for `feasopt`.
    CHECK(enum_from_name<RelaxInfeasibleAction>("diagnose")
              .value_or(RelaxInfeasibleAction::stop)
          == RelaxInfeasibleAction::feasopt);
  }
}

TEST_CASE("MipStartOptions JSON round-trip")  // NOLINT
{
  MipStartOptions opts;
  opts.enabled = true;
  opts.round.threshold = 0.6;
  opts.inject.effort = MipStartEffort::repair;
  opts.from_file = "in.start";
  opts.dump_file = "out.start";
  opts.scip_repair.enabled = true;
  opts.relax.check = true;
  opts.relax.on_infeasible = RelaxInfeasibleAction::feasopt;
  opts.relax.report_saturated = true;
  opts.elastic = true;
  opts.checkpoint_gap = 0.025;
  opts.checkpoint_file = "ckpt.start";

  const auto json_string = daw::json::to_json(opts);
  const auto back = daw::json::from_json<MipStartOptions>(json_string);

  CHECK(back.enabled.value_or(false) == true);
  CHECK(back.round.threshold.value_or(-1.0) == doctest::Approx(0.6));
  CHECK(back.inject.effort.value_or(MipStartEffort::check_feasibility)
        == MipStartEffort::repair);
  CHECK(back.from_file.value_or("") == "in.start");
  CHECK(back.dump_file.value_or("") == "out.start");
  CHECK(back.scip_repair.enabled.value_or(false) == true);
  CHECK(back.relax.check.value_or(false) == true);
  CHECK(back.relax.on_infeasible.value_or(RelaxInfeasibleAction::stop)
        == RelaxInfeasibleAction::feasopt);
  CHECK(back.relax.report_saturated.value_or(false) == true);
  CHECK(back.elastic.value_or(false) == true);
  CHECK(back.checkpoint_gap.value_or(-1.0) == doctest::Approx(0.025));
  CHECK(back.checkpoint_file.value_or("") == "ckpt.start");
}

TEST_CASE("MipStartOptions parses from a monolithic_options block")  // NOLINT
{
  // The feature is reachable via `--set monolithic_options.mip_start.*`; a
  // bare mip_start block must deserialize the staged shape with the string
  // enums resolved.
  constexpr std::string_view json = R"({
    "mip_start": {
      "enabled": true,
      "inject": { "effort": "solve_fixed" },
      "relax": { "on_infeasible": "warn", "check": true }
    }
  })";
  const auto mono = daw::json::from_json<MonolithicOptions>(json);
  REQUIRE(mono.mip_start.has_value());
  CHECK(mono.mip_start->enabled.value_or(false) == true);
  CHECK(
      mono.mip_start->inject.effort.value_or(MipStartEffort::check_feasibility)
      == MipStartEffort::solve_fixed);
  CHECK(
      mono.mip_start->relax.on_infeasible.value_or(RelaxInfeasibleAction::stop)
      == RelaxInfeasibleAction::warn);
  CHECK(mono.mip_start->relax.check.value_or(false) == true);
}

TEST_CASE("make_mip_start_generator factory")  // NOLINT
{
  SUBCASE("default (no from_file) yields the round+rules generator")
  {
    MipStartOptions opts;
    auto gen = make_mip_start_generator(opts);
    REQUIRE(gen != nullptr);
    CHECK(gen->name() == "warmstart");
  }

  SUBCASE("from_file set yields the file generator")
  {
    MipStartOptions opts;
    opts.from_file = "some.start";
    auto gen = make_mip_start_generator(opts);
    REQUIRE(gen != nullptr);
    CHECK(gen->name() == "file");
  }
}

// ── Generator behaviour (solved-relaxation → rounded start) ───────────────
//
// These exercise the actual algorithm in `WarmStartGenerator` /
// `RelaxFixMipStart` (and the internal `round_with_threshold` /
// `repair_run_lengths` helpers) through the public `generate()` surface.
// A tiny LP is built directly on a LinearInterface, solved as a
// continuous relaxation with known column values, then handed to the
// generator.  `int_cols` are the columns to round (kept continuous here;
// the generator only needs their indices).  Any LP solver suffices — no
// MIP solver required.

// Pin column `col` to the constant `v` with an equality row, so the
// relaxation has a deterministic value at that column.
namespace
{
void pin_col(LinearInterface& li, ColIndex col, double v)
{
  const auto r = li.add_row(SparseRow {.lowb = v, .uppb = v});
  li.set_coeff(r, col, 1.0);
}
}  // namespace

TEST_CASE("MipStart warmstart rounds integer columns by threshold")  // NOLINT
{
  LinearInterface li;
  // cols 0,1 are the "integer" columns to round; col 2 is a pure
  // continuous column the generator must leave untouched.
  const auto x0 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
  const auto x1 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
  const auto cc = li.add_col(SparseCol {.lowb = 0.0, .uppb = 5.0, .cost = 0.0});
  pin_col(li, x0, 0.7);
  pin_col(li, x1, 0.2);
  pin_col(li, cc, 3.0);
  REQUIRE(li.get_numcols() == 3);
  REQUIRE(li.initial_solve(SolverOptions {.log_level = 0}).has_value());
  REQUIRE(li.is_optimal());

  const std::array<int, 2> int_cols {0, 1};
  const SolverOptions relax_opts {.log_level = 0};
  auto gen = make_mip_start_generator(MipStartOptions {});
  REQUIRE(gen != nullptr);

  SUBCASE("threshold 0.5 — 0.7 rounds up, 0.2 rounds down")
  {
    MipStartOptions opts;
    opts.round.threshold = 0.5;
    MipStartContext ctx {.li = li,
                         .relax_opts = relax_opts,
                         .int_cols = int_cols,
                         .opts = opts,
                         .commitments = {}};
    const auto start = gen->generate(ctx);
    REQUIRE(start.has_value());
    REQUIRE(start->size() == 3);
    CHECK((*start)[0] == doctest::Approx(1.0));  // 0.7 → 1
    CHECK((*start)[1] == doctest::Approx(0.0));  // 0.2 → 0
    CHECK((*start)[2] == doctest::Approx(3.0));  // continuous, untouched
  }

  SUBCASE("threshold 0.8 — 0.7 now rounds down")
  {
    MipStartOptions opts;
    opts.round.threshold = 0.8;
    MipStartContext ctx {.li = li,
                         .relax_opts = relax_opts,
                         .int_cols = int_cols,
                         .opts = opts,
                         .commitments = {}};
    const auto start = gen->generate(ctx);
    REQUIRE(start.has_value());
    CHECK((*start)[0] == doctest::Approx(0.0));  // 0.7 < 0.8 → 0
    CHECK((*start)[1] == doctest::Approx(0.0));  // 0.2 → 0
  }
}

TEST_CASE("MipStart warmstart rounds without repairing run lengths")  // NOLINT
{
  // Four unit-status columns rounding to on,on,off,on.  The in-tree
  // min-up/down repair was removed (2026-07-14) — the seed PRODUCER owns
  // commitment feasibility — so the generator now returns the pure rounded
  // pattern verbatim.
  LinearInterface li;
  const auto s0 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  const auto s1 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  const auto s2 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  const auto s3 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  pin_col(li, s0, 0.9);  // → 1
  pin_col(li, s1, 0.9);  // → 1
  pin_col(li, s2, 0.1);  // → 0
  pin_col(li, s3, 0.9);  // → 1 (kept: no repair)
  REQUIRE(li.initial_solve(SolverOptions {.log_level = 0}).has_value());
  REQUIRE(li.is_optimal());

  const std::array<int, 4> int_cols {0, 1, 2, 3};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .status_cols = {0, 1, 2, 3},
  }};
  MipStartOptions opts;
  opts.round.threshold = 0.5;
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = li,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = commitments};

  auto gen = make_mip_start_generator(opts);
  const auto start = gen->generate(ctx);
  REQUIRE(start.has_value());
  CHECK((*start)[0] == doctest::Approx(1.0));
  CHECK((*start)[1] == doctest::Approx(1.0));
  CHECK((*start)[2] == doctest::Approx(0.0));
  CHECK((*start)[3] == doctest::Approx(1.0));  // rounded verbatim
}

// ── apply_mip_start orchestrator ──────────────────────────────────────────

TEST_CASE("apply_mip_start: feature off is a no-op report")  // NOLINT
{
  // enabled=false + relax.check=false → returns immediately, nothing solved.
  LinearInterface li;
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  MipStartOptions opts;  // enabled unset (false), relax.check unset (false)
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK_FALSE(rep->relaxation_solved);
  CHECK_FALSE(rep->relaxation_feasible);
  CHECK_FALSE(rep->injected);
  CHECK(rep->source.empty());
}

TEST_CASE("apply_mip_start: pure LP (no integer cols) skips before solving")
// NOLINT
{
  // relax.check requested, but there are no integer columns → returns
  // before stage A (relaxation_solved stays false).  No solver needed.
  LinearInterface li;
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 5.0, .cost = 1.0});
  MipStartOptions opts;
  opts.relax.check = true;  // diagnosis requested
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK_FALSE(rep->relaxation_solved);  // bailed at the empty int_cols guard
  CHECK_FALSE(rep->injected);
}

TEST_CASE("apply_mip_start: warmstart injects on a feasible MIP relaxation")
// NOLINT
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // Integer x in [0,1] with x >= 0.6, minimise x → relaxation x = 0.6
  // (feasible, obj 0.6).  Orchestrator: relax → solve → warmstart → restore
  // integrality → inject.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 0.6, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  MipStartOptions opts;
  opts.enabled = true;
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK(rep->relaxation_solved);
  CHECK(rep->relaxation_feasible);
  REQUIRE(rep->relax_obj.has_value());
  CHECK(rep->relax_obj.value() == doctest::Approx(0.6));
  CHECK(rep->source == "warmstart");  // a start was produced
  // Integrality is re-established before returning (restore_integers) —
  // also guards the CPLEX restore_integers fix in the orchestrator path.
  CHECK(li.is_integer(x));
}

TEST_CASE(
    "apply_mip_start: infeasible relaxation with on_infeasible=stop errors")
// NOLINT
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // Integer x in [0,1] but x >= 2 → relaxation infeasible.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 2.0, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  SUBCASE("on_infeasible=stop → Error (caller must not proceed)")
  {
    MipStartOptions opts;
    opts.enabled = true;
    opts.relax.on_infeasible = RelaxInfeasibleAction::stop;
    const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
    CHECK_FALSE(rep.has_value());  // propagated as an Error
  }

  SUBCASE("on_infeasible=warn → report, no error, nothing injected")
  {
    MipStartOptions opts;
    opts.enabled = true;
    opts.relax.on_infeasible = RelaxInfeasibleAction::warn;
    const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
    REQUIRE(rep.has_value());
    CHECK(rep->relaxation_solved);
    CHECK_FALSE(rep->relaxation_feasible);
    CHECK_FALSE(rep->injected);
    // Integrality restored even on the infeasible path.
    CHECK(li.is_integer(x));
  }
}

TEST_CASE(  // NOLINT
    "apply_mip_start: infeasible relaxation with on_infeasible=feasopt errors")
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // Integer x in [0,1] but x >= 2 → relaxation infeasible.  The `feasopt`
  // policy runs `diagnose_infeasibility()` (CPLEX conflict refiner) for the
  // log, then — like `stop` — propagates the infeasibility as an Error so the
  // caller must NOT proceed to the MIP solve.  If the only solver lacks a
  // conflict refiner the diagnose is a no-op but the Error is still returned,
  // which is the assertion.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 2.0, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  MipStartOptions opts;
  opts.enabled = true;
  opts.relax.on_infeasible = RelaxInfeasibleAction::feasopt;
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  CHECK_FALSE(rep.has_value());  // feasopt diagnoses then propagates an Error
  // Integrality is restored before the Error is returned.
  CHECK(li.is_integer(x));
}

TEST_CASE(  // NOLINT
    "apply_mip_start: report_saturated on a feasible binding relaxation")
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // Integer x in [0,1], x >= 0.6, minimise x → relaxation x = 0.6 with the
  // x>=0.6 row BINDING (nonzero dual), so report_saturated has a row to log.
  // enabled=false + relax.check=true → diagnosis-only (no injection).
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 0.6, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  MipStartOptions opts;
  opts.relax.check = true;
  opts.relax.report_saturated = true;
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK(rep->relaxation_solved);
  CHECK(rep->relaxation_feasible);
  REQUIRE(rep->relax_obj.has_value());
  CHECK(rep->relax_obj.value() == doctest::Approx(0.6));
  CHECK_FALSE(rep->injected);  // diagnosis-only run
  // Integrality restored after the diagnosis-only run.
  CHECK(li.is_integer(x));
}

TEST_CASE(  // NOLINT
    "apply_mip_start: relax_check-only with an integer column restores "
    "integrality")
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // enabled=false + relax.check=true, with a real integer column and a
  // feasible relaxation: stage A solves the relaxation and reports feasibility,
  // but no start is generated (injected stays false) and integrality restored.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 0.4, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  MipStartOptions opts;
  opts.relax.check = true;
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK(rep->relaxation_solved);
  CHECK(rep->relaxation_feasible);
  REQUIRE(rep->relax_obj.has_value());
  CHECK(rep->relax_obj.value() == doctest::Approx(0.4));
  CHECK_FALSE(rep->injected);  // not enabled → nothing injected
  CHECK(li.is_integer(x));  // integrality re-established before return
}

TEST_CASE(  // NOLINT
    "apply_mip_start: relax_solver_options overlay still solves the "
    "relaxation")
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // The relaxation solve overlays `opts.relax.solver_options` on top of the
  // base options — pick a distinct algorithm (primal simplex) and assert the
  // relaxation still solves feasibly with the configured method.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 0.6, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  MipStartOptions opts;
  opts.enabled = true;
  SolverOptions relax_overlay {.log_level = 0};
  relax_overlay.algorithm = LPAlgo::primal;  // distinct from the base default
  opts.relax.solver_options = relax_overlay;
  // Base options use the dual algorithm; the overlay must take effect for the
  // relaxation without breaking the solve.
  const auto rep = apply_mip_start(
      li, SolverOptions {.algorithm = LPAlgo::dual, .log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK(rep->relaxation_solved);
  CHECK(rep->relaxation_feasible);
  REQUIRE(rep->relax_obj.has_value());
  CHECK(rep->relax_obj.value() == doctest::Approx(0.6));
  CHECK(rep->source == "warmstart");
  CHECK(li.is_integer(x));
}

// ── FileMipStart consumer: overlay a dump onto the relaxation base ─────────
//
// The `file` generator must take integer-column values FROM the dump and keep
// the continuous columns AT the destination LP's own relaxation value.  Drive
// it with a hand-written dump (the exact format `dump_integer_solution` emits)
// so the consumer is exercised with no MIP solver: one LP relaxation suffices.

TEST_CASE(
    "MipStart file generator overlays integers onto relaxation")  // NOLINT
{
  const std::filesystem::path path = "test_mip_start_overlay.start";
  {
    std::ofstream out(path, std::ios::trunc);
    out << "# gtopt mip_start integer solution (index value)\n";
    out << "ncols 3\n";
    out << "nint 2\n";
    out << "0 1\n";  // col 0 → 1
    out << "1 0\n";  // col 1 → 0
  }

  // Destination LP: a DIFFERENT (fractional) continuous relaxation.
  LinearInterface dst;
  const auto y0 =
      dst.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
  const auto y1 =
      dst.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
  const auto dd =
      dst.add_col(SparseCol {.lowb = 0.0, .uppb = 5.0, .cost = 0.0});
  pin_col(dst, y0, 0.3);  // overwritten by the file's integer (→ 1)
  pin_col(dst, y1, 0.7);  // overwritten by the file's integer (→ 0)
  pin_col(dst, dd, 2.0);  // continuous — must keep the dst relaxation value
  REQUIRE(dst.initial_solve(SolverOptions {.log_level = 0}).has_value());
  REQUIRE(dst.is_optimal());

  const std::array<int, 2> int_cols {0, 1};
  MipStartOptions opts;
  opts.from_file = path.string();
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = dst,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = {}};

  auto gen = make_mip_start_generator(opts);
  REQUIRE(gen != nullptr);
  const auto start = gen->generate(ctx);
  REQUIRE(start.has_value());
  const std::vector<double> sv = start.value_or(std::vector<double> {});
  REQUIRE(sv.size() == 3);
  CHECK(sv[0] == doctest::Approx(1.0));  // from the dump
  CHECK(sv[1] == doctest::Approx(0.0));  // from the dump
  CHECK(sv[2] == doctest::Approx(2.0));  // dst continuous, untouched

  std::filesystem::remove(path);
}

// ── dump → file full round-trip (production ordering: integers set, then a
// real MIP solve, then dump).  Gated on a MIP solver because the dump must
// read a SOLVED integer primal — `set_integer` before the solve is exactly the
// production path, and re-reading the cached primal after a post-hoc
// `set_integer` is invalid on some backends.  The dump is COMPLETE (integer
// and continuous columns), so the replay reconstructs the source solution
// verbatim — including the continuous values, which take precedence over the
// destination's own relaxation primal.
TEST_CASE("MipStart dump → file round-trips a solved MIP")  // NOLINT
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // This test asserts an INTEGER primal straight out of initial_solve(), so it
  // needs a solver that solves the MIP as posed in initial_solve()
  // (cplex/highs/gurobi/mindopt/scip).  The COIN backend does not: `clp` is
  // LP-only and `cbc`'s branch-and-bound lives in resolve(), so initial_solve()
  // returns the (fractional) LP relaxation.  `has_mip_solver()` is true when
  // cbc is present, so gate explicitly on the COIN solvers — and on the SAME
  // solver the default-constructed LinearInterface below will use.  (On a
  // COIN-only CI the default resolves to clp, not cbc, so both must be gated.)
  const auto ds = reg.default_solver();
  if (ds == "cbc" || ds == "clp") {
    MESSAGE("Skipping — COIN (clp/cbc) initial_solve does not solve the MIP");
    return;
  }
  const std::filesystem::path path = "test_mip_start_roundtrip.start";
  std::filesystem::remove(path);

  // Source MIP: x0,x1 binary, x0>=0.6 (→1), x1<=0.4 (→0); cc pinned at 3.
  {
    LinearInterface src;
    const auto x0 =
        src.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
    const auto x1 =
        src.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
    const auto cc =
        src.add_col(SparseCol {.lowb = 0.0, .uppb = 5.0, .cost = 0.0});
    src.set_integer(x0);  // integrality set BEFORE solving (production order)
    src.set_integer(x1);
    const auto rlo =
        src.add_row(SparseRow {.lowb = 0.6, .uppb = SparseRow::DblMax});
    src.set_coeff(rlo, x0, 1.0);  // x0 >= 0.6 → 1
    const auto rhi = src.add_row(SparseRow {.lowb = 0.0, .uppb = 0.4});
    src.set_coeff(rhi, x1, 1.0);  // x1 <= 0.4 → 0
    pin_col(src, cc, 3.0);
    REQUIRE(src.initial_solve(SolverOptions {.log_level = 0}).has_value());
    REQUIRE(src.is_optimal());
    REQUIRE(src.get_col_sol_raw()[0] == doctest::Approx(1.0));
    REQUIRE(src.get_col_sol_raw()[1] == doctest::Approx(0.0));

    const auto dumped = dump_integer_solution(src, path.string());
    REQUIRE(dumped.has_value());
  }

  // Destination LP: different continuous base; replay the dumped integers.
  LinearInterface dst;
  const auto y0 =
      dst.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
  const auto y1 =
      dst.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 0.0});
  const auto dd =
      dst.add_col(SparseCol {.lowb = 0.0, .uppb = 5.0, .cost = 0.0});
  pin_col(dst, y0, 0.3);
  pin_col(dst, y1, 0.7);
  pin_col(dst, dd, 2.0);
  REQUIRE(dst.initial_solve(SolverOptions {.log_level = 0}).has_value());

  const std::array<int, 2> int_cols {0, 1};
  MipStartOptions opts;
  opts.from_file = path.string();
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = dst,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = {}};
  auto gen = make_mip_start_generator(opts);
  REQUIRE(gen != nullptr);
  const auto start = gen->generate(ctx);
  REQUIRE(start.has_value());
  const std::vector<double> sv = start.value_or(std::vector<double> {});
  REQUIRE(sv.size() == 3);
  CHECK(sv[0] == doctest::Approx(1.0));  // dumped MIP value
  CHECK(sv[1] == doctest::Approx(0.0));  // dumped MIP value
  CHECK(sv[2] == doctest::Approx(3.0));  // dumped continuous value —
  // the complete dump wins over the dst relaxation (2.0): replaying it must
  // reconstruct the SOURCE solution, not an integers-over-relaxation mix.

  std::filesystem::remove(path);
}

TEST_CASE("MipStart file generator rejects an ncols mismatch")  // NOLINT
{
  // A dump built for a 3-column LP must NOT be replayed onto a 2-column LP —
  // the `ncols` header guard returns nullopt rather than silently injecting a
  // start built for a different problem.
  const std::filesystem::path path = "test_mip_start_ncols_mismatch.start";
  {
    std::ofstream out(path, std::ios::trunc);
    out << "# gtopt mip_start integer solution (index value)\n";
    out << "ncols 3\n";
    out << "nint 1\n";
    out << "0 1\n";
  }

  LinearInterface dst;  // only TWO columns
  (void)dst.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  (void)dst.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  pin_col(dst, ColIndex {0}, 0.5);
  pin_col(dst, ColIndex {1}, 0.5);
  REQUIRE(dst.initial_solve(SolverOptions {.log_level = 0}).has_value());

  const std::array<int, 1> int_cols {0};
  MipStartOptions opts;
  opts.from_file = path.string();
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = dst,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = {}};

  auto gen = make_mip_start_generator(opts);
  REQUIRE(gen != nullptr);
  CHECK_FALSE(gen->generate(ctx).has_value());  // ncols 3 ≠ 2 → refused

  std::filesystem::remove(path);
}

}  // namespace mip_start_test

TEST_CASE("seed_only_start assembles without a solved relaxation")  // NOLINT
{
  // The skip_relaxation path is pure assembly: zero base + seed overlay by
  // (generator, block) identity — no LP solve involved, no solver needed.
  const auto path = (std::filesystem::temp_directory_path()
                     / "gtopt_seed_only_start_test.csv")
                        .string();
  {
    std::ofstream ofs(path);
    ofs << "generator_uid,block_uid,u\n";
    ofs << "7,10,1\n";
    ofs << "7,11,0\n";
  }
  LinearInterface li;
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});  // u @ block 10
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});  // u @ block 11
  (void)li.add_col(SparseCol {.lowb = 0.0});  // continuous dispatch

  const std::array<int, 2> int_cols {0, 1};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .uid = Uid {7},
      .status_cols = {0, 1},
      .block_uids = {Uid {10}, Uid {11}},
  }};
  MipStartOptions opts;
  opts.seed_solution_file = path;
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = li,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = commitments};

  const auto start = detail::seed_only_start(ctx, "seed");
  std::filesystem::remove(path);
  REQUIRE(start.size() == 3);
  CHECK(start[0] == doctest::Approx(1.0));  // seeded ON
  CHECK(start[1] == doctest::Approx(0.0));  // seeded OFF
  CHECK(start[2] == doctest::Approx(0.0));  // continuous zero base
}
