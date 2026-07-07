// SPDX-License-Identifier: BSD-3-Clause
//
// Mode-agnostic line-loss validation: solve a single-line LP at five
// flow points (0, tmax/4, tmax/2, 3·tmax/4, tmax) in BOTH directions
// (A→B via flowp, B→A via flown) for each loss model, then compare
// the LP-reported per-block loss to the analytic ``R·f²/V²``.  Pins
// the loss model end-to-end at the dispatch level, independent of
// any internal PWL geometry.
//
// Covers all loss modes:
//   * ``none``        — must report 0 loss everywhere
//   * ``linear``      — linear approximation, loss = λ·|f| with
//                       λ = R·fmax/V²; deliberately permissive
//                       tolerance (linear is a known under-estimate
//                       at low |f| and over-estimate at high |f|)
//   * ``piecewise`` — three PWL layouts (uniform / midpoint / tangent)
//                     swept over K = {1, 2, 4, 8, 16, 128} segments
//   * ``bidirectional`` — per-direction PWL, midpoint layout
//   * ``piecewise_direct`` — segments stamp directly on bus balance,
//                            uniform layout
//
// Regression coverage for the 2026-05-29 ``seg_uppb=seg_width on
// segs 1..K-1 even when decoupled_envelope=true`` fix
// (``source/line_losses.cpp::add_segments``): without that cap, the
// LP would stuff all flow into seg_1 (lowest slope) and report a
// loss far below the analytic value.  The flow points include the
// tmax case where the LP must use the steepest segment, exercising
// the overflow path the fix preserves.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

// Fixture-wide constants.  Picked so analytic R/V²·f² lands in the
// 10⁰..10⁴ MW range for the chosen flow points → comparison ratios
// stay numerically clean.
constexpr double kR = 0.01;  // p.u. on 100 MVA base
constexpr double kV = 100.0;  // kV → V² = 10,000
constexpr double kFmax = 1000.0;  // MW; both line cap AND envelope
constexpr double kGcost = 10.0;
constexpr double kScale = 1000.0;
constexpr double kLambda = kR * kFmax / (kV * kV);  // auto-linear λ
constexpr double kLossCoeff = kR / (kV * kV);  // 1e-6 MW per MW²

enum class Direction
{
  AtoB,  // gen on bus_a, demand on bus_b → flowp populated
  BtoA,  // gen on bus_b, demand on bus_a → flown populated
};

/// Solve a tiny single-line, single-block LP with the given loss
/// model and demand, and return the LP-reported loss in MW (per the
/// objective-balance identity ``gen = demand + loss``).
///
/// ``loss_segments == 0`` is forwarded as-is; loss modes that ignore
/// segmentation (``none`` / ``linear``) tolerate it, the PWL paths
/// would assert — caller is responsible for not pairing K=0 with a
/// PWL mode.
double solve(Direction dir,
             double demand,
             std::string_view mode,
             std::string_view pwl_layout,
             int loss_segments,
             int enforce_level)
{
  // Bus / generator / demand layout is swapped per direction so the
  // LP must route flow A→B (flowp) or B→A (flown) respectively.
  const auto gen_bus = (dir == Direction::AtoB) ? Uid {1} : Uid {2};
  const auto dem_bus = (dir == Direction::AtoB) ? Uid {2} : Uid {1};

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = gen_bus,
          .gcost = kGcost,
          .capacity = 2 * kFmax,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = dem_bus,
          .capacity = demand,
      },
  };

  Line l1 {
      .uid = Uid {1},
      .name = "l1",
      .bus_a = Uid {1},
      .bus_b = Uid {2},
      .voltage = kV,
      .resistance = kR,
      .line_losses_mode = OptName {std::string(mode)},
      .loss_segments = loss_segments,
      .loss_envelope =
          OptTRealFieldSched {
              kFmax,
          },
      .tmax_ba = kFmax,
      .tmax_ab = kFmax,
      // ``enforce_level`` is RETIRED (2026-06-10) — a no-op kept for
      // schema back-compat.  Sweeping it over {0, 2} here only proves
      // the loss model is invariant to the (ignored) flag; the cap
      // always binds at ``tmax`` regardless.  Flow points stay ≤ 0.95·
      // fmax so the cap never binds ahead of the loss model anyway.
      .enforce_level = OptInt {enforce_level},
      .capacity = kFmax,
  };
  if (!pwl_layout.empty()) {
    l1.loss_pwl_layout = OptName {std::string(pwl_layout)};
  }
  const Array<Line> line_array = {
      l1,
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

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = kScale;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "LossSweep",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);
  auto&& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  const double obj = lp.get_obj_value_raw();
  // gen × gcost / scale = obj → gen = obj·scale/gcost; loss = gen − demand
  return ((obj * kScale) / kGcost) - demand;
}

