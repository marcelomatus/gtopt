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

#include <doctest/doctest.h>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_enums.hpp>
#include <gtopt/solver_enums.hpp>

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

}  // namespace mip_start_test
