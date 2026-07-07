// SPDX-License-Identifier: BSD-3-Clause
//
// Integration tests for the AMPL resolver consulting the naming-dialects
// registry.  Previous turn wired `canonicalize_json_keys` into
// `parse_planning_json`; this turn made `resolve_single_param` look up
// alias attribute names via the same `NamesRegistry`.
//
// These tests build two end-to-end Planning fixtures with identical
// structure but different attribute spellings (canonical vs alias)
// inside the user-constraint expression, solve both, and assert the
// LP objective matches.  A regression that breaks the resolver's
// canonicalisation path (or drops the `tmax` legacy alias from the
// registry) makes the alias-side constraint expression resolve to
// `std::nullopt`, which `resolve_single_param`'s caller treats as
// "parameter missing → constraint trivially satisfied" — and the
// objective diverges.

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

/// Helper: build a Planning where the user-constraint expression uses
/// `attr_name` as the Generator parameter (gcost vs marginal_cost).
/// Returns the solved objective value.
[[nodiscard]] double solve_generator_param_fixture(std::string_view attr_name)
{
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .fcost = 100.0,
          .capacity = 100.0,
      },
  };
  // demand.load + generator.<attr> <= 55  ⇒  load <= 50.
  // With gcost = 5 (cheap) and demand 100 MW, LP serves 50 MW (cost
  // 50) and pays fail on 50 MW at fail_cost 100 (cost 5000).  Same
  // algebra whether the param is referenced as `gcost` or
  // `marginal_cost` — that's the invariant we are pinning.
  const std::string expression =
      std::format("demand('d1').load + generator('g1').{} <= 55", attr_name);
  const Array<UserConstraint> user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "gen_param_cap",
          .expression = expression,
      },
  };
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 100.0;

  const System system {
      .name = "ResolverAliasFixture",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .user_constraint_array = user_constraint_array,
  };
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  return lp.get_obj_value();
}

/// Helper: same idea for a Line parameter (`tmax_ab` canonical vs the
/// legacy `tmax` alias that used to be hardcoded in the resolver's
/// `tmax || tmax_ab` chain — now routed through the registry).
[[nodiscard]] double solve_line_param_fixture(std::string_view attr_name)
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .fcost = 100.0,
          .capacity = 100.0,
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.02,
          .tmax_ba = 300.0,
          .tmax_ab = 300.0,
      },
  };
  // line('l1').flow + line('l1').<attr> <= 500
  // With tmax_ab = 300, this becomes flow <= 200.  Either spelling
  // (`tmax_ab` canonical, `tmax` legacy alias) must resolve to the
  // same parameter and yield the same constraint.
  const std::string expression =
      std::format("line('l1').flow + line('l1').{} <= 500", attr_name);
  const Array<UserConstraint> user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "line_param_cap",
          .expression = expression,
      },
  };
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 100.0;
  popts.model_options.use_kirchhoff = true;
  popts.model_options.use_single_bus = false;

  const System system {
      .name = "ResolverLineAliasFixture",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
      .user_constraint_array = user_constraint_array,
  };
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  return lp.get_obj_value();
}

}  // namespace

TEST_CASE(  // NOLINT
    "PAMPL resolver — generator.marginal_cost resolves identically to gcost")
{
  const auto canonical = solve_generator_param_fixture("gcost");
  const auto aliased = solve_generator_param_fixture("marginal_cost");
  CHECK(aliased == doctest::Approx(canonical).epsilon(1e-9));
}

TEST_CASE(  // NOLINT
    "PAMPL resolver — line.tmax (legacy) resolves identically to tmax_ab")
{
  // Before this turn the resolver had a hardcoded
  //   if (ref.attribute == "tmax" || ref.attribute == "tmax_ab")
  // chain.  That chain was removed; `tmax → tmax_ab` is now an entry
  // in `share/gtopt/naming_dialects.json`.  This test pins that the
  // legacy `tmax` form still works exactly the same as `tmax_ab`.
  const auto canonical = solve_line_param_fixture("tmax_ab");
  const auto legacy_alias = solve_line_param_fixture("tmax");
  CHECK(legacy_alias == doctest::Approx(canonical).epsilon(1e-9));
}
