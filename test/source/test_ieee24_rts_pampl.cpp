/**
 * @file      test_ieee24_rts_pampl.cpp
 * @brief     Integration tests: AMPL-derived DC-OPF operating limits, written
 *            in pseudo-AMPL (.pampl), loaded and solved on an IEEE RTS-style
 *            DC-OPF network.
 * @date      2026-06-27
 * @copyright BSD-3-Clause
 *
 * These tests translate the constraint families of a classic AMPL DC-OPF
 * model — the IEEE Reliability Test System lossless DC-OPF distributed with
 * rpglab/PSOPT_Models (`DCOPF/LosslessDCOPF.mod`) — into gtopt
 * user-constraint syntax and verify that each family survives the full PAMPL
 * pipeline (`PamplParser` -> `UserConstraint` -> LP assembly -> solve) and
 * actually changes the dispatch.
 *
 * The reference AMPL model:
 *   minimize Cost: BaseMVA * sum{i in GEN} gen_supply[i] * gen_C[i];
 *   PGenMaxMin{i}: gen_Pmin[i] <= gen_supply[i] <= gen_Pmax[i];
 *   Line_Flow{j}:  line_flow[j] = (theta_from - theta_to) / branch_x[j];
 *   Thermal1{j}:   -rateA[j] <= line_flow[j] <= rateA[j];
 *
 * The objective, per-generator bounds, the DC line-flow identity and the
 * per-line thermal rating are assembled natively by gtopt from the JSON
 * model.  The PAMPL rows exercised here are the *additional* operating
 * policies: group caps, corridor limits, system-wide caps, a soft
 * spinning-reserve floor (penalty + named slack), a dur-weighted energy
 * budget (`sum{b in stage} dur[b] * ...`), a per-block scheduled RHS, and
 * `for(...)` domain scoping.
 *
 * The network is a compact 3-bus DC-OPF whose generator costs mirror the
 * RTS-24 merit order ($4.4231 base-load, $12.3883 mid-merit, $48.5804
 * peaker; cf. cases/ieee_24b_rts/ieee_24b_rts.json).  Keeping it small makes
 * the optimal dispatch hand-verifiable so the binding assertions are crisp.
 * The companion example `cases/ieee_24b_rts/uc_opf_limits.pampl` carries the
 * same constraint families against the full RTS-24 element names for CLI use.
 */

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/pampl_parser.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions;
// the nested anonymous namespace gives the helpers internal linkage.
namespace rts_pampl_test
{
namespace
{

// clang-format off

/// 3-bus RTS-style DC-OPF, single stage / single block, `scale_objective = 1`
/// so the raw objective equals the physical dispatch cost.
///   b1: base-load g_base  $4.4231/MWh  pmax 400
///   b2: mid-merit g_mid   $12.3883/MWh pmax 155
///   b3: peaker    g_peak  $48.5804/MWh pmax 300   (reference bus, 300 MW load)
/// All three lines rated 250 MW, reactance 0.05.  Unconstrained the load is
/// served entirely from g_base (300 MW, cost 1326.93).
/// `{}` is replaced verbatim with the user_constraint_array body so each
/// test can splice in its own PAMPL-derived rows.
auto make_dcopf_json(std::string_view uc_body) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": false,
        "use_kirchhoff": true,
        "scale_objective": 1,
        "demand_fail_cost": 1000
      }}
    }},
    "simulation": {{
      "block_array": [ {{ "uid": 1, "duration": 1 }} ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "rts_dcopf_pampl",
      "bus_array": [
        {{ "uid": 1, "name": "b1" }},
        {{ "uid": 2, "name": "b2" }},
        {{ "uid": 3, "name": "b3", "reference_theta": 0 }}
      ],
      "generator_array": [
        {{ "uid": 1, "name": "g_base", "bus": "b1", "pmin": 0, "pmax": 400,
           "gcost": 4.4231,  "capacity": 400 }},
        {{ "uid": 2, "name": "g_mid",  "bus": "b2", "pmin": 0, "pmax": 155,
           "gcost": 12.3883, "capacity": 155 }},
        {{ "uid": 3, "name": "g_peak", "bus": "b3", "pmin": 0, "pmax": 300,
           "gcost": 48.5804, "capacity": 300 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d_load", "bus": "b3", "lmax": [ [ 300.0 ] ] }}
      ],
      "line_array": [
        {{ "uid": 1, "name": "l1_3", "bus_a": "b1", "bus_b": "b3",
           "reactance": 0.05, "tmax_ab": 250, "tmax_ba": 250 }},
        {{ "uid": 2, "name": "l2_3", "bus_a": "b2", "bus_b": "b3",
           "reactance": 0.05, "tmax_ab": 250, "tmax_ba": 250 }},
        {{ "uid": 3, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
           "reactance": 0.05, "tmax_ab": 250, "tmax_ba": 250 }}
      ],
      "user_constraint_array": [ {} ]
    }}
  }})",
      uc_body);
}

