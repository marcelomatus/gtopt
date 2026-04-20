#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Generator set_attrs functionality")
{
  using namespace gtopt;
  Generator gen;
  GeneratorAttrs attrs;

  // Setup test values
  attrs.bus = Uid {42};
  attrs.pmin = 10.0;
  attrs.pmax = 100.0;

  // Test moving attrs to generator
  gen.set_attrs(attrs);

  CHECK(std::get<Uid>(gen.bus) == 42);
  CHECK(attrs.bus == SingleId {});  // Should be reset after exchange
  CHECK(gen.pmin.has_value());
  CHECK(gen.pmax.has_value());
  CHECK_FALSE(attrs.pmin.has_value());  // Should be moved
  CHECK_FALSE(attrs.pmax.has_value());  // Should be moved

  if (gen.pmin) {
    CHECK(std::get<Real>(gen.pmin.value()) == 10.0);
  }
  if (gen.pmax) {
    CHECK(std::get<Real>(gen.pmax.value()) == 100.0);
  }
}

TEST_CASE("Generator construction and attributes")
{
  Generator gen;

  // Default values
  CHECK(gen.uid == Uid {unknown_uid});
  CHECK(gen.name == Name {});
  CHECK_FALSE(gen.active.has_value());

  // Set some values
  gen.uid = Uid {1001};
  gen.name = "TestGenerator";
  gen.active = true;
  gen.bus = Uid {5};

  CHECK(gen.uid == 1001);
  CHECK(gen.name == "TestGenerator");
  CHECK(std::get<IntBool>(gen.active.value()) == 1);
  CHECK(std::get<Uid>(gen.bus) == 5);
}

TEST_CASE("GeneratorLP - basic generation")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
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
      .name = "GenTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto sol = lp.get_col_sol();
  CHECK(sol[2] == doctest::Approx(100.0));  // generation matches demand
}

