// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for OutputFlags bitmask enum and parse_output_flags.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <doctest/doctest.h>
#include <gtopt/output_context.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("OutputFlags - enum values and bitwise ops")  // NOLINT
{
  CHECK(std::to_underlying(OutputFlags::none) == 0);
  CHECK(std::to_underlying(OutputFlags::solution) == 1);
  CHECK(std::to_underlying(OutputFlags::dual) == 2);
  CHECK(std::to_underlying(OutputFlags::reduced_cost) == 4);
  CHECK(std::to_underlying(OutputFlags::extras) == 8);
  CHECK(std::to_underlying(OutputFlags::all) == 15);

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
  // Atomic-tuple parses to the OR of its atoms — NOT to `all`, which
  // since the `extras` atom landed is `sol|dual|rc|extras`.
  CHECK(parse_output_flags("solution, dual, reduced_cost")
        == (OutputFlags::solution | OutputFlags::dual
            | OutputFlags::reduced_cost));
  CHECK(parse_output_flags("sol|dual|rcost")
        == (OutputFlags::solution | OutputFlags::dual
            | OutputFlags::reduced_cost));
  CHECK(parse_output_flags("all") == OutputFlags::all);
  CHECK(parse_output_flags("dual,dual") == OutputFlags::dual);  // idempotent
  CHECK(parse_output_flags("extras") == OutputFlags::extras);
  CHECK(parse_output_flags("extra") == OutputFlags::extras);  // alias
  CHECK(parse_output_flags("sol,extras")
        == (OutputFlags::solution | OutputFlags::extras));
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

TEST_CASE(
    "PlanningOptionsLP::write_out - default is "
    "sol,dual,rc:Generator,Line (extras excluded)")  // NOLINT
{
  // Default emits every primal + every dual unscoped, but restricts
  // reduced costs to Generator + Line — the union of what every
  // current consumer reads.  `extras` stays off by default.  Users
  // who want every reduced cost on every class pass
  // `--write-out sol,dual,rc` (no scope); those who want every
  // stream including extras pass `--write-out all`.
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  const auto sel = wrapper.write_out();
  CHECK(sel.atoms
        == (OutputFlags::solution | OutputFlags::dual
            | OutputFlags::reduced_cost));
  CHECK_FALSE(has_flag(sel.atoms, OutputFlags::extras));
  CHECK(sel.sol_classes.empty());
  CHECK(sel.dual_classes.empty());
  REQUIRE(sel.rc_classes.size() == 2);
  CHECK(sel.rc_classes[0] == "Generator");
  CHECK(sel.rc_classes[1] == "Line");
  CHECK(sel.extra_classes.empty());

  // The default applies rc to Generator + Line but not to other classes.
  CHECK(sel.emits(OutputFlags::reduced_cost, "Generator"));
  CHECK(sel.emits(OutputFlags::reduced_cost, "Line"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Battery"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Demand"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Reservoir"));

  // sol / dual are unscoped — apply to every class.
  CHECK(sel.emits(OutputFlags::solution, "Battery"));
  CHECK(sel.emits(OutputFlags::dual, "Junction"));
}

TEST_CASE("PlanningOptionsLP::write_out - explicit overrides")  // NOLINT
{
  PlanningOptions opts;
  opts.write_out = OutputSelection {OutputFlags::solution | OutputFlags::dual};
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.write_out().atoms
        == (OutputFlags::solution | OutputFlags::dual));
}

TEST_CASE(
    "PlanningOptionsLP::write_out - explicit `all` includes extras")  // NOLINT
{
  PlanningOptions opts;
  opts.write_out = OutputSelection {OutputFlags::all};
  const PlanningOptionsLP wrapper(opts);
  CHECK(has_flag(wrapper.write_out().atoms, OutputFlags::extras));
}

TEST_CASE(
    "parse_output_selection - scoped reduced_cost (`rc:Generator,Line`) "
    "round-trips through `emits()`")  // NOLINT
{
  const auto sel = parse_output_selection("sol,dual,rc:Generator,Line");
  CHECK(has_flag(sel.atoms, OutputFlags::solution));
  CHECK(has_flag(sel.atoms, OutputFlags::dual));
  CHECK(has_flag(sel.atoms, OutputFlags::reduced_cost));
  CHECK_FALSE(has_flag(sel.atoms, OutputFlags::extras));

  // rc only applies to the two named classes.
  CHECK(sel.emits(OutputFlags::reduced_cost, "Generator"));
  CHECK(sel.emits(OutputFlags::reduced_cost, "Line"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Battery"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Demand"));

  // sol / dual are unscoped — apply to every class.
  CHECK(sel.emits(OutputFlags::solution, "Battery"));
  CHECK(sel.emits(OutputFlags::dual, "Junction"));
}

namespace
{
auto make_single_gen_system()
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
  };
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

  // With CSV format, each field lands under Generator/ as a separate
  // shard.  Filenames follow the convention `<col>_<tier>_<scen>_<stg>.csv`
  // where tier ∈ {sol, dual, cost}.  Column names may themselves
  // contain `_cost` (e.g. `vom_cost`, `fuel_cost`) — so we identify
  // the tier by its POSITION (third-from-last `_`-separated token of
  // the stem), not a substring match.
  const auto tier_of = [](const std::string& filename) -> std::string
  {
    auto stem = filename;
    if (const auto dot = stem.find_last_of('.'); dot != std::string::npos) {
      stem = stem.substr(0, dot);
    }
    // Stem layout: <col>_<tier>_<scen>_<stg> → split on `_` and take
    // the third-from-last token.
    std::vector<std::string> parts;
    std::string token;
    for (auto c : stem) {
      if (c == '_') {
        parts.push_back(std::move(token));
        token.clear();
      } else {
        token.push_back(c);
      }
    }
    parts.push_back(std::move(token));
    if (parts.size() < 3) {
      return {};
    }
    return parts[parts.size() - 3];
  };

  const auto gen_dir = tmpdir / "Generator";
  REQUIRE(std::filesystem::exists(gen_dir));
  bool saw_sol = false;
  bool saw_dual_or_cost = false;
  for (const auto& entry : std::filesystem::directory_iterator(gen_dir)) {
    const auto name = entry.path().filename().string();
    const auto tier = tier_of(name);
    if (tier == "sol") {
      saw_sol = true;
    } else if (tier == "dual" || tier == "cost") {
      saw_dual_or_cost = true;
    }
  }
  CHECK(saw_sol);
  CHECK(!saw_dual_or_cost);

  std::filesystem::remove_all(tmpdir);
}
