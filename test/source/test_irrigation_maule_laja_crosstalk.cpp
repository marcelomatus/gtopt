/**
 * @file      test_irrigation_maule_laja_crosstalk.cpp
 * @brief     Tier 11 — Maule + Laja together (cross-talk regression)
 * @date      2026-04-10
 * @copyright BSD-3-Clause
 *
 * End-to-end regression tests for running two independent irrigation
 * agreements (Maule-shaped and Laja-shaped) inside a single LP.  The
 * concern is that the decentralized AMPL variable registry introduced
 * in commit f02856d7 uses a composite key `(class, uid, attribute,
 * scenario, stage)`, and any accidental collision between two
 * subsystems would silently corrupt one side's column mapping.  These
 * tests exercise the co-existence directly:
 *
 *   11.1 — combined Maule+Laja LP is feasible
 *   11.2 — each subsystem keeps its own registered columns (no key
 *          collision between the two subsystems' rights)
 *   11.3 — a UserConstraint spanning a Maule right and a Laja right
 *          resolves both references correctly (and binds when the
 *          implied coupling is tight)
 *
 * The test fixture mounts two independent hydro cascades on a single
 * electric bus.  Each cascade has its own reservoir, waterway,
 * turbine, hydro generator, and drain junction — so there is no
 * hydrological coupling between the Maule and Laja halves.  The only
 * shared structure is the electric bus (to keep the LP trivially
 * feasible) and a single thermal generator.  All Uids are chosen in
 * disjoint ranges (Maule: 1-19 for backbone, 20-39 for rights; Laja:
 * 50-69 for backbone, 70-89 for rights).
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Shared two-subsystem backbone: one electric bus, one thermal back-fill,
// one demand, plus two independent hydro cascades (Maule + Laja).
struct CrosstalkFixture
{
  Array<Bus> bus_array {
      {
          .uid = Uid {1},
          .name = "b_main",
      },
  };

  Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "hydro_maule",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 300.0,
      },
      {
          .uid = Uid {2},
          .name = "hydro_laja",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 300.0,
      },
      {
          .uid = Uid {3},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };

  Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d_main",
          .bus = Uid {1},
          .capacity = 150.0,
      },
  };

  // Maule cascade: Uids 10-19 (junctions/waterway/turbine/reservoir).
  // Laja cascade:  Uids 50-59 (same shape, disjoint Uids).
  Array<Junction> junction_array {
      // Maule
      {
          .uid = Uid {10},
          .name = "j_maule_up",
      },
      {
          .uid = Uid {11},
          .name = "j_maule_down",
          .drain = true,
      },
      // Laja
      {
          .uid = Uid {50},
          .name = "j_laja_up",
      },
      {
          .uid = Uid {51},
          .name = "j_laja_down",
          .drain = true,
      },
  };

  Array<Waterway> waterway_array {
      {
          .uid = Uid {10},
          .name = "ww_maule",
          .junction_a = Uid {10},
          .junction_b = Uid {11},
          .fmin = 0.0,
          .fmax = 800.0,
      },
      {
          .uid = Uid {50},
          .name = "ww_laja",
          .junction_a = Uid {50},
          .junction_b = Uid {51},
          .fmin = 0.0,
          .fmax = 800.0,
      },
  };

  Array<Reservoir> reservoir_array {
      {
          .uid = Uid {10},
          .name = "rsv_maule",
          .junction = Uid {10},
          .capacity = 1'000'000.0,
          .emin = 0.0,
          .emax = 1'000'000.0,
          .eini = 1'000'000.0,
      },
      {
          .uid = Uid {50},
          .name = "rsv_laja",
          .junction = Uid {50},
          .capacity = 1'000'000.0,
          .emin = 0.0,
          .emax = 1'000'000.0,
          .eini = 1'000'000.0,
      },
  };

  Array<Turbine> turbine_array {
      {
          .uid = Uid {10},
          .name = "tur_maule",
          .waterway = Uid {10},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
      {
          .uid = Uid {50},
          .name = "tur_laja",
          .waterway = Uid {50},
          .generator = Uid {2},
          .production_factor = 2.0,
      },
  };
};

[[nodiscard]] Simulation make_crosstalk_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 24,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
                  .month = MonthType::april,
              },
              {
                  .uid = Uid {2},
                  .first_block = 0,
                  .count_block = 1,
                  .month = MonthType::may,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
}

[[nodiscard]] System make_crosstalk_system(const CrosstalkFixture& fx,
                                           Array<VolumeRight> vrs,
                                           Array<FlowRight> frs,
                                           Array<UserConstraint> ucs,
                                           std::string name)
{
  return {
      .name = std::move(name),
      .bus_array = fx.bus_array,
      .demand_array = fx.demand_array,
      .generator_array = fx.generator_array,
      .junction_array = fx.junction_array,
      .waterway_array = fx.waterway_array,
      .reservoir_array = fx.reservoir_array,
      .turbine_array = fx.turbine_array,
      .flow_right_array = std::move(frs),
      .volume_right_array = std::move(vrs),
      .user_constraint_array = std::move(ucs),
  };
}

// Maule-flavoured rights: drain into junction 11, VR couples to
// reservoir 10.  Uids 20-29 (FR), 30-39 (VR).
[[nodiscard]] Array<FlowRight> make_maule_rights_frs()
{
  return {
      {
          .uid = Uid {20},
          .name = "fr_m_irr",
          .junction = Uid {11},
          .direction = -1,
          .discharge = 8.0,
          .fail_cost = 4000.0,
      },
      {
          .uid = Uid {21},
          .name = "fr_m_eco",
          .junction = Uid {11},
          .direction = -1,
          .discharge = 4.0,
          .fail_cost = 8000.0,
      },
  };
}

[[nodiscard]] Array<VolumeRight> make_maule_rights_vrs()
{
  return {
      {
          .uid = Uid {30},
          .name = "vr_m_embalsar",
          .reservoir = Uid {10},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 5.0,
          .fail_cost = 2000.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {31},
          .name = "vr_m_district",
          .reservoir = Uid {10},
          .emax = 300.0,
          .eini = 300.0,
          .demand = 8.0,
          .fail_cost = 3500.0,
          .use_state_variable = false,
      },
  };
}

// Laja-flavoured rights: drain into junction 51, VR couples to
// reservoir 50.  Uids 70-79 (FR), 80-89 (VR).
[[nodiscard]] Array<FlowRight> make_laja_rights_frs()
{
  return {
      {
          .uid = Uid {70},
          .name = "fr_l_canal",
          .junction = Uid {51},
          .direction = -1,
          .discharge = 8.0,
          .fail_cost = 4000.0,
      },
      {
          .uid = Uid {71},
          .name = "fr_l_eco",
          .junction = Uid {51},
          .direction = -1,
          .discharge = 4.0,
          .fail_cost = 8000.0,
      },
  };
}

[[nodiscard]] Array<VolumeRight> make_laja_rights_vrs()
{
  return {
      {
          .uid = Uid {80},
          .name = "vr_l_econ",
          .reservoir = Uid {50},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 5.0,
          .fail_cost = 2000.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {81},
          .name = "vr_l_canal_x",
          .reservoir = Uid {50},
          .emax = 300.0,
          .eini = 300.0,
          .demand = 8.0,
          .fail_cost = 3500.0,
          .use_state_variable = false,
      },
  };
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Tier 11.1 — combined Maule+Laja LP is feasible
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 11.1 - combined Maule+Laja two-cascade LP is feasible")
{
  const CrosstalkFixture fx;
  auto vrs = make_maule_rights_vrs();
  for (auto&& lvr : make_laja_rights_vrs()) {
    vrs.emplace_back(std::move(lvr));
  }
  auto frs = make_maule_rights_frs();
  for (auto&& lfr : make_laja_rights_frs()) {
    frs.emplace_back(std::move(lfr));
  }

  const auto system = make_crosstalk_system(
      fx, std::move(vrs), std::move(frs), {}, "Tier11_1_Combined");
  const PlanningOptionsLP options;
  const auto simulation = make_crosstalk_simulation();
  SimulationLP simulation_lp(simulation, options);
  simulation_lp.set_need_ampl_variables(true);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 11.2 — registry isolation: Maule and Laja columns stay disjoint
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 11.2 - Maule and Laja rights occupy distinct registry columns")
{
  // Every Maule right and every Laja right must get its own
  // ColIndex in the (scene, phase) AMPL registry cell.  Any key
  // collision (e.g. attribute namespace clash, missing class_name in
  // the key, shared uid bucket) would cause `find_ampl_col` to
  // return the same column for two different rights — which would
  // silently corrupt downstream UserConstraint resolution.
  const CrosstalkFixture fx;
  auto vrs = make_maule_rights_vrs();
  for (auto&& lvr : make_laja_rights_vrs()) {
    vrs.emplace_back(std::move(lvr));
  }
  auto frs = make_maule_rights_frs();
  for (auto&& lfr : make_laja_rights_frs()) {
    frs.emplace_back(std::move(lfr));
  }

  const auto system = make_crosstalk_system(
      fx, std::move(vrs), std::move(frs), {}, "Tier11_2_Isolation");
  const PlanningOptionsLP options;
  const auto simulation = make_crosstalk_simulation();
  SimulationLP simulation_lp(simulation, options);
  simulation_lp.set_need_ampl_variables(true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(scenarios.size() == 1);
  REQUIRE(stages.size() == 2);
  const auto sc_uid = scenarios[0].uid();

  // Collect every (volume_right.extraction) and (flow_right.flow)
  // column across all four rights and both stages.  Each lookup must
  // succeed; the full set of ColIndex values must be pairwise
  // distinct — a duplicate would prove the keying is broken.
  std::vector<ColIndex> cols;
  cols.reserve(16);

  constexpr std::array<Uid, 4> vr_uids {Uid {30}, Uid {31}, Uid {80}, Uid {81}};
  constexpr std::array<Uid, 4> fr_uids {Uid {20}, Uid {21}, Uid {70}, Uid {71}};

  for (const auto& st : stages) {
    const auto st_uid = st.uid();
    const auto bk_uid = st.blocks().front().uid();
    for (const auto vuid : vr_uids) {
      const auto col = sc.find_ampl_col(
          "volume_right", vuid, "extraction", sc_uid, st_uid, bk_uid);
      REQUIRE(col.has_value());
      cols.push_back(*col);
    }
    for (const auto fuid : fr_uids) {
      const auto col =
          sc.find_ampl_col("flow_right", fuid, "flow", sc_uid, st_uid, bk_uid);
      REQUIRE(col.has_value());
      cols.push_back(*col);
    }
  }

  // All 16 column indices must be pairwise distinct.
  std::ranges::sort(cols);
  const auto dup_it = std::ranges::adjacent_find(cols);
  CHECK(dup_it == cols.end());
  CHECK(cols.size() == 16);
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 11.3 — UserConstraint spanning both subsystems resolves + binds
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 11.3 - cross-subsystem UserConstraint resolves Maule+Laja terms")
{
  // A single UserConstraint that mixes one Maule FlowRight and one
  // Laja FlowRight must resolve both references through the same
  // registry pass (Tier 6 guarantees per-subsystem registration;
  // Tier 11 guarantees they can be cross-referenced from one UC).
  // Pin the eco flows together: `fr_m_eco - fr_l_eco == 0`.  This
  // is structurally trivial — both sides are free variables in
  // otherwise-symmetric sub-cascades — so the solve must remain
  // feasible and the registry must carry the binding through.
  const CrosstalkFixture fx;
  auto vrs = make_maule_rights_vrs();
  for (auto&& lvr : make_laja_rights_vrs()) {
    vrs.emplace_back(std::move(lvr));
  }
  auto frs = make_maule_rights_frs();
  for (auto&& lfr : make_laja_rights_frs()) {
    frs.emplace_back(std::move(lfr));
  }

  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "eco_pair",
          .expression =
              R"(flow_right("fr_m_eco").flow - flow_right("fr_l_eco").flow == 0)",
          .constraint_type = "raw",
      },
      {
          .uid = Uid {2},
          .name = "vr_pair",
          .expression =
              R"(volume_right("vr_m_district").extraction - volume_right("vr_l_canal_x").extraction == 0)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_crosstalk_system(
      fx, std::move(vrs), std::move(frs), ucs, "Tier11_3_CrossSystemUC");
  const PlanningOptionsLP options;
  const auto simulation = make_crosstalk_simulation();
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // Verify the cross-system UCs actually tied the LP solution
  // values together (i.e. both expressions resolved and bound the
  // right columns — not silently skipped).
  const auto sol = lp.get_col_sol();
  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto sc_uid = scenarios[0].uid();

  for (const auto& st : stages) {
    const auto st_uid = st.uid();
    const auto bk_uid = st.blocks().front().uid();

    const auto m_eco = sc.find_ampl_col(
        "flow_right", Uid {21}, "flow", sc_uid, st_uid, bk_uid);
    const auto l_eco = sc.find_ampl_col(
        "flow_right", Uid {71}, "flow", sc_uid, st_uid, bk_uid);
    const auto m_vr = sc.find_ampl_col(
        "volume_right", Uid {31}, "extraction", sc_uid, st_uid, bk_uid);
    const auto l_vr = sc.find_ampl_col(
        "volume_right", Uid {81}, "extraction", sc_uid, st_uid, bk_uid);
    REQUIRE(m_eco.has_value());
    REQUIRE(l_eco.has_value());
    REQUIRE(m_vr.has_value());
    REQUIRE(l_vr.has_value());

    CHECK(sol[*m_eco] == doctest::Approx(sol[*l_eco]).epsilon(1e-6));
    CHECK(sol[*m_vr] == doctest::Approx(sol[*l_vr]).epsilon(1e-6));
  }
}