TEST_CASE("GeneratorLP - multiple generators merit order")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "cheap",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {2},
          .name = "expensive",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 80.0,
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
      .name = "MeritOrderTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("GeneratorLP - generator with expansion")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 0.0,
          .expcap = 200.0,
          .expmod = 100.0,
          .annual_capcost = 5000.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "GenExpansionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("GeneratorLP - generator with pmin/pmax constraints")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 20.0,
          .pmax = 150.0,
          .gcost = 50.0,
          .capacity = 200.0,
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
      .name = "PMinMaxTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("GeneratorLP — capainst primal col_sol expands to meet demand")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Deterministic LP where a cheap expandable generator must build capacity
  // (capainst) to cover a 50 MW demand. The base `capacity = 5` of the cheap
  // generator is insufficient; the alternative (`expensive_base`) has
  // `gcost = 500`, so the LP prefers expanding the cheap unit paying a small
  // `annual_capcost = 10`. Expected solution:
  //   - capainst(cheap_expandable) = 45  (5 base + 45 expansion = 50)
  //   - generation(cheap_expandable) = 50
  //   - generation(expensive_base)   = 0
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "cheap_expandable",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 5.0,
          .expcap = 100.0,
          .expmod = 1.0,
          .annual_capcost = 10.0,
      },
      {
          .uid = Uid {2},
          .name = "expensive_base",
          .bus = Uid {1},
          .gcost = 500.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "GenCapainstExpansionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 2);

  const auto cheap_it = std::ranges::find_if(
      gen_lps, [](const auto& g) { return g.uid() == Uid {1}; });
  REQUIRE(cheap_it != gen_lps.end());
  const auto& cheap_gen_lp = *cheap_it;

  const auto exp_it = std::ranges::find_if(
      gen_lps, [](const auto& g) { return g.uid() == Uid {2}; });
  REQUIRE(exp_it != gen_lps.end());
  const auto& exp_gen_lp = *exp_it;

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();

  const auto col_sol = lp.get_col_sol();

  // capainst column for the cheap expandable generator.
  // Row: -capainst + expcap*expmod = dcap (= 0 on first stage with
  // prev=current capacity). With expcap=100, capainst = 100*expmod.
  // LP picks the minimum expmod that satisfies generation ≤ capainst
  // for the 50 MW dispatch: expmod = 0.5 → capainst = 50.
  const auto cap_col = cheap_gen_lp.capacity_col_at(stage_lp);
  REQUIRE(cap_col.has_value());
  const auto cap_val = col_sol[*cap_col];
  CHECK(cap_val == doctest::Approx(50.0).epsilon(1e-6));

  // Cheap generator produces all 50 MW.
  const auto& cheap_gcols =
      cheap_gen_lp.generation_cols_at(scenario_lp, stage_lp);
  const auto cheap_it_c = cheap_gcols.find(block_lp.uid());
  REQUIRE(cheap_it_c != cheap_gcols.end());
  CHECK(col_sol[cheap_it_c->second] == doctest::Approx(50.0).epsilon(1e-6));

  // Expensive generator produces nothing.
  const auto& exp_gcols = exp_gen_lp.generation_cols_at(scenario_lp, stage_lp);
  const auto exp_it_c = exp_gcols.find(block_lp.uid());
  REQUIRE(exp_it_c != exp_gcols.end());
  CHECK(col_sol[exp_it_c->second] == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("GeneratorLP — capacity row dual equals marginal cost minus gcost")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Deterministic LP where a cheap generator at its capacity limit sets the
  // marginal rent on its capacity constraint. With demand > cheap capacity,
  // the expensive generator is marginal, so the bus marginal price is 100
  // $/MWh. The cheap generator's capacity row (generation ≤ capacity)
  // binds, and its dual equals the marginal price minus the cheap gcost:
  //   dual(cheap.capacity) = 100 − 10 = 90.
  //
  // Dispatch: cheap = 30 (at cap), expensive = 20 → total = 50 MW.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  // The capacity row (generation ≤ capainst) is only created when the
  // generator has an expansion column. Enable expansion on cheap_gen with a
  // prohibitively expensive `annual_capcost` so the LP leaves capainst at
  // its base value of 30 MW — the capacity row then binds and carries the
  // rent (100 − 10 = 90). The expensive generator keeps a large static
  // capacity so the LP is feasible without its own capacity row.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "cheap_gen",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 30.0,
          .expcap = 100.0,
          .expmod = 1.0,
          .capmax = 1000.0,
          .annual_capcost = 1.0e7,
      },
      {
          .uid = Uid {2},
          .name = "expensive_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "GenCapacityRowDualTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Trigger lazy crossover if barrier was used without crossover — a no-op
  // for the default simplex solve.
  lp.ensure_duals();

  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 2);

  const auto cheap_it = std::ranges::find_if(
      gen_lps, [](const auto& g) { return g.uid() == Uid {1}; });
  REQUIRE(cheap_it != gen_lps.end());
  const auto& cheap_gen_lp = *cheap_it;

  const auto exp_it = std::ranges::find_if(
      gen_lps, [](const auto& g) { return g.uid() == Uid {2}; });
  REQUIRE(exp_it != gen_lps.end());
  const auto& exp_gen_lp = *exp_it;

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();

  // Primal sanity checks: cheap hits its 30 MW cap, expensive covers the
  // remaining 20 MW.
  const auto col_sol = lp.get_col_sol();
  const auto& cheap_gcols =
      cheap_gen_lp.generation_cols_at(scenario_lp, stage_lp);
  const auto cheap_gcol_it = cheap_gcols.find(block_lp.uid());
  REQUIRE(cheap_gcol_it != cheap_gcols.end());
  CHECK(col_sol[cheap_gcol_it->second] == doctest::Approx(30.0).epsilon(1e-5));

  const auto& exp_gcols = exp_gen_lp.generation_cols_at(scenario_lp, stage_lp);
  const auto exp_gcol_it = exp_gcols.find(block_lp.uid());
  REQUIRE(exp_gcol_it != exp_gcols.end());
  CHECK(col_sol[exp_gcol_it->second] == doctest::Approx(20.0).epsilon(1e-5));

  // Capacity row for the cheap generator at this (scenario, stage, block).
  // Row: capacity_col − generation_col ≥ 0 (i.e., generation ≤ capacity).
  // Since cheap is binding and expensive is marginal at gcost=100, the
  // shadow price (row dual) on the cheap capacity row equals the rent on
  // capacity: 100 − 10 = 90. Sign convention of `get_row_dual()` for a
  // `≥` constraint yields a non-negative value here.
  const auto& cheap_crows =
      cheap_gen_lp.capacity_rows_at(scenario_lp, stage_lp);
  const auto cheap_crow_it = cheap_crows.find(block_lp.uid());
  REQUIRE(cheap_crow_it != cheap_crows.end());

  const auto& row_dual = lp.get_row_dual();
  const auto dual_val = row_dual[cheap_crow_it->second];
  CHECK(std::abs(dual_val) == doctest::Approx(90.0).epsilon(1e-5));
}

