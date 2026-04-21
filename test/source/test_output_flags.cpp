// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for OutputFlags bitmask enum and parse_output_flags.

#include <filesystem>
#include <stdexcept>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/output_context.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("OutputFlags - enum values and bitwise ops")  // NOLINT
{
  CHECK(static_cast<uint8_t>(OutputFlags::none) == 0);
  CHECK(static_cast<uint8_t>(OutputFlags::solution) == 1);
  CHECK(static_cast<uint8_t>(OutputFlags::dual) == 2);
  CHECK(static_cast<uint8_t>(OutputFlags::reduced_cost) == 4);
  CHECK(static_cast<uint8_t>(OutputFlags::all) == 7);

  const auto both = OutputFlags::solution | OutputFlags::dual;
  CHECK(has_flag(both, OutputFlags::solution));
  CHECK(has_flag(both, OutputFlags::dual));
  CHECK(!has_flag(both, OutputFlags::reduced_cost));

  CHECK(has_flag(OutputFlags::all, OutputFlags::solution));
  CHECK(has_flag(OutputFlags::all, OutputFlags::dual));
  CHECK(has_flag(OutputFlags::all, OutputFlags::reduced_cost));

  CHECK(!has_flag(OutputFlags::none, OutputFlags::solution));
}

TEST_CASE("parse_output_flags - canonical names")  // NOLINT
{
  CHECK(parse_output_flags("none") == OutputFlags::none);
  CHECK(parse_output_flags("solution") == OutputFlags::solution);
  CHECK(parse_output_flags("dual") == OutputFlags::dual);
  CHECK(parse_output_flags("reduced_cost") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("all") == OutputFlags::all);
}

TEST_CASE("parse_output_flags - abbreviations")  // NOLINT
{
  CHECK(parse_output_flags("sol") == OutputFlags::solution);
  CHECK(parse_output_flags("cost") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("rcost") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("rc") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("reduced-cost") == OutputFlags::reduced_cost);
}

TEST_CASE("parse_output_flags - combinations")  // NOLINT
{
  CHECK(parse_output_flags("sol,dual")
        == (OutputFlags::solution | OutputFlags::dual));
  CHECK(parse_output_flags("solution, dual, reduced_cost") == OutputFlags::all);
  CHECK(parse_output_flags("sol|dual|rcost") == OutputFlags::all);
  CHECK(parse_output_flags("dual,dual") == OutputFlags::dual);  // idempotent
}

TEST_CASE("parse_output_flags - case-insensitive")  // NOLINT
{
  CHECK(parse_output_flags("SOL") == OutputFlags::solution);
  CHECK(parse_output_flags("Dual") == OutputFlags::dual);
  CHECK(parse_output_flags("REDUCED_COST") == OutputFlags::reduced_cost);
}

TEST_CASE("parse_output_flags - empty input yields none")  // NOLINT
{
  CHECK(parse_output_flags("") == OutputFlags::none);
  CHECK(parse_output_flags("  , ,") == OutputFlags::none);
}

TEST_CASE("parse_output_flags - unknown token throws")  // NOLINT
{
  CHECK_THROWS_AS((void)parse_output_flags("marginal"), std::invalid_argument);
  CHECK_THROWS_AS((void)parse_output_flags("sol,bogus"), std::invalid_argument);
}

TEST_CASE("output_flags_to_string - round-trip")  // NOLINT
{
  CHECK(output_flags_to_string(OutputFlags::none) == "none");
  CHECK(output_flags_to_string(OutputFlags::all) == "all");
  CHECK(output_flags_to_string(OutputFlags::solution) == "solution");
  CHECK(output_flags_to_string(OutputFlags::solution | OutputFlags::dual)
        == "solution,dual");

  const auto combined = OutputFlags::solution | OutputFlags::reduced_cost;
  CHECK(parse_output_flags(output_flags_to_string(combined)) == combined);
}

TEST_CASE("PlanningOptionsLP::write_out - default is all")  // NOLINT
{
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.write_out() == OutputFlags::all);
}

TEST_CASE("PlanningOptionsLP::write_out - explicit overrides")  // NOLINT
{
  PlanningOptions opts;
  opts.write_out = OutputFlags::solution | OutputFlags::dual;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.write_out() == (OutputFlags::solution | OutputFlags::dual));
}

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
auto make_single_gen_system()
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {{
      .uid = Uid {1},
      .name = "g1",
      .bus = Uid {1},
      .gcost = 50.0,
      .capacity = 300.0,
  }};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "OutFlagsTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  return std::pair {system, simulation};
}
}  // namespace

TEST_CASE("OutputContext - write_out=none skips all output fields")  // NOLINT
{
  auto [system, simulation] = make_single_gen_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_output_flags_none";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.write_out = OutputFlags::none;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  auto result = system_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  system_lp.write_out();

  // With OutputFlags::none, no element-class directories should be written.
  CHECK(!std::filesystem::exists(tmpdir / "Generator"));
  CHECK(!std::filesystem::exists(tmpdir / "Demand"));

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(
    "OutputContext - solution-only emits sol and skips dual/cost")  // NOLINT
{
  auto [system, simulation] = make_single_gen_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_output_flags_sol_only";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.write_out = OutputFlags::solution;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);
  auto result = system_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  system_lp.write_out();

  // With CSV format, each field lands under Generator/ as a separate shard.
  // With solution-only, shards ending in "_sol_*" must exist while "_dual_*"
  // and "_cost_*" must not.
  const auto gen_dir = tmpdir / "Generator";
  REQUIRE(std::filesystem::exists(gen_dir));
  bool saw_sol = false;
  bool saw_dual_or_cost = false;
  for (const auto& entry : std::filesystem::directory_iterator(gen_dir)) {
    const auto name = entry.path().filename().string();
    if (name.contains("_sol_")) {
      saw_sol = true;
    }
    if (name.contains("_dual_") || name.contains("_cost_")) {
      saw_dual_or_cost = true;
    }
  }
  CHECK(saw_sol);
  CHECK(!saw_dual_or_cost);

  std::filesystem::remove_all(tmpdir);
}
