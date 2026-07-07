// SPDX-License-Identifier: BSD-3-Clause
//
// Parity tests for the unified `CapacityProfile` / `CapacityProfileLP`
// infrastructure (Commit 1 of the legacy →  unified migration).
//
// The unified `CapacityProfile{owner_kind = Generator|Demand, owner = ...}`
// builds the same LP rows/cols as the legacy `GeneratorProfile`
// /`DemandProfile` elements, including identical objective value and
// identical row/col counts.  Tests below assert this parity on a
// minimal fixture for each owner kind, plus the JSON contract.

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/capacity_profile.hpp>
#include <gtopt/capacity_profile_traits.hpp>
#include <gtopt/json/json_capacity_profile.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

[[nodiscard]] Simulation make_single_block_sim()
{
  return Simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
}

}  // namespace

TEST_CASE("CapacityProfile default construction")  // NOLINT
{
  const CapacityProfile p;

  CHECK(p.uid == Uid {unknown_uid});
  CHECK(p.name.empty());
  CHECK_FALSE(p.active.has_value());
  CHECK(p.owner_kind == ProfileOwnerKind::Generator);  // default enumerator
  CHECK(p.owner == SingleId {unknown_uid});
  CHECK_FALSE(p.scost.has_value());
}

TEST_CASE(
    "ProfileOwnerKind round-trips through the NamedEnum framework")  // NOLINT
{
  CHECK(enum_name(ProfileOwnerKind::Generator) == "generator");
  CHECK(enum_name(ProfileOwnerKind::Demand) == "demand");

  // ASCII case-insensitive lookup.
  CHECK(enum_from_name<ProfileOwnerKind>("generator")
            .value_or(ProfileOwnerKind::Demand)
        == ProfileOwnerKind::Generator);
  CHECK(enum_from_name<ProfileOwnerKind>("DEMAND").value_or(
            ProfileOwnerKind::Generator)
        == ProfileOwnerKind::Demand);
  CHECK_FALSE(enum_from_name<ProfileOwnerKind>("bogus").has_value());
}

TEST_CASE(
    "CapacityProfileTraits expose per-kind labels and slack names")  // NOLINT
{
  CHECK(CapacityProfileTraits<GeneratorLP>::kind
        == ProfileOwnerKind::Generator);
  CHECK(CapacityProfileTraits<GeneratorLP>::slack_name == "spillover");
  CHECK(CapacityProfileTraits<GeneratorLP>::output_class_name
        == LPClassName {"GeneratorProfile"});

  CHECK(CapacityProfileTraits<DemandLP>::kind == ProfileOwnerKind::Demand);
  CHECK(CapacityProfileTraits<DemandLP>::slack_name == "unserved");
  CHECK(CapacityProfileTraits<DemandLP>::output_class_name
        == LPClassName {"DemandProfile"});
}

TEST_CASE(
    "CapacityProfile JSON round-trip preserves owner_kind enum")  // NOLINT
{
  constexpr std::string_view src = R"({
    "uid": 7,
    "name": "p_solar",
    "owner_kind": "generator",
    "owner": 42,
    "profile": 0.6
  })";

  const auto p = daw::json::from_json<CapacityProfile>(src);
  CHECK(p.uid == Uid {7});
  CHECK(p.name == "p_solar");
  CHECK(p.owner_kind == ProfileOwnerKind::Generator);
  REQUIRE(std::holds_alternative<Uid>(p.owner));
  CHECK(std::get<Uid>(p.owner) == Uid {42});

  // Re-emit and re-parse to confirm the enum survives the trip.
  const auto rendered = daw::json::to_json(p);
  const auto p2 = daw::json::from_json<CapacityProfile>(rendered);
  CHECK(p2.owner_kind == ProfileOwnerKind::Generator);
  CHECK(p2.uid == p.uid);
}