/// Test combo for the parametrised loop.
struct ModelSpec
{
  std::string_view mode;
  std::string_view pwl_layout;  // empty for non-PWL
};

/// One tolerance helper that handles every (mode, K) combo.
/// Returns the maximum relative error allowed between the LP's
/// reported loss and the analytic ``R·f²/V²`` value.
///
/// Bounds rationale (loosest first):
///   * ``none`` reports zero regardless of flow → analytic only matches
///     at f = 0; allow infinite tolerance so the dedicated f = 0 check
///     applies elsewhere.
///   * ``linear`` is a single-piece approximation of the quadratic with
///     λ = R·fmax/V²; over-estimates at f ≪ fmax by up to fmax/f → 4×
///     at f = fmax/4 in our fixture.  Cap at 5×.
///   * K = 0 falls back to ``none`` via the dispatcher → 5× cap, same
///     reason.
///   * K = 1 has one segment spanning the full envelope; same numeric
///     shape as ``linear`` (slope = w·R/V² = env·R/V²) → 5× cap.
///   * K ≥ 2 PWL: per-segment chord gap = (env/(2K))²·R/V², worst
///     relative error ≈ (env/(2K·f_min))² which falls as 1/K².  Add
///     a 10 % headroom so K = 2 at the breakpoint (theoretical
///     25 % error) still has comfortable slack vs CPLEX noise.
double tolerance_for(std::string_view mode, int K)
{
  if (mode == "none" || mode == "linear" || K <= 1) {
    return 5.0;
  }
  // Worst-case relative error for ``uniform`` PWL at flow = env/4
  // (mid of seg 1 when K = 2) is (env/(2K) / f)² = (2/K)² = 1.0 for
  // K = 2.  Use 2/K + 0.10 headroom so K = 2 just passes and
  // larger K stays meaningfully tight.
  return std::max((2.0 / static_cast<double>(K)) + 0.10, 0.05);
}

}  // namespace