TEST_CASE("GeneratorLP — integer_expmod MIP gives integer expansion modules")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Gate: this test requires a MIP-capable backend. When no MIP solver is
  // available (e.g., CLP-only sandbox), skip silently with a message.
  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE(
        "Skipping integer_expmod MIP test: no MIP-capable solver plugin "
        "loaded (need cbc, cplex, highs, or similar).");
    return;
  }

  // Single-bus, single-stage, single-block setup.  Generator has no base
  // capacity and may install up to 10 expansion modules of 30 MW each.
  // With a 75 MW demand and expcap=30 modules, the smallest integer count
  // that covers demand is 3 → capainst = 90 MW (2 modules = 60 MW is
  // infeasible for the demand balance given demand_fail_cost=10000).
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g_int",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 0.0,
          .expcap = 30.0,
          .expmod = 10.0,
          .annual_capcost = 1000.0,
          .integer_expmod = true,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 75.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "GenIntegerExpmodTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 10000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  // Pick the first MIP-capable solver explicitly so we don't fall
  // through to the GTOPT_SOLVER env override (CI pins to "clp" —
  // LP-only).  Defensive: skip if no MIP solver was found, even
  // though has_mip_solver() returned true above (guards against an
  // inconsistency between has_mip_solver() and supports_mip()).
  LpMatrixOptions flat_opts;
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      flat_opts.solver_name = name;
      break;
    }
  }
  if (flat_opts.solver_name.empty()) {
    MESSAGE(
        "Skipping MIP test — supports_mip() returned false for "
        "every loaded solver despite has_mip_solver()=true");
    return;
  }
  SystemLP system_lp(system, simulation_lp, flat_opts);

  auto&& lp = system_lp.linear_interface();

  // Locate the generator's expmod column and verify the integer flag was
  // propagated from the Generator down to the LP column.
  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 1);
  const auto& gen_lp = gen_lps.front();

  const auto& stage_lp = simulation_lp.stages().front();
  const auto expmod_col = gen_lp.expmod_col_at(stage_lp);
  REQUIRE(expmod_col.has_value());
  CHECK(lp.is_integer(*expmod_col));

  // Solve the MIP and check the integer optimum.
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto col_sol = lp.get_col_sol();
  const auto expmod_val = col_sol[*expmod_col];
  CHECK(expmod_val == doctest::Approx(3.0).epsilon(1e-6));

  // capainst must equal 3 * 30 = 90 MW (enough to cover the 75 MW demand).
  const auto cap_col = gen_lp.capacity_col_at(stage_lp);
  REQUIRE(cap_col.has_value());
  CHECK(col_sol[*cap_col] == doctest::Approx(90.0).epsilon(1e-6));
}
