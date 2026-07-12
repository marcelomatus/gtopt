// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_negative_lmp_reserve.cpp — mode × channel exposure
// under mechanism M3: operational constraints (reserves).
//
// A DOWN-reserve requirement forces a generator's energy output UP:
// the provision row is `p − dr ≥ p_min` (reserve_provision_lp.cpp,
// dprov_row) and the zone row is a HARD `Σ pf·dr ≥ drreq` when no
// shortage cost is configured (reserve_zone_lp.cpp, add_requirement).
// With a single provider this pins `p ≥ drreq` — a reserve-driven
// must-run.
//
// WHY pure reserve alone cannot flip a bus dual negative here: gtopt's
// bus balance is an EQUALITY row whose only slack (demand_fail) is
// one-sided, so forced surplus with inelastic demand is either
// absorbable by redispatch (all LMPs stay at marginal gen costs ≥ 0)
// or hard-infeasible.  A negative dual needs the forced MW to relieve
// a PRICED constraint — so per the fixture-family doctrine we combine
// the reserve-forced must-run with the M2 congestion ring: the forced
// unit sits at bus 4, whose injections LOAD the binding 4→3 corridor
// (PTDF 1/2 vs the cheap gen's 1/4), reproducing the classic
// "reserve keeps a unit on; the surplus floods a congested pocket;
// the pocket price goes negative" pattern with ALL-POSITIVE costs.
//
//        g1 (c=10, 1000 MW)          d2 = 200 MW
//   bus1 ─────── l12 (1000) ─────── bus2
//    │                                │
//   l14 (1000)                      l32 (1000)
//    │                                │
//   bus4 ─────── l43 (30 MW) ─────── bus3
//    g4 (c=30, 200 MW,                 g3 (c=100, 1000 MW)
//        down-reserve forced ≥ 50)
//
// Lossless DC with the reserve active: p4 = 50 (pinned by dr), and the
// l43 limit binds with g1 = 85, g3 = 65 both interior.  The marginal
// pair (g1, g3) is the same as the pure-M2 fixture, so the duals are
// unchanged:
//   pi1 = 10,  pi2 = 55,  pi3 = 100,  pi4 = (3c1 − c3)/2 = −35.
// g4 runs at a loss (c4 = 30 > pi4 = −35) purely because the reserve
// forces it — the M3 mechanism.
//
// Line l14 (pi1 + pi4 = −25 < 0) is again the arbitrage corridor; the
// defect tests below pin that the reserve-generated negative LMP feeds
// the same loss-arbitrage channels demonstrated under M2 in
// test_line_losses_negative_lmp_kvl.cpp:
//   (A) phantom circulation — `piecewise_direct` (worst mode);
//   (D) v-inflation         — `tangent_signed_flow` at the
//       plexos2gtopt production default ε = 0.1.

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{
namespace negative_lmp_reserve_test
{

constexpr double kV = 100.0;
constexpr double kR = 0.5;  ///< c = R/V² = 5e-5 per line
constexpr double kX = 10.0;
constexpr double kTmax = 1000.0;
constexpr double kTmaxLimited = 30.0;  ///< l43 binding limit
constexpr double kCLoss = kR / (kV * kV);  ///< 5e-5
constexpr double kDrReq = 50.0;  ///< hard down-reserve requirement

struct ReserveRingFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit ReserveRingFixture(std::string_view mode_name,
                              double loss_cost_eps = 0.0,
                              int loss_segments = 4)
      : system {
            .name = "NegLmpReserveRing",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                    {.uid = Uid {3}, .name = "b3",},
                    {.uid = Uid {4}, .name = "b4",},
                },
            .demand_array =
                {
                    {.uid = Uid {1},
                     .name = "d2",
                     .bus = Uid {2},
                     .capacity = 200.0,},
                },
            .generator_array =
                {
                    {.uid = Uid {1},
                     .name = "g1",
                     .bus = Uid {1},
                     .gcost = 10.0,
                     .capacity = 1000.0,},
                    {.uid = Uid {3},
                     .name = "g3",
                     .bus = Uid {3},
                     .gcost = 100.0,
                     .capacity = 1000.0,},
                    {.uid = Uid {4},
                     .name = "g4_mustrun",
                     .bus = Uid {4},
                     .gcost = 30.0,
                     .capacity = 200.0,},
                },
            .line_array =
                {
                    make_line(
                        Uid {1}, "l12", Uid {1}, Uid {2}, kTmax, mode_name,
                        loss_segments),
                    make_line(
                        Uid {2}, "l32", Uid {3}, Uid {2}, kTmax, mode_name,
                        loss_segments),
                    make_line(
                        Uid {3}, "l43", Uid {4}, Uid {3}, kTmaxLimited,
                        mode_name, loss_segments),
                    make_line(
                        Uid {4}, "l14", Uid {1}, Uid {4}, kTmax, mode_name,
                        loss_segments),
                },
            .reserve_zone_array =
                {
                    // No drcost and no model_options.reserve_shortage_cost
                    // → HARD requirement row `Σ pf·dr ≥ drreq`.
                    {.uid = Uid {1}, .name = "rz_down", .drreq = kDrReq,},
                },
            .reserve_provision_array =
                {
                    {.uid = Uid {1},
                     .name = "rp_g4",
                     .generator = Uid {4},
                     .reserve_zones = {SingleId {Uid {1}}},
                     .drmax = 200.0,
                     .dr_provision_factor = 1.0,},
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1}, .first_block = 0, .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options(loss_cost_eps))
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_matrix_opts())
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static Line make_line(Uid uid,
                        std::string_view name,
                        Uid bus_a,
                        Uid bus_b,
                        double tmax,
                        std::string_view mode_name,
                        int loss_segments)
  {
    return {
        .uid = uid,
        .name = std::string(name),
        .bus_a = bus_a,
        .bus_b = bus_b,
        .voltage = kV,
        .resistance = kR,
        .reactance = kX,
        .line_losses_mode = OptName {std::string(mode_name)},
        .loss_segments = loss_segments,
        .tmax_ba = tmax,
        .tmax_ab = tmax,
        .capacity = tmax,
    };
  }

  PlanningOptionsLP make_options(double loss_cost_eps)
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = true;
    opts.model_options.kirchhoff_mode = std::string("cycle_basis");
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    // Deliberately NOT setting model_options.reserve_shortage_cost:
    // the zone requirement must stay a hard row (see file header).
    if (loss_cost_eps > 0.0) {
      opts.model_options.loss_cost_eps = loss_cost_eps;
    }
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_matrix_opts()
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    return bo;
  }
};

