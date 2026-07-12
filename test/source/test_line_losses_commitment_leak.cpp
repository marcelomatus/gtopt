// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_commitment_leak.cpp — channel (E): commitment leak.
//
// `LineCommitmentLP::add_to_lp` gates ONLY the aggregate flow columns
// (line_commitment_lp.cpp:255-296: `f − F·u ≤ 0` on fp/fn/fs).  The
// loss apparatus — per-direction loss columns, the |f|-aux `v`
// column, PWL segment columns — is never multiplied by `u`.  Whether
// an OFFLINE line (u = 0) can still book losses therefore depends
// entirely on how each loss mode ties its loss column to the (now
// zero) flow:
//
//   * linear                — no loss column at all (factors baked
//                             into the fp/fn bus stamps).  IMMUNE.
//   * bidirectional/uniform — link row `f − Σseg = 0` (equality) +
//                             loss row `ℓ − Σ loss_k·seg_k = 0`
//                             (equality): u=0 ⇒ f=0 ⇒ seg=0 ⇒ ℓ=0.
//                             IMMUNE (transitively gated).
//   * piecewise + midpoint  — the loss row is `≥` only
//                             (line_losses.cpp:1420-1434); ℓ's only
//                             upper bound is its column cap c·tmax².
//                             u=0 leaves a FULL-SIZE free sink at the
//                             receiver bus.  EXPOSED.
//   * piecewise + tangent   — legacy shared-loss path: K tangent
//                             LOWER bounds only (line_losses.cpp:
//                             665-684); shared ℓ capped at c·env².
//                             EXPOSED.
//   * tangent_signed_flow   — v is bounded by `v ≥ ±f` only; at
//                             f = 0 every tangent row is slack, v
//                             inflates to the envelope and drags ℓ up
//                             the chord to c·env² (line_losses.cpp:
//                             1984-2054).  EXPOSED at ε = 0; FULLY
//                             closed by ε > ε* ≈ |πa+πb|/2·c·env/
//                             (1+c·env) because the chord residual
//                             vanishes at f = 0 (unlike the ONLINE
//                             channel D, where it persists).
//   * tangent_signed_flow
//     + SOS2                — λ-adjacency + flow row pin the ladder
//                             at the b=0 breakpoint when f = 0, so
//                             chord ⇒ ℓ = 0.  IMMUNE (MIP).
//   * piecewise_direct      — WORSE than a leak: `resolve_flow_cols`
//                             (line_commitment_lp.cpp:53-73) looks
//                             only at flowp/flown/flows aggregates,
//                             which piecewise_direct never populates
//                             (line_losses.cpp:2378-2379, empty
//                             fp_col/fn_col/flow_col).  The block is
//                             skipped at line_commitment_lp.cpp:131-
//                             133: NO status column, NO gating rows,
//                             no warning.  The "offline" line keeps
//                             carrying flow.  Contrast: the KVL
//                             emitters DO consult the segment maps
//                             (kirchhoff_node_angle.cpp:92-94), so
//                             segment-only lines are known to the
//                             rest of the code base.
//
// Fixture: the M2 4-bus ring of
// test_line_losses_negative_lmp_kvl.cpp (binding l43 limit flips
// π4 = −35 with all-positive costs; π1 + π4 = −25 on the bus1–bus4
// corridor), with two changes:
//
//   1. kirchhoff_mode = node_angle, so LineCommitmentLP emits the
//      per-line KVL big-M disjunction (line_commitment_lp.cpp:331-380)
//      and opening one line relaxes ONLY that line's angle row.  (In
//      cycle_basis the disjunctive form slacks EVERY basis cycle
//      through the switched line, so a parallel-line fixture could
//      silently lose the ring KVL depending on the spanning tree.)
//   2. Only the line under test carries a loss model; every other
//      line runs mode `none`, so the base dispatch is exactly the
//      lossless control: g1 = 160, g3 = 40, f43 = 30 (binding),
//      π = (10, 55, 100, −35).
//
// The line under test is either an extra parallel corridor line l14b
// (uid 5, bus1–bus4) forced open via LineCommitment.initial_status=0
// (+ relax=true so the fixture stays a pure LP), or — for the
// piecewise_direct skip defect — the corridor line l14 itself.
//
// Defect-documenting tests assert the defect IS present (CI stays
// green); when one starts failing, the defect was fixed and the test
// should be inverted into a regression guard.

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_commitment.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{
namespace commitment_leak_test
{

constexpr double kV = 100.0;
constexpr double kR = 0.5;  ///< c = R/V² = 5e-5 per line
constexpr double kX = 10.0;  ///< x_tau = X/V² = 1e-3 (node_angle)
constexpr double kTmax = 1000.0;
constexpr double kTmaxLimited = 30.0;  ///< l43 binding limit
constexpr double kCLoss = kR / (kV * kV);  ///< 5e-5
constexpr double kSinkCap = kCLoss * kTmax * kTmax;  ///< c·env² = 50 MW

/// Loss-model + topology knobs for one fixture build.
struct LeakKnobs
{
  std::string_view mode = "none";  ///< losses mode of the TARGET line
  std::string_view layout = {};  ///< loss_pwl_layout of the target
  double loss_cost_eps = 0.0;  ///< global model_options.loss_cost_eps
  int loss_secant_segments = 1;
  bool loss_use_sos2 = false;
  /// true  → add parallel line l14b (uid 5) as the committed target;
  /// false → the corridor line l14 (uid 4) is the committed target.
  bool parallel_line = true;
};

[[nodiscard]] constexpr Uid target_uid(const LeakKnobs& knobs)
{
  return knobs.parallel_line ? Uid {5} : Uid {4};
}

struct RingFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit RingFixture(const LeakKnobs& knobs)
      : system {
            .name = "CommitmentLeakRing",
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
                },
            .line_array = make_lines(knobs),
            .line_commitment_array =
                {
                    {.uid = Uid {1},
                     .name = "target_ots",
                     .line = target_uid(knobs),
                     // Pins u = 0 on the (single) first block; with
                     // relax = true the fixture stays a pure LP that
                     // CLP can solve (except the SOS2 variant).
                     .initial_status = 0.0,
                     .relax = true,},
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
                  // LineCommitmentLP silently skips non-chronological
                  // stages (line_commitment_lp.cpp:90) and the Stage
                  // default is FALSE — without this the whole test
                  // would vacuously "pass" with no gate at all.
                  .chronological = true,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options(knobs))
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_matrix_opts(knobs))
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static std::vector<Line> make_lines(const LeakKnobs& knobs)
  {
    const auto target = target_uid(knobs);
    std::vector<Line> lines;
    lines.push_back(
        make_line(Uid {1}, "l12", Uid {1}, Uid {2}, kTmax, knobs, target));
    lines.push_back(
        make_line(Uid {2}, "l32", Uid {3}, Uid {2}, kTmax, knobs, target));
    lines.push_back(make_line(
        Uid {3}, "l43", Uid {4}, Uid {3}, kTmaxLimited, knobs, target));
    lines.push_back(
        make_line(Uid {4}, "l14", Uid {1}, Uid {4}, kTmax, knobs, target));
    if (knobs.parallel_line) {
      lines.push_back(
          make_line(Uid {5}, "l14b", Uid {1}, Uid {4}, kTmax, knobs, target));
    }
    return lines;
  }

  static Line make_line(Uid uid,
                        std::string_view name,
                        Uid bus_a,
                        Uid bus_b,
                        double tmax,
                        const LeakKnobs& knobs,
                        Uid target)
  {
    const bool is_target = uid == target;
    Line ln = {
        .uid = uid,
        .name = std::string(name),
        .bus_a = bus_a,
        .bus_b = bus_b,
        .voltage = kV,
        .resistance = kR,
        .reactance = kX,
        .line_losses_mode =
            OptName {std::string(is_target ? knobs.mode : "none")},
        .loss_segments = 4,
        .tmax_ba = tmax,
        .tmax_ab = tmax,
        .capacity = tmax,
    };
    if (is_target && !knobs.layout.empty()) {
      ln.loss_pwl_layout = std::string(knobs.layout);
    }
    if (is_target && knobs.loss_secant_segments > 1) {
      ln.loss_secant_segments = knobs.loss_secant_segments;
      ln.loss_use_sos2 = knobs.loss_use_sos2;
    }
    return ln;
  }

  PlanningOptionsLP make_options(const LeakKnobs& knobs)
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = true;
    opts.model_options.kirchhoff_mode = std::string("node_angle");
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    if (knobs.loss_cost_eps > 0.0) {
      opts.model_options.loss_cost_eps = knobs.loss_cost_eps;
    }
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_matrix_opts(const LeakKnobs& knobs)
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    if (knobs.loss_use_sos2) {
      auto& reg = SolverRegistry::instance();
      reg.load_all_plugins();
      if (reg.has_solver("cplex")) {
        bo.solver_name = "cplex";
      }
    }
    return bo;
  }
};

