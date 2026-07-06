// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simple_commitment.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

const Array<Bus> bus_array = {
    {
        .uid = Uid {1},
        .name = "b1",
    },
};

const Array<Demand> demand_array = {
    {
        .uid = Uid {1},
        .name = "d1",
        .bus = Uid {1},
        .capacity = 100.0,
    },
};

const Simulation simulation = {
    .block_array =
        {
            {
                .uid = Uid {1},
                .duration = 1,
            },
        },
    .stage_array =
        {
            {
                .uid = Uid {1},
                .first_block = 0,
                .count_block = 1,
            },
        },
    .scenario_array =
        {
            {
                .uid = Uid {0},
            },
        },
};

}  // namespace

TEST_CASE("SimpleCommitment construction and default values")
{
  const SimpleCommitment sc;

  CHECK(sc.uid == Uid {unknown_uid});
  CHECK(sc.name == Name {});
  CHECK_FALSE(sc.active.has_value());
  CHECK_FALSE(sc.dispatch_pmin.has_value());
  CHECK_FALSE(sc.relax.has_value());
  CHECK_FALSE(sc.must_run.has_value());
}

TEST_CASE("SimpleCommitment attribute assignment")
{
  SimpleCommitment sc;
  sc.uid = 100;
  sc.name = "sc1";
  sc.generator = Uid {1};
  sc.dispatch_pmin = 50.0;
  sc.relax = false;
  sc.must_run = false;

  CHECK(sc.uid == 100);
  CHECK(sc.name == "sc1");
  REQUIRE(sc.dispatch_pmin.has_value());
  auto* pmin_ptr = std::get_if<Real>(&sc.dispatch_pmin.value());
  REQUIRE(pmin_ptr != nullptr);
  CHECK(*pmin_ptr == 50.0);
  CHECK(sc.relax.value_or(true) == false);
  CHECK(sc.must_run.value_or(true) == false);
}