[[nodiscard]] auto find_col(const LinearInterface& li, std::string_view substr)
    -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

[[nodiscard]] auto find_row(const LinearInterface& li, std::string_view substr)
    -> RowIndex
{
  RowIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

[[nodiscard]] auto sum_cols(const LinearInterface& li,
                            std::span<const double> sol,
                            std::string_view substr) -> double
{
  double total = 0.0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      total += sol[static_cast<std::size_t>(value_of(idx))];
    }
  }
  return total;
}

// ── Control: reserves create the negative LMP ─────────────────────────

TEST_CASE(
    "negative-LMP M3 control: down-reserve must-run + congestion flips "
    "the bus-4 dual negative (mode none)")  // NOLINT
{
  ReserveRingFixture fix("none");
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const auto duals = li.get_row_dual_raw();
  REQUIRE(duals.size() == static_cast<std::size_t>(li.get_numrows()));

  // The reserve is the forcing device: g4 would never run on merit
  // (c4 = 30 > c1 = 10 AND its injections load the binding corridor).
  // The hard down-reserve pins p4 = drreq = 50 exactly.
  const auto g4_col = find_col(li, "generator_generation_4_");
  const double p4 = sol[static_cast<std::size_t>(value_of(g4_col))];
  CHECK(p4 == doctest::Approx(kDrReq).epsilon(0.01));

  // Redispatch around the forced unit: g1 = 85, g3 = 65 (lossless DC),
  // with l43 still binding at 30 MW.
  const auto g1_col = find_col(li, "generator_generation_1_");
  const auto g3_col = find_col(li, "generator_generation_3_");
  const auto f43_col = find_col(li, "line_flowp_3_");
  CHECK(sol[static_cast<std::size_t>(value_of(g1_col))]
        == doctest::Approx(85.0).epsilon(0.02));
  CHECK(sol[static_cast<std::size_t>(value_of(g3_col))]
        == doctest::Approx(65.0).epsilon(0.02));
  CHECK(std::abs(sol[static_cast<std::size_t>(value_of(f43_col))])
        == doctest::Approx(30.0).epsilon(0.01));

  // Dual pattern — same marginal pair (g1, g3) as the pure-M2 ring,
  // so the ratios are unchanged.  Sign-free via bus-1 normalization.
  const double pi1 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_1_")))];
  const double pi3 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_3_")))];
  const double pi4 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_4_")))];
  CAPTURE(pi1);
  CAPTURE(pi3);
  CAPTURE(pi4);
  REQUIRE(std::abs(pi1) > 0.0);

  CHECK(pi3 / pi1 == doctest::Approx(10.0).epsilon(0.02));
  CHECK(pi4 / pi1 == doctest::Approx(-3.5).epsilon(0.02));
  // The M3 headline: the reserve-forced must-run bus prices NEGATIVE
  // with all-positive offer costs.
  CHECK(pi4 * pi1 < 0.0);
}

