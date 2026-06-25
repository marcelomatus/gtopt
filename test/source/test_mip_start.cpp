/**
 * @file      test_mip_start.cpp
 * @brief     Unit tests for the initial-MIP-solution (warm-start) framework
 * @date      2026-06-23
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers the solver-independent surface of the MIP-start framework: the
 * `MipStartOptions` JSON contract, the option enums (`MipStartMethod`,
 * `MipStartEffort`, `RelaxInfeasibleAction`) and their aliases, and the
 * generator factory.  The end-to-end relaxation→round→inject pipeline is
 * validated separately by the benchmark on a real cliff case.
 */

#include <array>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_enums.hpp>
#include <gtopt/solver_enums.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace mip_start_test  // NOLINT(misc-use-anonymous-namespace)
{

TEST_CASE("MipStart option enums round-trip by name")  // NOLINT
{
  SUBCASE("MipStartMethod")
  {
    CHECK(enum_name(MipStartMethod::none) == "none");
    CHECK(enum_name(MipStartMethod::lp_round) == "lp_round");
    CHECK(enum_name(MipStartMethod::relax_fix) == "relax_fix");
    CHECK(enum_from_name<MipStartMethod>("lp_round")
              .value_or(MipStartMethod::none)
          == MipStartMethod::lp_round);
  }

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
  opts.method = MipStartMethod::lp_round;
  opts.round_threshold = 0.6;
  opts.effort = MipStartEffort::repair;
  opts.relax_check = true;
  opts.on_infeasible = RelaxInfeasibleAction::feasopt;
  opts.report_saturated = true;

  const auto json_string = daw::json::to_json(opts);
  const auto back = daw::json::from_json<MipStartOptions>(json_string);

  CHECK(back.method.value_or(MipStartMethod::none) == MipStartMethod::lp_round);
  CHECK(back.round_threshold.value_or(-1.0) == doctest::Approx(0.6));
  CHECK(back.effort.value_or(MipStartEffort::check_feasibility)
        == MipStartEffort::repair);
  CHECK(back.relax_check.value_or(false) == true);
  CHECK(back.on_infeasible.value_or(RelaxInfeasibleAction::stop)
        == RelaxInfeasibleAction::feasopt);
  CHECK(back.report_saturated.value_or(false) == true);
}

TEST_CASE("MipStartOptions parses from a monolithic_options block")  // NOLINT
{
  // The feature is reachable via `--set monolithic_options.mip_start.*`; a
  // bare mip_start block must deserialize with the string enums resolved.
  constexpr std::string_view json = R"({
    "mip_start": {
      "method": "lp_round",
      "effort": "solve_fixed",
      "on_infeasible": "warn",
      "relax_check": true
    }
  })";
  const auto mono = daw::json::from_json<MonolithicOptions>(json);
  REQUIRE(mono.mip_start.has_value());
  CHECK(mono.mip_start->method.value_or(MipStartMethod::none)
        == MipStartMethod::lp_round);
  CHECK(mono.mip_start->effort.value_or(MipStartEffort::check_feasibility)
        == MipStartEffort::solve_fixed);
  CHECK(mono.mip_start->on_infeasible.value_or(RelaxInfeasibleAction::stop)
        == RelaxInfeasibleAction::warn);
  CHECK(mono.mip_start->relax_check.value_or(false) == true);
}

TEST_CASE("make_mip_start_generator factory")  // NOLINT
{
  SUBCASE("lp_round yields a named generator")
  {
    auto gen = make_mip_start_generator(MipStartMethod::lp_round);
    REQUIRE(gen != nullptr);
    CHECK(gen->name() == "lp_round");
  }

  SUBCASE("relax_fix yields a named generator")
  {
    auto gen = make_mip_start_generator(MipStartMethod::relax_fix);
    REQUIRE(gen != nullptr);
    CHECK(gen->name() == "relax_fix");
  }

  SUBCASE("none yields no generator")
  {
    CHECK(make_mip_start_generator(MipStartMethod::none) == nullptr);
  }
}

// ── Generator behaviour (solved-relaxation → rounded start) ───────────────
//
// These exercise the actual algorithm in `LpRoundMipStart` /
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

TEST_CASE("MipStart lp_round rounds integer columns by threshold")  // NOLINT
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
  auto gen = make_mip_start_generator(MipStartMethod::lp_round);
  REQUIRE(gen != nullptr);

  SUBCASE("threshold 0.5 — 0.7 rounds up, 0.2 rounds down")
  {
    MipStartOptions opts;
    opts.round_threshold = 0.5;
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
    opts.round_threshold = 0.8;
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

TEST_CASE(
    "MipStart lp_round repairs min-up/down run-length violations")  // NOLINT
{
  // Four unit-status columns rounding to on,on,off,on with min_down=2h and
  // 1h blocks: the single 1h OFF period is shorter than min_down, so the
  // greedy repair suppresses the off→on transition, extending the OFF run —
  // i.e. the last column is flipped 1→0.
  LinearInterface li;
  const auto s0 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  const auto s1 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  const auto s2 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  const auto s3 = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});
  pin_col(li, s0, 0.9);  // → 1
  pin_col(li, s1, 0.9);  // → 1
  pin_col(li, s2, 0.1);  // → 0
  pin_col(li, s3, 0.9);  // → 1 (repaired to 0)
  REQUIRE(li.initial_solve(SolverOptions {.log_level = 0}).has_value());
  REQUIRE(li.is_optimal());

  const std::array<int, 4> int_cols {0, 1, 2, 3};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .min_up_hours = 1.0,
      .min_down_hours = 2.0,
      .status_cols = {0, 1, 2, 3},
      .durations = {1.0, 1.0, 1.0, 1.0},
  }};
  MipStartOptions opts;
  opts.round_threshold = 0.5;
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = li,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = commitments};

  auto gen = make_mip_start_generator(MipStartMethod::lp_round);
  const auto start = gen->generate(ctx);
  REQUIRE(start.has_value());
  CHECK((*start)[0] == doctest::Approx(1.0));
  CHECK((*start)[1] == doctest::Approx(1.0));
  CHECK((*start)[2] == doctest::Approx(0.0));
  CHECK((*start)[3] == doctest::Approx(0.0));  // min-down repair flipped 1→0
}