TEST_CASE(
    "CapacityProfile JSON — unknown owner_kind throws (fail-fast)")  // NOLINT
{
  // Typo / unsupported kind must fail at parse time, not silently
  // coerce to Generator.  Matches StrictParsePolicy convention.
  constexpr std::string_view src = R"({
    "uid": 1,
    "name": "bad_kind",
    "owner_kind": "battery",
    "owner": 1,
    "profile": 1.0
  })";

  CHECK_THROWS_AS((void)daw::json::from_json<CapacityProfile>(src),
                  std::invalid_argument);
}

TEST_CASE(
    "CapacityProfileLP (Generator kind) builds the same LP as "
    "GeneratorProfileLP")  // NOLINT
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const auto sim = make_single_block_sim();

  // Legacy path.
  double legacy_obj {};
  int legacy_rows {};
  int legacy_cols {};
  {
    const Array<GeneratorProfile> gp_array = {
        {.uid = Uid {1},
         .name = "gp1",
         .generator = Uid {1},
         .profile = 0.5,
         .scost = 10.0},
    };
    const System sys = {
        .name = "LegacyGenProfile",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .generator_profile_array = gp_array,
    };
    PlanningOptions opts;
    opts.model_options.scale_objective = 1.0;
    const PlanningOptionsLP options {opts};
    SimulationLP sim_lp(sim, options);
    SystemLP system_lp(sys, sim_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());
    legacy_obj = lp.get_obj_value();
    legacy_rows = lp.get_numrows();
    legacy_cols = lp.get_numcols();
  }

  // Unified path: same numbers via CapacityProfile{owner_kind = Generator}.
  double unified_obj {};
  int unified_rows {};
  int unified_cols {};
  {
    CapacityProfile p;
    p.uid = Uid {1};
    p.name = "gp1";
    p.owner_kind = ProfileOwnerKind::Generator;
    p.owner = Uid {1};
    p.profile = 0.5;
    p.scost = 10.0;

    const System sys = {
        .name = "UnifiedGenProfile",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .capacity_profile_array = Array<CapacityProfile> {p},
    };
    PlanningOptions opts;
    opts.model_options.scale_objective = 1.0;
    const PlanningOptionsLP options {opts};
    SimulationLP sim_lp(sim, options);
    SystemLP system_lp(sys, sim_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());
    unified_obj = lp.get_obj_value();
    unified_rows = lp.get_numrows();
    unified_cols = lp.get_numcols();
  }

  CHECK(legacy_obj == doctest::Approx(unified_obj).epsilon(1e-9));
  CHECK(legacy_rows == unified_rows);
  CHECK(legacy_cols == unified_cols);
}

TEST_CASE(
    "CapacityProfileLP (Demand kind) builds the same LP as DemandProfileLP")  // NOLINT
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
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 100.0,
      },
  };

  const auto sim = make_single_block_sim();

  double legacy_obj {};
  int legacy_rows {};
  int legacy_cols {};
  {
    const Array<DemandProfile> dp_array = {
        {.uid = Uid {1}, .name = "dp1", .demand = Uid {1}, .profile = 0.6},
    };
    const System sys = {
        .name = "LegacyDemandProfile",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .demand_profile_array = dp_array,
    };
    PlanningOptions opts;
    opts.model_options.scale_objective = 1.0;
    const PlanningOptionsLP options {opts};
    SimulationLP sim_lp(sim, options);
    SystemLP system_lp(sys, sim_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());
    legacy_obj = lp.get_obj_value();
    legacy_rows = lp.get_numrows();
    legacy_cols = lp.get_numcols();
  }

  double unified_obj {};
  int unified_rows {};
  int unified_cols {};
  {
    CapacityProfile p;
    p.uid = Uid {1};
    p.name = "dp1";
    p.owner_kind = ProfileOwnerKind::Demand;
    p.owner = Uid {1};
    p.profile = 0.6;

    const System sys = {
        .name = "UnifiedDemandProfile",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .capacity_profile_array = Array<CapacityProfile> {p},
    };
    PlanningOptions opts;
    opts.model_options.scale_objective = 1.0;
    const PlanningOptionsLP options {opts};
    SimulationLP sim_lp(sim, options);
    SystemLP system_lp(sys, sim_lp);

    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());
    unified_obj = lp.get_obj_value();
    unified_rows = lp.get_numrows();
    unified_cols = lp.get_numcols();
  }

  CHECK(legacy_obj == doctest::Approx(unified_obj).epsilon(1e-9));
  CHECK(legacy_rows == unified_rows);
  CHECK(legacy_cols == unified_cols);
}