TEST_CASE(
    "line_losses sweep - LP loss tracks R/V²·f² for all modes/K/directions")  // NOLINT
{
  const std::vector<int> k_values = {0, 1, 2, 4, 8, 16, 128};
  // Five flow points: 0, fmax/4, fmax/2, 3·fmax/4, ~fmax.  At
  // demand=fmax the LP would need fmax+loss capacity → leave a
  // small headroom (95 %) so the line cap doesn't bind ahead of
  // the loss model.
  const std::vector<double> demand_fracs = {0.0, 0.25, 0.5, 0.75, 0.95};

  const std::vector<ModelSpec> models = {
      {.mode = "none", .pwl_layout = ""},
      {.mode = "linear", .pwl_layout = ""},
      {.mode = "piecewise", .pwl_layout = "uniform"},
      {.mode = "piecewise", .pwl_layout = "midpoint"},
      {.mode = "piecewise", .pwl_layout = "tangent"},
      {.mode = "bidirectional", .pwl_layout = "midpoint"},
      {.mode = "piecewise_direct", .pwl_layout = "uniform"},
  };

  // ``enforce_level`` is RETIRED — a no-op.  Sweeping {0, 2} only
  // confirms the loss model is independent of the (ignored) flag; the
  // cap binds at ``tmax`` in both cases.  Flow points stay ≤ 0.95·fmax
  // so the cap never binds ahead of the loss model.
  const std::vector<int> el_values = {0, 2};

  for (const auto& spec : models) {
    for (const int K : k_values) {
      const double tol = tolerance_for(spec.mode, K);
      for (const int EL : el_values) {
        for (const auto dir : {Direction::AtoB, Direction::BtoA}) {
          for (const double frac : demand_fracs) {
            const double demand = frac * kFmax;
            const double loss_lp =
                solve(dir, demand, spec.mode, spec.pwl_layout, K, EL);
            // Realised flow includes the loss the LP paid to serve
            // demand.
            const double f = demand + loss_lp;
            const double loss_analytic = kLossCoeff * f * f;
            // Relative error with a small absolute floor so the
            // tolerance stays meaningful when both values approach 0.
            const double denom = std::max(loss_analytic, 1e-6);
            const double rel_err = std::abs(loss_lp - loss_analytic) / denom;

            CAPTURE(spec.mode);
            CAPTURE(spec.pwl_layout);
            CAPTURE(K);
            CAPTURE(EL);
            CAPTURE(dir == Direction::AtoB ? "AtoB" : "BtoA");
            CAPTURE(demand);
            CAPTURE(f);
            CAPTURE(loss_lp);
            CAPTURE(loss_analytic);
            CAPTURE(rel_err);
            CAPTURE(tol);
            // Invariant #1: zero flow → zero loss for every model.
            // Invariant #2: K = 0 (no PWL segments) degenerates to the
            // lossless formulation for PWL-required modes (the
            // dispatcher routes ``nseg <= 0`` for ``piecewise`` /
            // ``bidirectional`` / ``piecewise_direct`` through
            // ``add_none``).  ``none`` / ``linear`` ignore ``nseg`` so
            // the invariant doesn't apply there.
            const bool is_pwl_mode = spec.mode == "piecewise"
                || spec.mode == "bidirectional"
                || spec.mode == "piecewise_direct";
            if (demand == 0.0 || (K == 0 && is_pwl_mode)) {
              CHECK(loss_lp == doctest::Approx(0.0).epsilon(1e-6));
            }
            CHECK(rel_err <= tol);
          }
        }
      }
    }
  }
}

// ─── Integration: adaptive K rule × LP-tolerance ──────────────────────
//
// End-to-end test for the cube-root adaptive K allocation
// (``gtopt::line_losses::compute_adaptive_loss_segments``):
//
//   1. Build a heterogeneous 4-line mix whose ``L_max,i = R_i·fmax_i²``
//      spans ~3 orders of magnitude (mimicking the CEN PCP scale).
//   2. Call the rule with a target ``err_pct`` budget.
//   3. For each line, solve a single-line LP at multiple flow points
//      using the K_i the rule assigned.
//   4. Verify that the realised LP error per line stays inside the
//      rule's analytic bound ``L_max,i / (4 K_i²)`` (×slack).
//   5. Verify the SYSTEM-WIDE error budget ``Σ_i L_max,i / (4 K_i²)``
//      matches the rule's contract ``err_pct · Σ_i L_max,i`` (clamped
//      from below when ceiling binds on the heaviest lines).
//
// The K-sweep test above validates per-K LP correctness for a single
// line.  This test validates the END-TO-END flow: the rule picks K,
// the LP runs at that K, and the error stays within the rule's budget
// on a SYSTEM of lines.

namespace adaptive_integration_test_ns
{
namespace
{

/// Generalised single-line solve: same shape as ``solve()`` above, but
/// the line's R / V / fmax are parameters instead of fixture constants.
///
/// Capacity is sized generously so the gen-side bound never binds
/// ahead of the loss model.  Returns the LP-reported loss in MW via
/// the ``obj = gen·gcost/scale`` identity (``gen = demand + loss``).
double solve_line(double R,
                  double V,
                  double fmax,
                  Direction dir,
                  double demand,
                  std::string_view mode,
                  std::string_view pwl_layout,
                  int loss_segments,
                  int enforce_level)
{
  const auto gen_bus = (dir == Direction::AtoB) ? Uid {1} : Uid {2};
  const auto dem_bus = (dir == Direction::AtoB) ? Uid {2} : Uid {1};

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = gen_bus,
          .gcost = kGcost,
          .capacity = 4 * fmax,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = dem_bus,
          .capacity = demand,
      },
  };

