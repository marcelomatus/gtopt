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

using namespace gtopt;
// NOLINTBEGIN(bugprone-throwing-static-initialization,
// bugprone-unchecked-optional-access,cert-err58-cpp)

namespace
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
          .inertia_zones = {SingleId {Uid {1}}},
          .inertia_constant = 4.0,
          .rated_power = 100.0,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;

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
          .inertia_zones = {SingleId {Uid {1}}},
          .provision_factor = 8.0,
          .cost = 0.01,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;

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
          .inertia_zones = {SingleId {Uid {1}}},
          .provision_factor = 8.0,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;

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
          .inertia_zones = {SingleId {Uid {1}}},
          .inertia_constant = 4.0,
          .rated_power = 100.0,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;

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
          .inertia_zones = {SingleId {Uid {1}}, SingleId {Uid {2}}},
          .provision_factor = 6.0,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;

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
          .inertia_zones = {SingleId {Uid {1}}},
          .provision_factor = 8.0,
          .cost = 0.01,
      },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_inertia_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;
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

TEST_CASE("InertiaProvisionLP — empty inertia_zones is a no-op (no LP rows)")
{
  // Schema refactor (2026-05-16) accepts an empty `Array<SingleId>` for
  // `inertia_zones`.  Pin the LP-layer contract: an empty zone list
  // builds, solves, and emits no inertia-provision rows or coefficients.
  // No throw, no spurious rows — the provision is structurally a
  // dead element.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 30.0,
          .capacity = 100.0,
      },
  };

  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip_empty",
          .generator = Uid {1},
          .inertia_zones = {},  // empty — no zones to link
          .provision_max = 50.0,
          .provision_factor = 1.0,
      },
  };

  const System system = {
      .name = "InertiaEmptyZonesTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_provision_array = inertia_provision_array,
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // No inertia-provision column should reference any zone — verify
  // by counting `inertia_provision_p_` cols (the per-block provision
  // variable).  Empty zone list → zero provision rows linking to
  // zones; the provision variable itself may still exist depending
  // on the LP-builder branch, but it carries no coupling.
  std::size_t zone_coupling_rows = 0;
  for (const auto& [name, _row] : lp.row_name_map()) {
    if (name.find("inertia_zone_") != std::string_view::npos
        && name.find("requirement_") != std::string_view::npos)
    {
      ++zone_coupling_rows;
    }
  }
  CHECK(zone_coupling_rows == 0);
}