// ── Commit 2 — legacy → unified folding ──────────────────────────────

TEST_CASE(  // NOLINT
    "System::fold_legacy_profiles moves both legacy arrays into unified one")
{
  System sys;
  sys.generator_profile_array = {
      {.uid = Uid {11},
       .name = "gp_legacy",
       .generator = Uid {7},
       .profile = 0.42,
       .scost = 1.5},
  };
  sys.demand_profile_array = {
      {.uid = Uid {22},
       .name = "dp_legacy",
       .demand = Uid {3},
       .profile = 0.77},
  };

  sys.fold_legacy_profiles();

  CHECK(sys.generator_profile_array.empty());
  CHECK(sys.demand_profile_array.empty());
  REQUIRE(sys.capacity_profile_array.size() == 2);

  const auto find_by_uid = [&](Uid u) -> const CapacityProfile&
  {
    auto it = std::ranges::find_if(sys.capacity_profile_array,
                                   [u](const auto& p) { return p.uid == u; });
    REQUIRE(it != sys.capacity_profile_array.end());
    return *it;
  };
  const auto& g = find_by_uid(Uid {11});
  CHECK(g.owner_kind == ProfileOwnerKind::Generator);
  CHECK(g.name == "gp_legacy");
  CHECK(std::get<Uid>(g.owner) == Uid {7});

  const auto& d = find_by_uid(Uid {22});
  CHECK(d.owner_kind == ProfileOwnerKind::Demand);
  CHECK(d.name == "dp_legacy");
  CHECK(std::get<Uid>(d.owner) == Uid {3});

  sys.fold_legacy_profiles();  // idempotent
  CHECK(sys.capacity_profile_array.size() == 2);
}

TEST_CASE(  // NOLINT
    "Legacy generator_profile_array folded yields identical LP to direct "
    "legacy path")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 0.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "backup",
       .bus = Uid {1},
       .gcost = 100.0,
       .capacity = 200.0},
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };
  const Array<GeneratorProfile> gp_array = {
      {.uid = Uid {1},
       .name = "gp1",
       .generator = Uid {1},
       .profile = 0.5,
       .scost = 10.0},
  };
  const auto sim = make_single_block_sim();

  PlanningOptions opts;
  opts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {opts};
  SimulationLP sim_lp(sim, options);

  double direct_obj {};
  int direct_rows {};
  int direct_cols {};
  {
    const System sys = {
        .name = "LegacyDirect",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .generator_profile_array = gp_array,
    };
    SystemLP system_lp(sys, sim_lp);
    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());
    direct_obj = lp.get_obj_value();
    direct_rows = lp.get_numrows();
    direct_cols = lp.get_numcols();
  }

  double folded_obj {};
  int folded_rows {};
  int folded_cols {};
  {
    System sys = {
        .name = "FoldedViaUnified",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .generator_profile_array = gp_array,
    };
    sys.fold_legacy_profiles();
    CHECK(sys.generator_profile_array.empty());
    REQUIRE(sys.capacity_profile_array.size() == 1);

    SystemLP system_lp(sys, sim_lp);
    auto&& lp = system_lp.linear_interface();
    REQUIRE(lp.resolve().has_value());
    folded_obj = lp.get_obj_value();
    folded_rows = lp.get_numrows();
    folded_cols = lp.get_numcols();
  }

  CHECK(direct_obj == doctest::Approx(folded_obj).epsilon(1e-9));
  CHECK(direct_rows == folded_rows);
  CHECK(direct_cols == folded_cols);
}
