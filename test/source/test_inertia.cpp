// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/inertia_provision.hpp>
#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/inertia_zone.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simple_commitment.hpp>
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

TEST_CASE("InertiaZone construction and default values")
{
  const InertiaZone iz;

  CHECK(iz.uid == Uid {unknown_uid});
  CHECK(iz.name == Name {});
  CHECK_FALSE(iz.active.has_value());
  CHECK_FALSE(iz.requirement.has_value());
  CHECK_FALSE(iz.cost.has_value());
}

TEST_CASE("InertiaProvision construction and default values")
{
  const InertiaProvision ip;

  CHECK(ip.uid == Uid {unknown_uid});
  CHECK(ip.name == Name {});
  CHECK_FALSE(ip.active.has_value());
  CHECK_FALSE(ip.inertia_constant.has_value());
  CHECK_FALSE(ip.rated_power.has_value());
  CHECK_FALSE(ip.provision_max.has_value());
  CHECK_FALSE(ip.provision_factor.has_value());
  CHECK_FALSE(ip.cost.has_value());
}

TEST_CASE("InertiaZoneLP - basic inertia requirement with provision")
{
  // Generator: Pmax=200 MW, Pmin=50 MW
  // Inertia: H=4s, S=100 MVA => FE = 4*100/50 = 8 MWs/MW
  // Requirement: 500 MWs
  // With r_inertia up to 50 MW and FE=8: max contribution = 400 MWs
  // Scarcity cost fills the gap (500-400=100 MWs)
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

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz1",
          .requirement = 500.0,
          .cost = 10000.0,
      },
  };

  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip1",
          .generator = Uid {1},
          .inertia_zones = "1",
          .inertia_constant = 4.0,
          .rated_power = 100.0,
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "InertiaTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify the provision variable is at its cap (50 MW = generator pmin).
  // With Φ = H·S/pmin = 4·100/50 = 8, the provision contributes at most
  // 8 · 50 = 400 MWs, so the 500 MWs requirement leaves a 100 MWs slack
  // priced at 10'000 $/MWs.
  const auto& ip_lps = system_lp.elements<InertiaProvisionLP>();
  REQUIRE(ip_lps.size() == 1);
  const auto& ip_lp = ip_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto r_col =
      ip_lp.lookup_provision_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(r_col.has_value());
  const auto r_val = lp.get_col_sol()[*r_col];
  CHECK(r_val == doctest::Approx(50.0).epsilon(0.01));
}

TEST_CASE("InertiaZoneLP - explicit provision_factor")
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

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz1",
          .requirement = 200.0,
          .cost = 5000.0,
      },
  };

  // Use explicit provision_factor instead of H and S.  Small
  // tie-breaker cost on r_inertia so the LP has a unique optimum at
  // the minimum-feasible r (without it, any r ∈ [25, 50] is equally
  // optimal and solvers disagree on which extremum they return —
  // CPLEX: 25, MindOpt: 50).
  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip1",
          .generator = Uid {1},
          .inertia_zones = "1",
          .provision_factor = 8.0,
          .cost = 0.01,
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "InertiaExplicitFETest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Requirement 200 MWs with Φ=8 is feasible at r=25 MW (r · Φ ≥
  // 200 → r ≥ 25; provision_max = gen_pmin = 50).  The 0.01 $/MW
  // provision cost above breaks the tie between r=25 and r=50 so
  // the LP uniquely prefers the minimum.
  const auto& ip_lps = system_lp.elements<InertiaProvisionLP>();
  REQUIRE(ip_lps.size() == 1);
  const auto& ip_lp = ip_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto r_col =
      ip_lp.lookup_provision_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(r_col.has_value());
  CHECK(lp.get_col_sol()[*r_col] == doctest::Approx(25.0).epsilon(0.01));
}

TEST_CASE("InertiaZoneLP - hard requirement (no cost)")
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

  // Hard requirement (no cost => fixed slack at requirement value)
  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz1",
          .requirement = 100.0,
      },
  };

  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip1",
          .generator = Uid {1},
          .inertia_zones = "1",
          .provision_factor = 8.0,
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;

  const System system = {
      .name = "InertiaHardTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("InertiaProvisionLP with SimpleCommitment - unified model")
{
  // This tests the unified formulation: SimpleCommitment provides u,
  // InertiaProvision provides r_inertia, coupling through shared p column.
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

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz1",
          .requirement = 200.0,
          .cost = 10000.0,
      },
  };

  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip1",
          .generator = Uid {1},
          .inertia_zones = "1",
          .inertia_constant = 4.0,
          .rated_power = 100.0,
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "InertiaCommitmentTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .simple_commitment_array = simple_commitment_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("InertiaProvisionLP - multi-zone provision")
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

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz1",
          .requirement = 100.0,
          .cost = 5000.0,
      },
      {
          .uid = Uid {2},
          .name = "iz2",
          .requirement = 150.0,
          .cost = 8000.0,
      },
  };

  // Generator provides to both zones
  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip1",
          .generator = Uid {1},
          .inertia_zones = "1:2",
          .provision_factor = 6.0,
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "InertiaMultiZoneTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(
    "InertiaZoneLP and InertiaProvisionLP - add_to_output via write_out")  // NOLINT
{
  // This test exercises both InertiaZoneLP::add_to_output and
  // InertiaProvisionLP::add_to_output by calling system_lp.write_out()
  // after resolving the LP.
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

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz1",
          .requirement = 200.0,
          .cost = 5000.0,
      },
  };

  // Small tie-breaker cost on provision so the LP uniquely prefers
  // the minimum-feasible r (see sibling test for detail).
  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip1",
          .generator = Uid {1},
          .inertia_zones = "1",
          .provision_factor = 8.0,
          .cost = 0.01,
      },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_inertia_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  opts.reserve_fail_cost = 10000.0;
  opts.output_directory = tmpdir.string();

  const System system = {
      .name = "InertiaOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify the provision col solution: generator capacity=200, pmin=50,
  // provision_factor=8 → max provision = pmin × provision_factor = 50 × 8 =
  // 400 MWs.  Requirement = 200 MWs, so provision should be
  // min(requirement, 400) / provision_factor = 200 / 8 = 25 MW.  The
  // 0.01 $/MW tie-breaker cost breaks the [25, 50] alternate-optima
  // interval and forces r=25 uniquely.
  const auto& ip_lps = system_lp.elements<InertiaProvisionLP>();
  REQUIRE(ip_lps.size() == 1);
  const auto& ip_lp = ip_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto r_col =
      ip_lp.lookup_provision_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(r_col.has_value());
  CHECK(lp.get_col_sol()[*r_col] == doctest::Approx(25.0).epsilon(0.01));

  // Verify the inertia zone requirement col exists and the slack is ≥ 0
  const auto& iz_lps = system_lp.elements<InertiaZoneLP>();
  REQUIRE(iz_lps.size() == 1);
  const auto req_col = iz_lps.front().lookup_requirement_col(
      scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(req_col.has_value());
  CHECK(lp.get_col_sol()[*req_col] >= 0.0);

  // Exercises InertiaZoneLP::add_to_output and
  // InertiaProvisionLP::add_to_output
  CHECK_NOTHROW(system_lp.write_out());

  std::filesystem::remove_all(tmpdir);
}