/// Single-bus, single-stage, 3-block system (durations 8h each) for the
/// `sum{b in stage} dur[b] * ...` energy-budget test.  One generator at
/// $5/MWh serves 100 MW in every block; total energy demand 2400 MWh.
auto make_energy_json(std::string_view uc_body) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 1000
      }}
    }},
    "simulation": {{
      "block_array": [
        {{ "uid": 1, "duration": 8 }},
        {{ "uid": 2, "duration": 8 }},
        {{ "uid": 3, "duration": 8 }}
      ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 3, "active": 1 }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "rts_energy_pampl",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
           "gcost": 5, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1",
           "lmax": [ [ 100.0, 100.0, 100.0 ] ] }}
      ],
      "user_constraint_array": [ {} ]
    }}
  }})",
      uc_body);
}

// clang-format on

/// Append every constraint and param parsed from `pampl_src` onto `planning`,
/// exactly as `load_user_constraints()` does for a `.pampl` file.  UIDs start
/// after any inline constraints already present.
void attach_pampl(Planning& planning, std::string_view pampl_src)
{
  Uid next_uid = Uid {1};
  for (const auto& uc : planning.system.user_constraint_array) {
    if (uc.uid >= next_uid) {
      next_uid = uc.uid + Uid {1};
    }
  }
  auto parsed = PamplParser::parse(pampl_src, next_uid);

  auto& arr = planning.system.user_constraint_array;
  arr.insert(arr.end(),
             std::make_move_iterator(parsed.constraints.begin()),
             std::make_move_iterator(parsed.constraints.end()));

  auto& parr = planning.system.user_param_array;
  parr.insert(parr.end(),
              std::make_move_iterator(parsed.params.begin()),
              std::make_move_iterator(parsed.params.end()));
}

/// Build + solve the DC-OPF with the given PAMPL body and return the first
/// cell's raw objective (== physical cost, scale_objective = 1).
[[nodiscard]] auto solve_dcopf_obj(std::string_view pampl_src) -> double
{
  Planning base;
  base.merge(parse_planning_json(make_dcopf_json("")));
  if (!pampl_src.empty()) {
    attach_pampl(base, pampl_src);
  }
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
}

/// Build + solve the energy-budget system and return the first cell's raw
/// objective.
[[nodiscard]] auto solve_energy_obj(std::string_view pampl_src) -> double
{
  Planning base;
  base.merge(parse_planning_json(make_energy_json("")));
  if (!pampl_src.empty()) {
    attach_pampl(base, pampl_src);
  }
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
}

/// Unconstrained base cost: 300 MW from g_base @ $4.4231 = 1326.93.
constexpr double kBaseObj = 300.0 * 4.4231;

}  // namespace
}  // namespace rts_pampl_test