TEST_CASE("SimpleCommitmentLP - binary mode basic")
{
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 50.0,
          .gcost = 30.0,
          .capacity = 200.0,
      },
  };

  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 50.0,
          .relax = false,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  // Binary (non-relaxed) commitment → integer columns; pin a MIP-capable
  // solver so an ambient GTOPT_SOLVER=clp (CI) doesn't trip the LP-only
  // guard in LinearInterface::load_flat.
  opts.lp_matrix_options.solver_name = solver_test::first_mip_solver();

  const System system = {
      .name = "SimpleCommitTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  // Should have at least: gen col, demand fail col, status col = 3+ cols
  // And at least: demand balance row, gen_upper row, gen_lower row = 3+ rows
  CHECK(lp.get_numcols() >= 3);
  CHECK(lp.get_numrows() >= 3);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SimpleCommitmentLP - relaxed mode")
{
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 50.0,
          .gcost = 30.0,
          .capacity = 200.0,
      },
  };

  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 50.0,
          .relax = true,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "RelaxedCommitTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SimpleCommitmentLP - must run")
{
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 50.0,
          .gcost = 30.0,
          .capacity = 200.0,
      },
  };

  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 50.0,
          .must_run = true,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  // Binary commitment → integer columns; MIP-capable pin (see above).
  opts.lp_matrix_options.solver_name = solver_test::first_mip_solver();

  const System system = {
      .name = "MustRunTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // must_run = true clamps the status column to 1 via lower bound =
  // upper bound = 1.  Verify the resolved value is exactly 1.
  const auto& sc_lps = system_lp.elements<SimpleCommitmentLP>();
  REQUIRE(sc_lps.size() == 1);
  const auto& sc_lp = sc_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto u_col =
      sc_lp.lookup_status_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(u_col.has_value());
  CHECK(lp.get_col_sol()[*u_col] == doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE("SimpleCommitmentLP - dispatch_pmin defaults to generator pmin")
{
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 80.0,
          .gcost = 30.0,
          .capacity = 200.0,
      },
  };

  // No dispatch_pmin set — should fall back to generator's pmin=80
  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .relax = true,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "DefaultPminTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SimpleCommitmentLP - add_to_output via write_out")  // NOLINT
{
  // Exercises SimpleCommitmentLP::add_to_output by calling write_out after
  // resolving the LP.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 20.0,
          .gcost = 30.0,
          .capacity = 100.0,
      },
  };

  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 20.0,
          .relax = true,
      },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_sc_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.output_directory = tmpdir.string();

  const System system = {
      .name = "SimpleCommitmentOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify the status col: demand=100 MW, generator capacity=100 MW, so
  // the generator must be dispatched → status u should be 1.0 (relaxed).
  const auto& sc_lps = system_lp.elements<SimpleCommitmentLP>();
  REQUIRE(sc_lps.size() == 1);
  const auto& sc_lp = sc_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto u_col =
      sc_lp.lookup_status_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(u_col.has_value());
  CHECK(lp.get_col_sol()[*u_col] == doctest::Approx(1.0).epsilon(0.001));

  // Exercises SimpleCommitmentLP::add_to_output (status_cols, gen_upper_rows,
  // gen_lower_rows)
  CHECK_NOTHROW(system_lp.write_out());

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "SimpleCommitmentLP — MIP status u binary when pmin exceeds demand")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // NOLINTBEGIN(bugprone-throwing-static-initialization,
  // bugprone-unchecked-optional-access, cert-err58-cpp)

  auto& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // Demand = 20 MW, but generator pmin = 40 MW. When ON, must produce ≥ 40.
  // Optimal MIP solution: turn OFF (u=0) and pay demand_fail_cost for 20 MW.
  const Array<Demand> small_demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 20.0,
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 40.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 40.0,
          .relax = false,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "SimpleCommitMIPTest",
      .bus_array = bus_array,
      .demand_array = small_demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
  };

  const PlanningOptionsLP options(opts);
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

  // The status column must be marked integer (binary) since relax=false.
  const auto& sc_lps = system_lp.elements<SimpleCommitmentLP>();
  REQUIRE(sc_lps.size() == 1);
  const auto& sc_lp = sc_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto u_col =
      sc_lp.lookup_status_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(u_col.has_value());
  CHECK(lp.is_integer(*u_col));

  // Solve as MIP (resolve automatically dispatches to MIP solve when any
  // column is integer, provided the backend supports it).
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With demand=20 < pmin=40, dispatching is infeasible at the lower bound,
  // so the MIP optimum is u=0 (unit OFF, pay demand_fail_cost).
  CHECK(lp.get_col_sol()[*u_col] == doctest::Approx(0.0).epsilon(0.001));
}