TEST_CASE(
    "MipStart relax_fix re-solves dependent columns from the pinned "
    "commitment")  // NOLINT
{
  // c = 5·x ; x ≥ 0.6 ; minimise x ⇒ relaxation x = 0.6, c = 3.0.
  // relax_fix rounds x → 1, pins it, and re-solves ⇒ c = 5.0 (NOT the
  // relaxation's 3.0).  This is the distinguishing behaviour vs lp_round,
  // which would leave c at 3.0.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  const auto cc =
      li.add_col(SparseCol {.lowb = 0.0, .uppb = 10.0, .cost = 0.0});
  // c - 5x = 0
  const auto rbal = li.add_row(SparseRow {.lowb = 0.0, .uppb = 0.0});
  li.set_coeff(rbal, cc, 1.0);
  li.set_coeff(rbal, x, -5.0);
  // x >= 0.6
  const auto rlo =
      li.add_row(SparseRow {.lowb = 0.6, .uppb = SparseRow::DblMax});
  li.set_coeff(rlo, x, 1.0);
  REQUIRE(li.initial_solve(SolverOptions {.log_level = 0}).has_value());
  REQUIRE(li.is_optimal());
  // Relaxation: x = 0.6, c = 3.0.
  REQUIRE(li.get_col_sol_raw()[0] == doctest::Approx(0.6));
  REQUIRE(li.get_col_sol_raw()[1] == doctest::Approx(3.0));

  const std::array<int, 1> int_cols {0};
  MipStartOptions opts;
  opts.round_threshold = 0.5;
  const SolverOptions relax_opts {.log_level = 0};
  MipStartContext ctx {.li = li,
                       .relax_opts = relax_opts,
                       .int_cols = int_cols,
                       .opts = opts,
                       .commitments = {}};

  auto gen = make_mip_start_generator(MipStartMethod::relax_fix);
  REQUIRE(gen != nullptr);
  const auto start = gen->generate(ctx);
  REQUIRE(start.has_value());
  CHECK((*start)[0] == doctest::Approx(1.0));  // x rounded + pinned
  CHECK((*start)[1] == doctest::Approx(5.0));  // c recomputed = 5·1
  // Original (relaxed) bounds restored on the integer column.
  CHECK(li.get_col_low_raw()[0] == doctest::Approx(0.0));
  CHECK(li.get_col_upp_raw()[0] == doctest::Approx(1.0));
}

// ── apply_mip_start orchestrator ──────────────────────────────────────────

TEST_CASE("apply_mip_start: feature off is a no-op report")  // NOLINT
{
  // method=none + relax_check=false → returns immediately, nothing solved.
  LinearInterface li;
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  MipStartOptions opts;  // method unset (none), relax_check unset (false)
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
  // relax_check requested, but there are no integer columns → returns
  // before stage A (relaxation_solved stays false).  No solver needed.
  LinearInterface li;
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 5.0, .cost = 1.0});
  MipStartOptions opts;
  opts.relax_check = true;  // diagnosis requested
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK_FALSE(rep->relaxation_solved);  // bailed at the empty int_cols guard
  CHECK_FALSE(rep->injected);
}

TEST_CASE("apply_mip_start: lp_round injects on a feasible MIP relaxation")
// NOLINT
{
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }
  // Integer x in [0,1] with x >= 0.6, minimise x → relaxation x = 0.6
  // (feasible, obj 0.6).  Orchestrator: relax → solve → lp_round → restore
  // integrality → inject.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0, .cost = 1.0});
  li.set_integer(x);
  const auto r = li.add_row(SparseRow {.lowb = 0.6, .uppb = SparseRow::DblMax});
  li.set_coeff(r, x, 1.0);

  MipStartOptions opts;
  opts.method = MipStartMethod::lp_round;
  const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
  REQUIRE(rep.has_value());
  CHECK(rep->relaxation_solved);
  CHECK(rep->relaxation_feasible);
  REQUIRE(rep->relax_obj.has_value());
  CHECK(rep->relax_obj.value() == doctest::Approx(0.6));
  CHECK(rep->source == "lp_round");  // a start was produced
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
    opts.method = MipStartMethod::lp_round;
    opts.on_infeasible = RelaxInfeasibleAction::stop;
    const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
    CHECK_FALSE(rep.has_value());  // propagated as an Error
  }

  SUBCASE("on_infeasible=warn → report, no error, nothing injected")
  {
    MipStartOptions opts;
    opts.method = MipStartMethod::lp_round;
    opts.on_infeasible = RelaxInfeasibleAction::warn;
    const auto rep = apply_mip_start(li, SolverOptions {.log_level = 0}, opts);
    REQUIRE(rep.has_value());
    CHECK(rep->relaxation_solved);
    CHECK_FALSE(rep->relaxation_feasible);
    CHECK_FALSE(rep->injected);
    // Integrality restored even on the infeasible path.
    CHECK(li.is_integer(x));
  }
}

}  // namespace mip_start_test