TEST_CASE("RTS-24 PAMPL - baseline DC-OPF dispatches the cheapest unit")
{
  using namespace rts_pampl_test;

  // Sanity anchor: with no user constraints the load is served entirely by
  // the $4.4231/MWh base-load unit.
  CHECK(solve_dcopf_obj("") == doctest::Approx(kBaseObj));
}

TEST_CASE(
    "RTS-24 PAMPL - group capacity cap (PGenMaxMin) forces costlier dispatch")
{
  using namespace rts_pampl_test;
  // AMPL PGenMaxMin, aggregated: cap the base-load unit below its optimal
  // 300 MW so 100 MW must come from the $12.3883/MWh mid-merit unit.
  constexpr std::string_view src =
      "constraint base_load_cap \"base-load group cap\":\n"
      "  generator('g_base').generation <= 200;";

  const double obj = solve_dcopf_obj(src);
  // 200 @ 4.4231 + 100 @ 12.3883 = 884.62 + 1238.83 = 2123.45.
  CHECK(obj == doctest::Approx(200.0 * 4.4231 + 100.0 * 12.3883));
  CHECK(obj > kBaseObj + 1.0);
}

TEST_CASE("RTS-24 PAMPL - sum() element aggregation cap is binding")
{
  using namespace rts_pampl_test;
  // System-wide style cap on a generator group: Σ over {g_base, g_mid}
  // <= 250 forces at least 50 MW onto the peaker.
  constexpr std::string_view src =
      "constraint cheap_group_cap:\n"
      "  sum(generator('g_base','g_mid').generation) <= 250;";

  const double obj = solve_dcopf_obj(src);
  // 250 cheap @ 4.4231 + 50 peaker @ 48.5804 = 1105.78 + 2429.02 = 3534.79.
  CHECK(obj == doctest::Approx(250.0 * 4.4231 + 50.0 * 48.5804));
  CHECK(obj > kBaseObj + 1.0);
}

TEST_CASE(
    "RTS-24 PAMPL - corridor thermal limit (Thermal1/2) builds and solves")
{
  using namespace rts_pampl_test;
  // AMPL Thermal1/Thermal2: a two-sided signed flow limit on a corridor,
  // tighter than the line's native rating.  The LP must remain feasible
  // (demand can reroute / redispatch) and never cheaper than the base.
  constexpr std::string_view src =
      "constraint corridor_fwd: line('l1_3').flow <= 150;\n"
      "constraint corridor_rev: line('l1_3').flow >= -150;";

  const double obj = solve_dcopf_obj(src);
  CHECK(obj >= kBaseObj - 1e-6);
}

TEST_CASE("RTS-24 PAMPL - soft spinning-reserve floor (penalty + named slack)")
{
  using namespace rts_pampl_test;
  // Soft constraint with a per-UC named slack column.  The floor (50 MW of
  // mid-merit) is cheaper to honour ($7.965/MWh redispatch) than to violate
  // (penalty $1000/MWh), so the optimizer satisfies it physically.
  constexpr std::string_view src =
      "param reserve_penalty = 1000;\n"
      "var slack_mid_floor;\n"
      "constraint mid_floor \"reserve floor\" penalty reserve_penalty:\n"
      "  generator('g_mid').generation >= 50;";

  const double obj = solve_dcopf_obj(src);
  // 250 @ 4.4231 + 50 @ 12.3883 = 1105.78 + 619.42 = 1725.19 (no penalty).
  CHECK(obj == doctest::Approx(250.0 * 4.4231 + 50.0 * 12.3883));
  CHECK(obj > kBaseObj + 1.0);
}

TEST_CASE("RTS-24 PAMPL - soft floor is violated (penalty paid) when cheaper")
{
  using namespace rts_pampl_test;
  // An unsatisfiable floor (g_mid pmax is 155) with a tiny penalty: the LP
  // stays feasible by paying the slack rather than the constraint rendering
  // it infeasible — the defining property of a soft constraint.
  constexpr std::string_view src =
      "var slack_huge;\n"
      "constraint huge_floor penalty 0.001:\n"
      "  generator('g_mid').generation >= 1000;";

  // Just needs to solve; the slack absorbs the 845+ MW shortfall.
  const double obj = solve_dcopf_obj(src);
  CHECK(obj >= kBaseObj - 1e-6);
}