  Line l1 {
      .uid = Uid {1},
      .name = "l1",
      .bus_a = Uid {1},
      .bus_b = Uid {2},
      .voltage = V,
      .resistance = R,
      .line_losses_mode = OptName {std::string(mode)},
      .loss_segments = loss_segments,
      .loss_envelope =
          OptTRealFieldSched {
              fmax,
          },
      .tmax_ba = fmax,
      .tmax_ab = fmax,
      .enforce_level = OptInt {enforce_level},
      .capacity = fmax,
  };
  if (!pwl_layout.empty()) {
    l1.loss_pwl_layout = OptName {std::string(pwl_layout)};
  }
  const Array<Line> line_array = {
      l1,
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

  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = kScale;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "AdaptiveIntegration",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);
  auto&& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  const double obj = lp.get_obj_value_raw();
  return ((obj * kScale) / kGcost) - demand;
}

/// 4-line CEN-PCP-scale fixture.  R / fmax chosen so L_max spans
/// 900 / 72 / 50 / 2.5 — same order as the production system.
struct LineFixture
{
  double R;
  double fmax;
};

constexpr std::array<LineFixture, 4> kLines = {{
    {
        .R = 0.0001,
        .fmax = 3000.0,
    },  // L_max = 900
    {
        .R = 0.0008,
        .fmax = 300.0,
    },  // L_max = 72
    {
        .R = 0.005,
        .fmax = 100.0,
    },  // L_max = 50
    {
        .R = 0.025,
        .fmax = 10.0,
    },  // L_max = 2.5
}};

constexpr double kIntegrationV = 100.0;  // kV → V² = 10,000

}  // namespace
}  // namespace adaptive_integration_test_ns

using namespace adaptive_integration_test_ns;