/// SOS2 needs a MIP backend with native SOS2 (CPLEX in this env);
/// CBC/OSI throw at LP-build time.  Same guard as
/// test_line_losses_sos2.cpp::sos2_available.
[[nodiscard]] bool sos2_available()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  return reg.has_solver("cplex");
}

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

[[nodiscard]] auto count_cols(const LinearInterface& li,
                              std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
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

/// Sum of primal values over every column whose name contains `substr`.
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

[[nodiscard]] auto col_val(const LinearInterface& li,
                           std::span<const double> sol,
                           std::string_view substr) -> double
{
  return sol[static_cast<std::size_t>(value_of(find_col(li, substr)))];
}

struct RingDuals
{
  double pi1;
  double pi2;
  double pi3;
  double pi4;
};

[[nodiscard]] RingDuals read_duals(LinearInterface& li)
{
  const auto duals = li.get_row_dual_raw();
  REQUIRE(duals.size() == static_cast<std::size_t>(li.get_numrows()));
  const auto at = [&](std::string_view s)
  { return duals[static_cast<std::size_t>(value_of(find_row(li, s)))]; };
  return {
      .pi1 = at("bus_balance_1_"),
      .pi2 = at("bus_balance_2_"),
      .pi3 = at("bus_balance_3_"),
      .pi4 = at("bus_balance_4_"),
  };
}

// ── Guards: the gate works where the aggregators exist ───────────────

TEST_CASE(
    "commitment leak E control: offline bidirectional parallel line is "
    "fully gated under the M2 negative corridor (GUARD)")  // NOLINT
{
  // REGRESSION GUARD — bidirectional/uniform is transitively gated:
  // the gating rows pin fp = fn = 0, the link EQUALITY `f − Σseg = 0`
  // (line_losses.cpp:1406-1415) zeroes the non-negative segments, and
  // the uniform loss EQUALITY `ℓ − Σ loss_k·seg_k = 0` (:1431-1434)
  // zeroes both loss columns.  This holds even under the full −25
  // $/MWh corridor pressure (the same pressure that blows up the
  // midpoint / tangent / tangent_signed_flow variants below).
  //
  // Doubles as the node_angle control: the dual pattern of the M2
  // ring must survive the parallel offline line, proving the KVL
  // big-M disjunction (line_commitment_lp.cpp:331-380) decoupled
  // l14b without touching the ring physics.
  RingFixture fix(LeakKnobs {.mode = "bidirectional"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // The gate was actually emitted (contrast: piecewise_direct below).
  CHECK(count_cols(li, "status") == 1);

  const auto sol = li.get_col_sol_raw();

  // Base ring physics intact: lossless dispatch + binding l43.
  CHECK(col_val(li, sol, "generator_generation_1_")
        == doctest::Approx(160.0).epsilon(0.01));
  CHECK(col_val(li, sol, "generator_generation_3_")
        == doctest::Approx(40.0).epsilon(0.01));
  CHECK(std::abs(col_val(li, sol, "line_flowp_3_"))
        == doctest::Approx(30.0).epsilon(0.01));

  // Commitment purity on the offline line: u = 0 ⇒ f = seg = ℓ = 0.
  CHECK(col_val(li, sol, "line_flowp_5_") <= 1e-6);
  CHECK(col_val(li, sol, "line_flown_5_") <= 1e-6);
  CHECK(col_val(li, sol, "line_lossp_5_") <= 1e-6);
  CHECK(col_val(li, sol, "line_lossn_5_") <= 1e-6);
  CHECK(sum_cols(li, sol, "line_flowp_seg_5_") <= 1e-6);
  CHECK(sum_cols(li, sol, "line_flown_seg_5_") <= 1e-6);

  // Dual pattern preserved (ratios; raw sign is backend-dependent):
  // π = (10, 55, 100, −35) ⇒ π2/π1 = 5.5, π3/π1 = 10, π4/π1 = −3.5.
  const auto pi = read_duals(li);
  CAPTURE(pi.pi1);
  CAPTURE(pi.pi4);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi2 / pi.pi1 == doctest::Approx(5.5).epsilon(0.02));
  CHECK(pi.pi3 / pi.pi1 == doctest::Approx(10.0).epsilon(0.02));
  CHECK(pi.pi4 / pi.pi1 == doctest::Approx(-3.5).epsilon(0.02));
}

TEST_CASE(
    "commitment leak E: gating the corridor line itself truly opens the "
    "ring for aggregator modes (GUARD)")  // NOLINT
{
  // REGRESSION GUARD — when the committed line IS the corridor (l14,
  // bidirectional mode), u = 0 must physically open it: the ring goes
  // radial, l43 stops binding, and the negative dual disappears
  // (π4 = π1 = c1, g1 serves everything).  This is the positive
  // control for the piecewise_direct skip defect below: same
  // commitment input, opposite outcome.
  RingFixture fix(LeakKnobs {.mode = "bidirectional", .parallel_line = false});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  CHECK(count_cols(li, "status") == 1);

  const auto sol = li.get_col_sol_raw();
  CHECK(col_val(li, sol, "line_flowp_4_") <= 1e-6);
  CHECK(col_val(li, sol, "line_flown_4_") <= 1e-6);
  CHECK(col_val(li, sol, "line_lossp_4_") <= 1e-6);
  CHECK(col_val(li, sol, "line_lossn_4_") <= 1e-6);

  // Corridor open ⇒ nothing pushes flow around to l43; g1 takes all.
  CHECK(std::abs(col_val(li, sol, "line_flowp_3_")) <= 1e-6);
  CHECK(col_val(li, sol, "generator_generation_1_")
        == doctest::Approx(200.0).epsilon(0.01));

  // Negative LMP gone: π4 = π1 = c1.
  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE(
    "commitment leak E linear mode: no loss apparatus exists to leak "
    "(GUARD)")  // NOLINT
{
  // REGRESSION GUARD — structural immunity: `add_linear`
  // (line_losses.cpp:868-953) creates NO loss column; the loss factor
  // is baked into the fp/fn bus-balance coefficients
  // (apply_linear_allocation, :370-391).  Gating fp = fn = 0 removes
  // the loss with the flow, by construction.
  RingFixture fix(LeakKnobs {.mode = "linear"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  CHECK(count_cols(li, "status") == 1);

  const auto sol = li.get_col_sol_raw();
  CHECK(col_val(li, sol, "line_flowp_5_") <= 1e-6);
  CHECK(col_val(li, sol, "line_flown_5_") <= 1e-6);
  // The structural claim itself: no loss / v / segment columns exist
  // on the target line at all.
  CHECK(count_cols(li, "line_lossp_5_") == 0);
  CHECK(count_cols(li, "line_lossn_5_") == 0);
  CHECK(count_cols(li, "line_flowp_seg_5_") == 0);

  // Pressure was present (the corridor dual is still negative).
  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 == doctest::Approx(-3.5).epsilon(0.02));
}

// ── Channel E defects: the loss apparatus survives u = 0 ─────────────

TEST_CASE(
    "commitment leak E piecewise+midpoint: offline line books the full "
    "c·tmax² loss sink (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (E) via the midpoint free sink
  // (channel C at f = 0).
  //
  // The midpoint loss row is `s·ℓ − Σ s·loss_k·seg_k ≥ −s·offset`
  // (line_losses.cpp:1420-1434) — a LOWER bound only.  The gating
  // rows zero fp/fn, the link equality zeroes the segments, and ℓ is
  // then a free variable in [0, c·tmax²] whose only bus-balance role
  // is CONSUMING power at the receiver (apply_loss_allocation,
  // :395-413, default `receiver`).  With π4 = −35 at the positive
  // direction's receiver, the LP pins ℓ_p at its 50 MW column cap on
  // a line that is FORCED OPEN by its own commitment binary.
  // Activation: π_recv < −ε (ε = 0 here).
  // Fix path: gate the loss columns by u (ℓ ≤ c·tmax²·u), or use
  // uniform layout (equality row) / tangent_signed_flow + SOS2.
  RingFixture fix(LeakKnobs {.mode = "piecewise", .layout = "midpoint"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // The gate exists and works on the FLOW columns…
  CHECK(count_cols(li, "status") == 1);
  const auto sol = li.get_col_sol_raw();
  CHECK(col_val(li, sol, "line_flowp_5_") <= 1e-6);
  CHECK(col_val(li, sol, "line_flown_5_") <= 1e-6);
  CHECK(sum_cols(li, sol, "line_flowp_seg_5_") <= 1e-6);
  CHECK(sum_cols(li, sol, "line_flown_seg_5_") <= 1e-6);

  // …but the loss column of the offline line hosts a full-size sink.
  const double lossp5 = col_val(li, sol, "line_lossp_5_");
  const double lossn5 = col_val(li, sol, "line_lossn_5_");
  CAPTURE(lossp5);
  CAPTURE(lossn5);
  // Arbitrage volume: ~50 MWh of fictitious loss per hour on a line
  // carrying ZERO flow (physical loss = 0).
  CHECK(lossp5 > 10.0);
  CHECK(lossp5 <= kSinkCap + 0.01);
  // Channel signature: only the negative-π receiver (bus4, positive
  // direction) hosts the sink; ℓ_n's receiver is bus1 (π = +10).
  CHECK(lossn5 <= 1e-6);

  // The pressure that funds the sink is still on.
  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 < 0.0);
}

TEST_CASE(
    "commitment leak E piecewise+tangent layout: offline line books "
    "loss at the shared column cap (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (E) via the legacy shared-loss
  // path (`add_piecewise_shared`, tangent layout only post-2026-05-31).
  //
  // The K tangent rows are LOWER bounds `ℓ ≥ 2·c·t_k·(fp+fn) − c·t_k²`
  // (line_losses.cpp:665-684); at fp = fn = 0 (gated) they are all
  // slack, leaving the shared ℓ free in [0, c·env²]
  // (loss_ub, :1179-1191).  Receiver allocation dumps it at bus_b =
  // bus4 (π = −35) ⇒ the LP books the cap on the open line.
  RingFixture fix(LeakKnobs {.mode = "piecewise", .layout = "tangent"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  CHECK(count_cols(li, "status") == 1);
  const auto sol = li.get_col_sol_raw();
  CHECK(col_val(li, sol, "line_flowp_5_") <= 1e-6);
  CHECK(col_val(li, sol, "line_flown_5_") <= 1e-6);

  const double lossp5 = col_val(li, sol, "line_lossp_5_");
  CAPTURE(lossp5);
  CHECK(lossp5 > 10.0);
  CHECK(lossp5 <= kSinkCap + 0.01);

  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 < 0.0);
}

TEST_CASE(
    "commitment leak E tangent_signed_flow eps=0: offline line "
    "v-inflates to the envelope and books c·env² (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (E) via v-inflation (channel D
  // at f = 0, made PERMANENT by the commitment: the gate pins f = 0,
  // which is exactly the point where every tangent row is slack).
  //
  // The gate stamps only the signed flow column
  // (line_commitment_lp.cpp:265-296); v is tied to f by `Σv ≥ ±f`
  // LOWER bounds only (line_losses.cpp:2004-2026), so at f = 0 the v
  // ladder inflates to the envelope and the chord row `ℓ ≤ Σ slope·v`
  // (:2036-2054) lets ℓ ride to c·env² = 50 MW.  Split allocation
  // dumps 25 MW at each of bus1 (π = 10) and bus4 (π = −35): profit
  // 12.5 $/MWh per booked MW.  A breaker-open line is a 50 MW energy
  // sink.
  // Activation: π_a + π_b < −2ε_v/(c·env) − 2ε_ℓ (ε = 0 here).
  RingFixture fix(LeakKnobs {.mode = "tangent_signed_flow"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  CHECK(count_cols(li, "status") == 1);
  const auto sol = li.get_col_sol_raw();

  // The gate DID pin the flow…
  const double f5 = col_val(li, sol, "line_flows_5_");
  CHECK(std::abs(f5) <= 1e-6);

  // …but v and ℓ are ungated: idle-line purity (|f| = 0 ⇒ ℓ = 0)
  // measurably breaks.
  const double v5 = col_val(li, sol, "line_flow_abs_5_");
  const double loss5 = col_val(li, sol, "line_lossp_5_");
  CAPTURE(v5);
  CAPTURE(loss5);
  CHECK(v5 > 500.0);  // inflates toward the 1000 MW envelope
  CHECK(loss5 > 10.0);  // books toward the 50 MW cap
  CHECK(loss5 <= kSinkCap + 0.01);

  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 < 0.0);
}

TEST_CASE(
    "commitment leak E tangent_signed_flow eps=1.0 (loss-priced): the "
    "offline sink stays OPEN below the pair-sum threshold (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (E), loss-priced ε semantics.
  //
  // Since the 2026-07 re-point ε lands on the LOSS column only (v
  // carries the internal 1e-6 pin), so ONLINE and OFFLINE share the
  // single closure condition ε > |π1+π4|/2 = 12.5.  At ε = 1.0 the
  // ℓ-dump nets 11.5 $/MWh: the breaker-open line still v-inflates
  // to the envelope and books the full c·env² sink.
  //
  // (Pre-re-point, ε = 1.0 > ε* ≈ 0.595 closed the offline sink via
  // the v flow-toll — partial protection paid for with an ε $/MWh
  // toll on every legitimate flow system-wide.)
  RingFixture fix(
      LeakKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 1.0});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  CHECK(std::abs(col_val(li, sol, "line_flows_5_")) <= 1e-6);
  // DEFECT: the offline line's v and ℓ host the full-size sink.
  CHECK(col_val(li, sol, "line_flow_abs_5_") > 100.0);
  CHECK(col_val(li, sol, "line_lossp_5_") > 10.0);

  // Pressure present — the exposure is dual-driven.
  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 < 0.0);
}

TEST_CASE(
    "commitment leak E tangent_signed_flow eps=15 > |pair-sum|/2: the "
    "offline sink is fully closed (EPS-GUARD)")  // NOLINT
{
  // REGRESSION GUARD — loss-priced ε-closure of channel (E).
  //
  // ε = 15 > |π1+π4|/2 = 12.5: dumping ℓ on the offline line has
  // strictly positive net cost, so ℓ collapses to 0 and the internal
  // pin drops v to |f| = 0.  Same sizing rule as the online channel D
  // (test_line_losses_negative_lmp_kvl.cpp, eps=20 case) — since the
  // re-point the ε palliative closes ONLINE and OFFLINE alike, at
  // loss-scaled incidence.
  RingFixture fix(
      LeakKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 15.0});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  CHECK(std::abs(col_val(li, sol, "line_flows_5_")) <= 1e-6);
  CHECK(col_val(li, sol, "line_flow_abs_5_") <= 1e-4);
  CHECK(col_val(li, sol, "line_lossp_5_") <= 1e-4);

  // Pressure still present — the guard is doing real work.
  const auto pi = read_duals(li);
  REQUIRE(std::abs(pi.pi1) > 0.0);
  CHECK(pi.pi4 / pi.pi1 < 0.0);
}

TEST_CASE(
    "commitment leak E tangent_signed_flow + SOS2: lambda adjacency "
    "pins the offline line at zero loss (GUARD, needs CPLEX)")  // NOLINT
{
  // REGRESSION GUARD — the exact (MIP) fix also closes channel (E).
  //
  // The λ-form ladder (line_losses.cpp:1850-1942) has no v columns:
  // the flow row `Σ b_l·λ_l = f` with f gated to 0 plus SOS2
  // adjacency forces all λ mass onto the b = 0 breakpoint, so the
  // chord row gives ℓ ≤ Σ c·b_l²·λ_l = 0.  No ε needed.
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 guard — no SOS2-capable backend loaded");
    return;
  }

  constexpr int L = 4;
  RingFixture fix(LeakKnobs {.mode = "tangent_signed_flow",
                             .loss_secant_segments = L,
                             .loss_use_sos2 = true});
  auto& li = fix.lp();
  REQUIRE(li.sos2_set_count() == 1);  // only the target line is SOS2

  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto sol = li.get_col_sol_raw();
  CHECK(std::abs(col_val(li, sol, "line_flows_5_")) <= 1e-6);
  CHECK(col_val(li, sol, "line_lossp_5_") <= 1e-4);
}

// ── piecewise_direct: the gate is silently skipped entirely ──────────

TEST_CASE(
    "commitment leak E piecewise_direct: LineCommitment is silently "
    "ignored — no status column, the open line keeps flowing "
    "(DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (E), worst variant.
  //
  // `resolve_flow_cols` (line_commitment_lp.cpp:53-73) checks only
  // the flowp/flown/flows aggregator maps; `add_piecewise_direct`
  // populates NONE of them (line_losses.cpp:2378-2379 — only
  // seg_p_cols/seg_n_cols).  The per-block loop then hits the
  // "LineLP elided the block" skip (:131-133) — a false-negative:
  // the line is NOT elided, it has 2K live segment columns.  Result:
  // no status column, no gating rows, no KVL disjunction, no
  // warning.  The operator's `initial_status = 0` (breaker open) is
  // silently discarded and the line keeps carrying flow — compare
  // the bidirectional positive control above, where the same input
  // opens the ring.
  //
  // Contrast inside the code base: the KVL emitters DO consult the
  // segment maps to detect live piecewise_direct blocks
  // (kirchhoff_node_angle.cpp:92-94, kirchhoff_cycle_basis.cpp
  // cycle_intact), so the pattern for a fix already exists.
  RingFixture fix(
      LeakKnobs {.mode = "piecewise_direct", .parallel_line = false});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // The commitment emitted NOTHING for the target line.
  CHECK(count_cols(li, "status") == 0);

  // The "open" corridor still carries flow (and, with π1 + π4 = −25,
  // still phantom-circulates — channel A rides along unimpeded).
  const auto sol = li.get_col_sol_raw();
  const double sum_p = sum_cols(li, sol, "line_flowp_seg_4_");
  const double sum_n = sum_cols(li, sol, "line_flown_seg_4_");
  CAPTURE(sum_p);
  CAPTURE(sum_n);
  CHECK(sum_p > 10.0);
  CHECK(std::min(sum_p, sum_n) > 10.0);

  // Ring physics untouched: l43 still binding — the breaker-open
  // request had zero effect on the network.
  CHECK(std::abs(col_val(li, sol, "line_flowp_3_"))
        == doctest::Approx(30.0).epsilon(0.05));
}

}  // namespace commitment_leak_test
}  // namespace