TEST_CASE("RTS-24 PAMPL - dur-weighted energy budget (sum{} time aggregation)")
{
  using namespace rts_pampl_test;
  // `sum{b in stage} dur[b] * generation` caps ENERGY (MWh).  Demand is
  // 2400 MWh; a 1200 MWh budget forces 1200 MWh of curtailment at
  // $1000/MWh, dwarfing the $5/MWh generation cost.
  constexpr std::string_view unbudgeted = "";
  const double base_energy = solve_energy_obj(unbudgeted);
  CHECK(base_energy == doctest::Approx(5.0 * 100.0 * 24.0));  // 12000

  constexpr std::string_view src =
      "constraint energy_budget:\n"
      "  sum{b in stage} dur[b] * generator('g1').generation <= 1200;";
  const double obj = solve_energy_obj(src);
  CHECK(obj > base_energy + 1.0);
  // 1200 MWh served @ 5 + 1200 MWh failed @ 1000 = 6000 + 1.2e6.
  CHECK(obj == doctest::Approx(5.0 * 1200.0 + 1000.0 * 1200.0));
}

TEST_CASE("RTS-24 PAMPL - unweighted sum{} caps per-block MW, not energy")
{
  using namespace rts_pampl_test;
  // Without `dur[b] *` the row caps the SUM of per-block MW (3 blocks), not
  // energy: <= 90 means total MW across blocks <= 90, i.e. average 30 MW/block
  // against 100 MW demand — heavy curtailment, distinct from the energy form.
  constexpr std::string_view src =
      "constraint mw_count_cap:\n"
      "  sum{b in stage} generator('g1').generation <= 90;";
  const double obj = solve_energy_obj(src);
  CHECK(obj > 5.0 * 100.0 * 24.0 + 1.0);  // strictly above the unbudgeted cost
}

TEST_CASE("RTS-24 PAMPL - per-block scheduled RHS overrides the inline tail")
{
  using namespace rts_pampl_test;
  // The inline `<= 0` is the fallback; the rhs vector overrides it per block.
  // A 150 MW ceiling on the single block caps g1 below the 300 MW it would
  // otherwise produce in the DC-OPF, forcing 150 MW onto the mid-merit unit.
  constexpr std::string_view src =
      "constraint g_base_schedule rhs [200]:\n"
      "  generator('g_base').generation <= 0;";

  const double obj = solve_dcopf_obj(src);
  // g_base capped at 200 -> 100 MW from g_mid (same as base_load_cap).
  CHECK(obj == doctest::Approx(200.0 * 4.4231 + 100.0 * 12.3883));
  CHECK(obj > kBaseObj + 1.0);
}

TEST_CASE("RTS-24 PAMPL - for(...) domain scoping and inactive keyword")
{
  using namespace rts_pampl_test;
  // A `for(stage in all, block in all)` domain clause plus an `inactive`
  // row that must contribute nothing.  The active cap on g_base binds; the
  // inactive one (a 1 MW ceiling) would be wildly binding if it took effect.
  constexpr std::string_view src =
      "constraint windowed_cap:\n"
      "  generator('g_base').generation <= 200, for(stage in all, block in "
      "all);\n"
      "inactive constraint disabled_cap:\n"
      "  generator('g_base').generation <= 1;";

  const double obj = solve_dcopf_obj(src);
  // Only the active 200 MW cap applies.
  CHECK(obj == doctest::Approx(200.0 * 4.4231 + 100.0 * 12.3883));
}