// ── Channel A defect under M3 ─────────────────────────────────────────

TEST_CASE(
    "negative-LMP M3 piecewise_direct: reserve-driven corridor hosts "
    "phantom circulation (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (A), mode `piecewise_direct`,
  // mechanism M3.
  //
  // Identical channel to the M2 file, but here the negative pair-sum
  // pi1 + pi4 = −25 is manufactured by the RESERVE (not by dispatch
  // economics alone): remove the reserve and bus 4 prices positive
  // (l43 unbinds at g1 = 160, g3 = 40 only when the must-run is off —
  // with g4 forced, its PTDF-1/2 injections keep the corridor tight).
  // No link row, no aggregator → both segment ladders of l14 fill
  // simultaneously and dump λ_k-weighted loss at the −35 $/MWh bus.
  // Activation: pi_a + pi_b < 0 (ε inert for piecewise_direct).
  ReserveRingFixture fix("piecewise_direct");
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double sum_p = sum_cols(li, sol, "line_flowp_seg_4_");
  const double sum_n = sum_cols(li, sol, "line_flown_seg_4_");
  CAPTURE(sum_p);
  CAPTURE(sum_n);

  // Physical |net| on l14 is ~20 MW; both directions simultaneously
  // carrying > 50 MW is the defect.
  CHECK(std::min(sum_p, sum_n) > 50.0);
  CHECK(sum_p + sum_n > 5.0 * std::abs(sum_p - sum_n));

  // The reserve stays binding while the arbitrage runs (the defect
  // does not "solve" the reserve): p4 ≥ drreq still holds.
  const auto g4_col = find_col(li, "generator_generation_4_");
  CHECK(sol[static_cast<std::size_t>(value_of(g4_col))] >= kDrReq - 0.01);
}

// ── Channel D defect under M3 ─────────────────────────────────────────

TEST_CASE(
    "negative-LMP M3 tangent_signed_flow eps=0.1 (production default): "
    "v-inflation sink under a reserve-driven corridor (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (D), mechanism M3.
  //
  // Same activation as the M2 file: with ε = 0.1 on v and ℓ the sink
  // is closed only for pair-sums above ≈ −4.2 $/MWh; the reserve-driven
  // corridor sits at −25 $/MWh, so v inflates to fmax and ℓ rides the
  // chord row to ≈ c·env² = 50 MW on a ~20 MW line.  This pins that
  // M3-generated negative prices are just as exploitable as M2 ones —
  // the channel only sees the dual sign, not its cause.
  ReserveRingFixture fix("tangent_signed_flow", /*loss_cost_eps=*/0.1);
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double v14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flow_abs_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(v14);
  CAPTURE(loss14);

  CHECK(v14 > std::abs(f14) + 100.0);
  const double physical = kCLoss * f14 * f14;
  CAPTURE(physical);
  CHECK(loss14 > 5.0);
  CHECK(loss14 > 100.0 * physical);
}

}  // namespace negative_lmp_reserve_test
}  // namespace
