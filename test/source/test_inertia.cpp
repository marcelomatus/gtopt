// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/inertia_provision.hpp>
#include <gtopt/inertia_zone.hpp>
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

  // Use explicit provision_factor instead of H and S
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