TEST_CASE("RTS-24 PAMPL - multiple feature families compose in one file")
{
  using namespace rts_pampl_test;
  // A realistic operating-policy bundle: param + named slack + group cap +
  // soft reserve floor + system cap + scheduled RHS, all in one block, must
  // parse, build and solve together.
  constexpr std::string_view src = R"pampl(
    param reserve_penalty = 1000;
    var slack_mid_floor;

    constraint base_load_cap "base-load group cap":
      generator('g_base').generation <= 220;

    constraint mid_floor "reserve floor" penalty reserve_penalty:
      generator('g_mid').generation >= 40;

    constraint system_cap:
      sum(generator(all).generation) <= 855;

    constraint peak_schedule rhs [250]:
      generator('g_peak').generation <= 0;
  )pampl";

  const double obj = solve_dcopf_obj(src);
  // Feasible and costlier than the unconstrained base (cap forces redispatch).
  CHECK(obj > kBaseObj + 1.0);
}

TEST_CASE("RTS-24 PAMPL - parsed UserConstraint fields are populated correctly")
{
  using namespace rts_pampl_test;
  // Verify the PAMPL header metadata (description, penalty, rhs schedule,
  // slack_name, active flag) round-trips into UserConstraint, independent of
  // the LP solve.
  constexpr std::string_view src = R"pampl(
    param pen = 250;
    var slack_floor;
    constraint floor "reserve floor" penalty pen:
      generator('g_mid').generation >= 40;
    constraint sched rhs [10, 20, 30]:
      generator('g_base').generation <= 0;
    inactive constraint off_row:
      generator('g_base').generation <= 5;
  )pampl";

  const auto parsed = PamplParser::parse(src, Uid {7});
  REQUIRE(parsed.constraints.size() == 3);
  REQUIRE(parsed.params.size() == 1);
  REQUIRE(parsed.declared_vars.size() == 1);

  const auto& floor = parsed.constraints[0];
  CHECK(floor.uid == 7);
  CHECK(floor.name == "floor");
  CHECK(floor.description.value_or("") == "reserve floor");
  CHECK(floor.penalty.value_or(-1.0) == doctest::Approx(250.0));
  REQUIRE(floor.slack_name.has_value());
  CHECK(floor.slack_name.value() == "slack_floor");

  const auto& sched = parsed.constraints[1];
  CHECK(sched.name == "sched");
  REQUIRE(sched.rhs.has_value());

  const auto& off_row = parsed.constraints[2];
  CHECK(off_row.active.value_or(true) == false);
}

TEST_CASE("RTS-24 PAMPL - .pampl file round-trips through parse_file()")
{
  using namespace rts_pampl_test;
  // Exercise the filesystem-loading API (the path otherwise only hit by
  // gtopt_main): write a small .pampl, read it back via parse_file(), attach
  // its constraints, then build + solve the DC-OPF.
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_rts_pampl_roundtrip";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);
  const auto pampl_path = tmpdir / "uc_limits.pampl";

  {
    std::ofstream out(pampl_path);
    out << "# DC-OPF limits for the round-trip test\n"
        << "constraint base_load_cap: generator('g_base').generation <= 200;\n"
        << "constraint corridor: line('l1_3').flow <= 240;\n";
  }

  const auto parsed = PamplParser::parse_file(pampl_path.string(), Uid {1});
  REQUIRE(parsed.constraints.size() == 2);
  CHECK(parsed.constraints[0].name == "base_load_cap");

  Planning base;
  base.merge(parse_planning_json(make_dcopf_json("")));
  auto& arr = base.system.user_constraint_array;
  for (const auto& uc : parsed.constraints) {
    arr.push_back(uc);
  }

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const double obj =
      systems.front().front().linear_interface().get_obj_value_raw();
  // base_load_cap forces the same 200/100 redispatch as the inline test.
  CHECK(obj == doctest::Approx(200.0 * 4.4231 + 100.0 * 12.3883));

  std::filesystem::remove_all(tmpdir);
}