TEST_CASE(  // NOLINT
    "SimpleCommitmentLP — solved dispatch ∈ {0} ∪ [pmin, pmax] (off-or-min)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // NOLINTBEGIN(bugprone-throwing-static-initialization,
  // bugprone-unchecked-optional-access, cert-err58-cpp)

  auto& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // ── Off-or-min: the structural invariant ──────────────────────────
  //
  // With binary ``u_status ∈ {0, 1}`` and the gating rows
  //   gen ≥ pmin × u_status
  //   gen ≤ pmax × u_status
  // any feasible solution must have ``gen ∈ {0} ∪ [pmin, pmax]``.
  // Companion to "MIP status u binary when pmin exceeds demand" (line
  // 359) which covers the u=0 / idle branch; this one covers the u=1 /
  // active branch.
  //
  // Same Commitment-binary mechanism powers ``Converter.commitment``
  // (battery synthesised gen, see ``test_converter_commitment.cpp``),
  // so this test indirectly validates the off-or-min property for
  // battery commitment as well.
  //
  // Economic setup: demand 80 MW, generator pmin=40 / pmax=100 / gcost=$50.
  // demand_fail_cost = $1000 (so failing is far worse than dispatching).
  // The MIP optimum: u=1, gen=80 MW (cleanly inside [40, 100]).
  //
  // To also probe the u=0 branch in the same solve, the test runs TWO
  // SUBCASES — one with demand=80 (forces u=1, gen=80 ∈ [pmin, pmax]),
  // one with demand=20 (forces u=0, gen=0).

  // Generator and SimpleCommitment shared across SUBCASEs.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 40.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 40.0,
          .relax = false,  // MIP — binary u_status
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);

  LpMatrixOptions flat_opts;
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      flat_opts.solver_name = name;
      break;
    }
  }
  if (flat_opts.solver_name.empty()) {
    MESSAGE("Skipping MIP test — no loaded solver reports supports_mip()");
    return;
  }

  // Helper: solve and return (u_status_sol, gen_sol).
  auto solve_with_demand = [&](double demand_mw) -> std::pair<double, double>
  {
    const Array<Demand> dem = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = demand_mw,
        },
    };
    const System system = {
        .name = "OffOrMinTest",
        .bus_array = bus_array,
        .demand_array = dem,
        .generator_array = generator_array,
        .simple_commitment_array = simple_commitment_array,
    };
    SystemLP system_lp(system, simulation_lp, flat_opts);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);

    // Find the u_status col + the gen col.
    const auto& sc_lps = system_lp.elements<SimpleCommitmentLP>();
    REQUIRE(sc_lps.size() == 1);
    const auto& sc_lp = sc_lps.front();
    const auto& scenario_lp = simulation_lp.scenarios().front();
    const auto& stage_lp = simulation_lp.stages().front();
    const auto& block_lp = simulation_lp.blocks().front();
    const auto u_col =
        sc_lp.lookup_status_col(scenario_lp, stage_lp, block_lp.uid());
    REQUIRE(u_col.has_value());

    const auto& gen_lps = system_lp.elements<GeneratorLP>();
    REQUIRE(gen_lps.size() == 1);
    const auto gen_cols =
        gen_lps.front().generation_cols_at(scenario_lp, stage_lp);
    REQUIRE(gen_cols.size() == 1);
    // gen_cols is a flat_map<BlockUid, Col>; pull out the single entry.
    const auto gen_col = gen_cols.begin()->second;

    const auto sol = lp.get_col_sol();
    return {sol[*u_col], sol[gen_col]};
  };

  constexpr double k_pmin = 40.0;
  constexpr double k_pmax = 100.0;
  constexpr double k_tol = 1e-3;

  SUBCASE("u=1 active branch: dispatch sits in [pmin, pmax]")
  {
    // Demand 80 MW > pmin=40, so the MIP optimum is u=1 with
    // gen exactly meeting demand (80 ∈ [40, 100]).
    const auto [u, gen] = solve_with_demand(80.0);
    CHECK(u == doctest::Approx(1.0).epsilon(k_tol));
    // Off-or-min: u=1 ⇒ gen must be in [pmin, pmax].
    CHECK(gen >= k_pmin - k_tol);
    CHECK(gen <= k_pmax + k_tol);
    // Tight check: this LP picks gen = demand (no surplus needed).
    CHECK(gen == doctest::Approx(80.0).epsilon(k_tol));
  }

  SUBCASE("u=0 idle branch: dispatch is exactly zero")
  {
    // Demand 20 MW < pmin=40 — dispatching would force gen ≥ 40,
    // worse than just failing the 20 MW load.  MIP picks u=0, gen=0.
    const auto [u, gen] = solve_with_demand(20.0);
    CHECK(u == doctest::Approx(0.0).epsilon(k_tol));
    // Off-or-min: u=0 ⇒ gen = 0 (gating row gen ≤ pmax × u = 0).
    CHECK(gen == doctest::Approx(0.0).epsilon(k_tol));
  }

  // NOLINTEND(bugprone-throwing-static-initialization,
  // bugprone-unchecked-optional-access, cert-err58-cpp)
}

// NOLINTEND(bugprone-throwing-static-initialization,
// bugprone-unchecked-optional-access, cert-err58-cpp)