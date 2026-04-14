// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simple_commitment.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

const Array<Bus> bus_array = {{
    .uid = Uid {1},
    .name = "b1",
}};

const Array<Demand> demand_array = {
    {
        .uid = Uid {1},
        .name = "d1",
        .bus = Uid {1},
        .capacity = 100.0,
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
  opts.demand_fail_cost = 1000.0;

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
  opts.demand_fail_cost = 1000.0;

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
  opts.demand_fail_cost = 1000.0;

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
  opts.demand_fail_cost = 1000.0;

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