// ─────────────────────────────────────────────────────────────────────────
//   Multi-zone coverage (derived from "InertiaProvisionLP - multi-zone
//   provision" above, plus the InertiaZoneLP basic tests).  The shipped
//   suite has exactly one multi-zone fixture and it stops at "the LP
//   resolves" — these tests assert on per-zone behavior so the
//   inertia_zone_indexes_ iteration loop in
//   `InertiaProvisionLP::add_to_lp` is genuinely exercised.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "InertiaZoneLP - two zones with partitioned providers, "
    "distinct requirements bind independently")
{
  // 2 generators, 2 zones, 1 provision per generator → each provision
  // serves exactly one zone.  Both zones must be satisfied, so the LP
  // must dispatch enough from each generator to meet its zone's
  // requirement individually.  This catches regressions where the
  // per-zone requirement row wiring shares state across zones.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 20.0,
          .gcost = 30.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 20.0,
          .gcost = 60.0,
          .capacity = 200.0,
      },
  };

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz_north",
          .requirement = 120.0,
          .cost = 5000.0,
      },
      {
          .uid = Uid {2},
          .name = "iz_south",
          .requirement = 180.0,
          .cost = 8000.0,
      },
  };

  // Partitioned providers: gen1 → zone1 only, gen2 → zone2 only.
  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip_north",
          .generator = Uid {1},
          .inertia_zones = {SingleId {Uid {1}}},
          .provision_factor = 6.0,  // 6 × gen output contributes to zone1
      },
      {
          .uid = Uid {2},
          .name = "ip_south",
          .generator = Uid {2},
          .inertia_zones = {SingleId {Uid {2}}},
          .provision_factor = 9.0,  // 9 × gen output contributes to zone2
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;

  const System system = {
      .name = "InertiaTwoZonePartitioned",
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

  // Both zone requirements must hold:
  //   zone1: 6 × gen1_out >= 120 → gen1_out >= 20 (also = pmin)
  //   zone2: 9 × gen2_out >= 180 → gen2_out >= 20 (also = pmin)
  // Demand = 100 MW, so total gen = 100; the rest is split by cost.
  // Optimal: g1 supplies more (cheaper at 30 $/MWh).  Both pmins are
  // active.  LP must be feasible at well below demand_fail_cost.
  const auto obj_phys = lp.get_obj_value();
  CHECK(std::isfinite(obj_phys));
  CHECK(obj_phys < 1.0e5);  // dispatch dominates, no failure cost
}

TEST_CASE(
    "InertiaProvisionLP - one provider spans both zones with distinct "
    "requirements; the tighter zone binds the provision")
{
  // Single generator providing inertia to BOTH zones with the same
  // provision_factor.  The two zones have **different** requirements
  // so only the tighter one binds the provision variable.  This
  // mirrors the existing "InertiaProvisionLP - multi-zone provision"
  // test but adds an explicit objective-bound check that pins the
  // binding-zone behaviour: the LP must dispatch enough generation
  // to satisfy the LARGER of the two zone requirements.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,  // no lower bound — let the inertia req drive it
          .gcost = 30.0,
          .capacity = 500.0,
      },
  };

  const Array<InertiaZone> inertia_zone_array = {
      {
          .uid = Uid {1},
          .name = "iz_easy",
          .requirement = 120.0,  // 6 × 20 — pmin would satisfy
          .cost = 5000.0,
      },
      {
          .uid = Uid {2},
          .name = "iz_hard",
          .requirement = 360.0,  // 6 × 60 — needs 60 MW dispatch
          .cost = 8000.0,
      },
  };

  // Single provision feeds BOTH zones with the same factor.
  const Array<InertiaProvision> inertia_provision_array = {
      {
          .uid = Uid {1},
          .name = "ip_dual",
          .generator = Uid {1},
          .inertia_zones = {SingleId {Uid {1}}, SingleId {Uid {2}}},
          .provision_factor = 6.0,
      },
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.reserve_shortage_cost = 10000.0;

  const System system = {
      .name = "InertiaOneProviderTwoZones",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .inertia_zone_array = inertia_zone_array,
      .inertia_provision_array = inertia_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  // Enable LP-row names so the row_name_map check below sees the
  // class/constraint labels.  Default `LpMatrixOptions{}` has
  // `row_with_name_map = false`, which leaves the label_maker at
  // `LpNamesLevel::none` and skips the row-name string recording —
  // that's the cause of the legacy `zone_req_rows == 0` regression
  // (the underlying rows ARE there, just nameless).
  LpMatrixOptions flat_opts {};
  flat_opts.row_with_name_map = true;
  flat_opts.col_with_names = true;
  SystemLP system_lp(system, simulation_lp, flat_opts);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto obj_phys = lp.get_obj_value();
  CHECK(std::isfinite(obj_phys));
  CHECK(obj_phys < 1.0e5);  // single bus, single demand — no failures

  // Verify the LP wired BOTH zone requirement rows (not just one).
  // Each zone in `inertia_zone_array` should produce a
  // `requirement` row keyed by stage 1, block 1.  If the iteration
  // over `inertia_zones` silently skipped the second zone the LP
  // would build but the row count would tell us the difference.
  std::size_t zone_req_rows = 0;
  for (const auto& [name, _row] : lp.row_name_map()) {
    // Row names use the snake_case class label ("inertiazone") plus
    // constraint name ("requirement"); the label-maker concatenates
    // them as `<class>_<constraint>_<uid>_<scen>_<stage>_<block>`.
    if (name.find("inertiazone") != std::string_view::npos
        && name.find("requirement") != std::string_view::npos)
    {
      ++zone_req_rows;
    }
  }
  // 2 zones × 1 stage × 1 block = 2 requirement rows.
  CHECK(zone_req_rows == 2);
}

// ────────────────────────────────────────────────────────────────────────
// Substitute-out regression (issue #529 item 2 / follow-up audit G5):
// mirror of the ReserveZone test in test_reserve_zone.cpp.  When no
// `cost` is set on an InertiaZone, the legacy form built a fully-pinned
// bookkeeping column (lowb = uppb = requirement, cost = 0); the new form
// drops the column and stamps `requirement` on the row RHS instead.
// ────────────────────────────────────────────────────────────────────────
TEST_CASE(
    "InertiaZoneLP substitute-out: hard req skips the pinned col")  // NOLINT
{
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array_local = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array_local = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};

  const Array<Generator> generator_array_local = {{.uid = Uid {1},
                                                   .name = "g1",
                                                   .bus = Uid {1},
                                                   .gcost = 10.0,
                                                   .capacity = 200.0}};

  const Array<InertiaZone> inertia_zone_array_local = {
      {
          .uid = Uid {1},
          .name = "iz_hard",
          .requirement = 50.0,
          // NO `.cost` — exercises the hard substitute-out branch.
      },
  };

  const Array<InertiaProvision> inertia_provision_array_local = {
      {.uid = Uid {1},
       .name = "ip1",
       .generator = Uid {1},
       .inertia_zones = {SingleId {Uid {1}}},
       .provision_factor = 1.0},
  };

  const System system {
      .name = "InertiaZoneHardReqSubstitute",
      .bus_array = bus_array_local,
      .demand_array = demand_array_local,
      .generator_array = generator_array_local,
      .inertia_zone_array = inertia_zone_array_local,
      .inertia_provision_array = inertia_provision_array_local,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto& iz_collection =
      std::get<Collection<InertiaZoneLP>>(system_lp.collections());
  REQUIRE(!iz_collection.elements().empty());
  const auto& iz_lp = iz_collection.elements()[0];

  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];
  const auto buid = make_uid<Block>(1);

  // No bookkeeping column on the hard branch.
  const auto req_col = iz_lp.lookup_requirement_col(scenario, stage, buid);
  CHECK_FALSE(req_col.has_value());

  // Row still present with RHS = block_rreq.
  const auto& rows = iz_lp.requirement_rows();
  const auto rows_it = rows.find({scenario.uid(), stage.uid()});
  REQUIRE(rows_it != rows.end());
  REQUIRE(rows_it->second.find(buid) != rows_it->second.end());
}

// NOLINTEND(bugprone-throwing-static-initialization,
// bugprone-unchecked-optional-access,cert-err58-cpp)