TEST_CASE(
    "line_losses adaptive rule - per-line LP error ≤ predicted budget on "
    "4-line mix")  // NOLINT
{
  // Build parallel R / fmax vectors for the rule.
  std::vector<double> R_vec;
  std::vector<double> fmax_vec;
  std::vector<double> L_max;
  R_vec.reserve(kLines.size());
  fmax_vec.reserve(kLines.size());
  L_max.reserve(kLines.size());
  for (const auto& ln : kLines) {
    R_vec.push_back(ln.R);
    fmax_vec.push_back(ln.fmax);
    L_max.push_back(ln.R * ln.fmax * ln.fmax);
  }
  const double L_total = std::accumulate(L_max.begin(), L_max.end(), 0.0);

  // Flow fractions sampled per line.  Skip 0 (trivially zero loss).
  const std::vector<double> demand_fracs = {0.25, 0.5, 0.75, 0.95};

  for (const double err_pct : {0.005, 0.01, 0.05}) {
    const line_losses::AdaptiveSegmentsOpts rule_opts {
        .err_pct = err_pct,
        .floor = 2,
        .ceiling = 6,
    };
    const auto K_vec = line_losses::compute_adaptive_loss_segments(
        std::span<const double> {R_vec},
        std::span<const double> {fmax_vec},
        rule_opts);
    REQUIRE(K_vec.size() == kLines.size());

    // Per-line: walk a handful of flow points using the rule's K_i and
    // collect the max realised secant error.
    double system_realised = 0.0;
    double system_predicted = 0.0;
    for (std::size_t i = 0; i < kLines.size(); ++i) {
      const auto& ln = kLines[i];
      const int K_i = K_vec[i];
      // The rule never returns K < floor=2 for lossy lines.
      REQUIRE(K_i >= 2);
      REQUIRE(K_i <= 6);

      // Predicted upper bound on this line's PWL secant error (in MW):
      // worst chord error = R·(width/2)² = R·(fmax/(2K))² = L_max/(4K²).
      const double predicted_bound = L_max[i]
          / (4.0 * static_cast<double>(K_i) * static_cast<double>(K_i));

      double max_realised = 0.0;
      const double loss_coef = ln.R / (kIntegrationV * kIntegrationV);
      for (const double frac : demand_fracs) {
        const double demand = frac * ln.fmax;
        const double loss_lp = solve_line(ln.R,
                                          kIntegrationV,
                                          ln.fmax,
                                          Direction::AtoB,
                                          demand,
                                          "piecewise",
                                          "uniform",
                                          K_i,
                                          /*enforce_level=*/2);
        const double f = demand + loss_lp;
        const double loss_analytic = loss_coef * f * f;
        const double realised_err = std::abs(loss_lp - loss_analytic);

        CAPTURE(err_pct);
        CAPTURE(i);
        CAPTURE(K_i);
        CAPTURE(frac);
        CAPTURE(demand);
        CAPTURE(f);
        CAPTURE(loss_lp);
        CAPTURE(loss_analytic);
        CAPTURE(realised_err);
        CAPTURE(predicted_bound);

        // Per-flow-point invariant: realised secant error must stay
        // inside the rule's analytic bound, with 1.5× slack for integer
        // K rounding + LP scaling noise.  The (fmax/(2K))² formula is a
        // TIGHT envelope on the uniform PWL — actual error is normally
        // 30-70 % of it.
        CHECK(realised_err <= 1.5 * predicted_bound + 1e-6);

        max_realised = std::max(max_realised, realised_err);
      }

      system_realised += max_realised;
      system_predicted += predicted_bound;
    }

    // System-wide check: the SUM of per-line max errors should respect
    // the rule's contract.  Two regimes:
    //   * **Unclamped** (err_pct loose enough that ALL raw KKT K_i fall
    //     inside [floor, ceiling]): the rule meets the budget exactly,
    //     ``Σ L_i/(4 K_i²) ≤ err_pct · L_total`` (up to integer-ceiling
    //     slack from rounding K_i UP).
    //   * **Clamped** (err_pct so tight the heaviest lines saturate
    //     ``ceiling``): the predicted total can EXCEED the budget; the
    //     loosest theoretical floor is ``L_total / (4·ceiling²)``
    //     (everyone at ceiling) but realistic mixes (heavy at ceiling,
    //     tiny at floor) sit BETWEEN clamp_floor and the worst-case
    //     ``L_total / (4·floor·ceiling)`` upper envelope.
    const double budget = err_pct * L_total;
    // Worst-case predicted under the rule: heaviest line at ceiling
    // contributes L_max/(4·ceiling²); a tiny line capped at floor
    // contributes its share at (4·floor²).  An envelope that holds for
    // any [floor, ceiling]-bounded allocation is
    // ``L_total / (4·floor·ceiling)`` — every K_i ∈ [floor, ceiling] ⇒
    // K_i² ≥ floor · ceiling ⇒ Σ L_i/(4 K_i²) ≤ L_total/(4·floor·ceiling).
    constexpr int k_floor = 2;
    constexpr int k_ceiling = 6;
    const double worst_envelope =
        L_total / (4.0 * static_cast<double>(k_floor * k_ceiling));
    const double effective_upper = std::max(budget, worst_envelope);

    CAPTURE(err_pct);
    CAPTURE(L_total);
    CAPTURE(budget);
    CAPTURE(worst_envelope);
    CAPTURE(system_realised);
    CAPTURE(system_predicted);
    // (1) Realised LP error ≤ predicted analytic bound (×1.5 slack).
    CHECK(system_realised <= 1.5 * system_predicted + 1e-6);
    // (2) Predicted bound sits in the rule's envelope.  Loose ×1.05 on
    // top because the rule rounds K_i UP and may pick a smaller-than-
    // optimal K when raw KKT lands just above an integer.
    CHECK(system_predicted <= effective_upper * 1.05 + 1e-6);
  }
}
